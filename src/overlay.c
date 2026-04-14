#include "overlay.h"
#include "mount.h"    // setup_rootfs(), mount_proc()
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <ftw.h>
#include <dirent.h>    // opendir/readdir for fd table audit 
#include <sys/resource.h> // getrlimit for fd fallback range (§3.5)
#include <fcntl.h>        // open(), O_RDONLY — DEBUG TEST (remove with test block)

#define STACK_SIZE (1024 * 1024)

// Phase 3 correction: extern char **environ is no longer needed here.
// The child no longer falls back to the host environment.
// build_container_env() in main.c constructs the environment, and it's
// passed through config->envp → child_args.envp → execve().

typedef struct {
    const char *program;
    char *const *argv;
    char *const *envp;
    bool enable_debug;
    const char *rootfs_path;  // Either merged_path (overlay) or raw rootfs
} child_args_t;

/**
 * Generate a 12-character hex container ID.
 */
static void generate_container_id(char *id){
    
    // This will generate randomness in the id
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    unsigned long hash = ts.tv_sec ^ ts.tv_nsec ^ getpid();
    snprintf(id, 13, "%012lx", hash & 0xFFFFFFFFFFFF);
}

/**
 * Callback for nftw() to remove directory tree.
 */
static int remove_cb(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb;
    (void)typeflag;
    (void)ftwbuf;

    int rv = remove(path);
    if (rv) {
        perror(path);
    }
    return rv;
}

/**
 * Recursively remove a directory tree.
 */
static int remove_directory(const char *path) {
    return nftw(path, remove_cb, 64, FTW_DEPTH | FTW_PHYS);
}

/**
 * Initialize overlay paths in context.
 *
 * Generates a container ID, resolves absolute paths, and populates
 * the upper/work/merged path fields in ctx.
 *
 * @param ctx            Overlay context to populate
 * @param rootfs_path    Path to base image (lowerdir)
 * @param container_dir  Parent directory for overlay data (NULL = "./containers")
 * @param enable_debug   Print resolved paths
 * @return               0 on success, -1 on failure
 */
static int init_overlay_paths(overlay_context_t *ctx, const char *rootfs_path,
                              const char *container_dir, bool enable_debug) {
    // Generate container ID
    generate_container_id(ctx->container_id);

    // Resolve rootfs to absolute path
    if (!realpath(rootfs_path, ctx->lower_path)) {
        perror("realpath(rootfs)");
        return -1;
    }

    // Default container directory
    if (!container_dir) {
        container_dir = "./containers";
    }

    // Ensure container parent directory exists
    if (mkdir(container_dir, 0755) < 0 && errno != EEXIST) {
        perror("mkdir(container_dir)");
        return -1;
    }

    // Resolve container_dir to absolute path for overlay mount options
    char abs_container_dir[PATH_MAX];
    if (!realpath(container_dir, abs_container_dir)) {
        perror("realpath(container_dir)");
        return -1;
    }

    // Build paths for this container's overlay directories
    // Each snprintf return value is checked against PATH_MAX to detect
    // truncation — a silently truncated path would cause mkdir/mount to
    // operate on the wrong location.
    if (snprintf(ctx->container_base, PATH_MAX, "%s/%s",
                 abs_container_dir, ctx->container_id) >= PATH_MAX) {
        fprintf(stderr, "init_overlay_paths: container_base path truncated\n");
        return -1;
    }

    if (snprintf(ctx->upper_path, PATH_MAX, "%s/upper", ctx->container_base) >= PATH_MAX) {
        fprintf(stderr, "init_overlay_paths: upper_path truncated\n");
        return -1;
    }

    if (snprintf(ctx->work_path, PATH_MAX, "%s/work", ctx->container_base) >= PATH_MAX) {
        fprintf(stderr, "init_overlay_paths: work_path truncated\n");
        return -1;
    }

    if (snprintf(ctx->merged_path, PATH_MAX, "%s/merged", ctx->container_base) >= PATH_MAX) {
        fprintf(stderr, "init_overlay_paths: merged_path truncated\n");
        return -1;
    }

    if (enable_debug) {
        printf("[overlay] Container ID: %s\n", ctx->container_id);
        printf("[overlay] Lower: %s\n", ctx->lower_path);
        printf("[overlay] Upper: %s\n", ctx->upper_path);
        printf("[overlay] Work:  %s\n", ctx->work_path);
        printf("[overlay] Merged: %s\n", ctx->merged_path);
    }

    return 0;
}

/**
 * Create the overlay directory structure.
 *
 * Creates container_base/, upper/, work/, and merged/ directories.
 * Cleans up partial creation on failure.
 *
 * @param ctx  Overlay context with paths already initialized
 * @return     0 on success, -1 on failure
 */
static int create_overlay_dirs(overlay_context_t *ctx) {
    if (mkdir(ctx->container_base, 0755) < 0) {
        perror("mkdir(container_base)");
        return -1;
    }

    if (mkdir(ctx->upper_path, 0755) < 0) {
        perror("mkdir(upper)");
        goto cleanup_base;
    }

    if (mkdir(ctx->work_path, 0755) < 0) {
        perror("mkdir(work)");
        goto cleanup_upper;
    }

    if (mkdir(ctx->merged_path, 0755) < 0) {
        perror("mkdir(merged)");
        goto cleanup_work;
    }

    return 0;

cleanup_work:
    rmdir(ctx->work_path);
cleanup_upper:
    rmdir(ctx->upper_path);
cleanup_base:
    rmdir(ctx->container_base);
    return -1;
}

/**
 * Mount the overlay filesystem.
 *
 * Builds the mount options string and calls mount(2) with
 * MS_NODEV | MS_NOSUID (§3.6).
 *
 * @param ctx           Overlay context with paths initialized and dirs created
 * @param enable_debug  Print mount options
 * @return              0 on success, -1 on failure
 */
static int mount_overlay(overlay_context_t *ctx, bool enable_debug) {
    // Build mount options string
    char mount_opts[PATH_MAX * 4];
    snprintf(mount_opts, sizeof(mount_opts),
             "lowerdir=%s,upperdir=%s,workdir=%s",
             ctx->lower_path, ctx->upper_path, ctx->work_path);

    if (enable_debug) {
        printf("[overlay] Mount options: %s\n", mount_opts);
    }

    // Mount overlay — MS_NODEV | MS_NOSUID blocks device nodes and
    // setuid escalation inside the container (see §3.6)
    if (mount("overlay", ctx->merged_path, "overlay",
              MS_NODEV | MS_NOSUID, mount_opts) < 0) {
        perror("mount(overlay)");
        return -1;
    }

    ctx->is_mounted = true;

    if (enable_debug) {
        printf("[overlay] Overlay mounted at %s\n", ctx->merged_path);
    }

    return 0;
}

/**
 * Setup overlay filesystem.
 *
 * Public wrapper that initializes paths, creates directories, and mounts
 * overlayfs. On failure, partially created state is cleaned up by each
 * sub-function independently.
 */
int setup_overlay(overlay_context_t *ctx, const char *rootfs_path,
                  const char *container_dir, bool enable_debug) {
    if (!ctx || !rootfs_path) {
        fprintf(stderr, "setup_overlay: invalid arguments\n");
        return -1;
    }

    if (init_overlay_paths(ctx, rootfs_path, container_dir, enable_debug) < 0) {
        return -1;
    }

    if (create_overlay_dirs(ctx) < 0) {
        return -1;
    }

    if (mount_overlay(ctx, enable_debug) < 0) {
        rmdir(ctx->merged_path);
        rmdir(ctx->work_path);
        rmdir(ctx->upper_path);
        rmdir(ctx->container_base);
        return -1;
    }

    return 0;
}

/**
 * Teardown overlay filesystem.
 * Unmounts overlay and removes all container directories.
 */
int teardown_overlay(overlay_context_t *ctx, bool enable_debug){
    
    if(!ctx){
        return -1;
    }

    int ret = 0;

    // Unmount the overlay
    if(ctx->is_mounted){
        if(enable_debug){
            printf("[overlay] Unmounting: %s\n", ctx->merged_path);
        }

        if(umount2(ctx->merged_path, MNT_DETACH) < 0){
            perror("umount2(overlay)");
            ret = -1;
        }else{
            ctx->is_mounted = false;
        }
    }

    // Remove upper directory tree (contains container's writes)
    if(ctx->upper_path[0]){
        if(enable_debug){
            printf("[overlay] Removing upper layer: %s\n", ctx->upper_path);
        }
        remove_directory(ctx->upper_path);
    }

    // Remove work directory tree (kernel bookkeeping)
    if(ctx->work_path[0]){
        if(enable_debug){
            printf("[overlay] Removing work dir: %s\n", ctx->work_path);
        }
        remove_directory(ctx->work_path);
    }

    // Remove merged directory (mount point)
    if(ctx->merged_path[0]){
        if(enable_debug){
            printf("[overlay] Removing merged dir: %s\n", ctx->merged_path);
        }
        rmdir(ctx->merged_path);
    }

    // Remove container base directory
    if(ctx->container_base[0]){
        if(enable_debug){
            printf("[overlay] Removing container base: %s\n", ctx->container_base);
        }
        rmdir(ctx->container_base);
    }

    if (enable_debug) {
        printf("[overlay] Cleanup complete\n");
    }

    return ret;
}

/**
 * Close all file descriptors above STDERR_FILENO.
 *
 * After clone(), the child inherits every open fd from the parent.
 * Any fd referencing the host filesystem survives pivot_root + umount
 * and can be used to escape the mount namespace (CVE-2024-21626,
 * CVE-2016-9962).
 *
 * We iterate /proc/self/fd rather than brute-forcing close(3..MAX)
 * because the fd range can be enormous (ulimit -n defaults to 1024
 * but can be set to millions) while the actual open fd count is small.
 */
static void close_inherited_fds(bool enable_debug){
    DIR *dir = opendir("/proc/self/fd");
    
    // Use a fallback if /proc/self/fd is not available. This should never
    // happen in practice
    if(!dir){
        // /proc may not be mounted yet — fall back to rlimit range
        struct rlimit rl;
        int max_fd = 1024; // Double safety incase getrlimit fails
        
        if(getrlimit(RLIMIT_NOFILE, &rl) == 0){
            max_fd = (int)rl.rlim_cur;
        }
        if(enable_debug){
            printf("[child] /proc/self/fd not available, closing fds 3-%d\n", max_fd);
        }

        for(int fd = 3; fd < max_fd; fd++){
            close(fd);
        }
        
        return;
    }

    //If you made it here, /proc/self/fd is available, no fallback was used
    int dir_fd = dirfd(dir);

    struct dirent *entry;
    while((entry = readdir(dir)) != NULL){
        if(entry->d_name[0] == '.') continue;

        int fd = atoi(entry->d_name);

        // Keep stdin/stdout/stderr and the dirfd we're iterating
        if(fd <= STDERR_FILENO || fd == dir_fd) continue;

        if(enable_debug){
            printf("[child] Closing inherited fd %d\n", fd);
        }

        close(fd);
    }

    closedir(dir);
}

/**
 * Child function for containerized process.
 */
static int child_func(void *arg){
    child_args_t *args = (child_args_t *)arg;

    if(args->enable_debug){
        printf("[child] PID: %d\n", getpid());
    }

    // Setup rootfs if requested
    // rootfs_path points to merged/ when overlay is enabled
    // or to raw rootfs when overlay is disabled 
    if(args->rootfs_path){
        if(setup_rootfs(args->rootfs_path, args->enable_debug) < 0){
            fprintf(stderr, "[child] Failed to setup rootfs\n");
            return 1;
        }

        // Mount /proc after pivot_root
        if(mount_proc(args->enable_debug) < 0){
            fprintf(stderr, "[child] Failed to mount /proc\n");
            return 1;
        }
    }

    // Phase 3 correction §3.5: close all inherited fds before exec.
    // After clone(), the child has copies of every parent fd. Any fd
    // referencing the host filesystem survives pivot_root + MNT_DETACH
    // and would let the container escape the mount namespace.
    // Must be called AFTER mount_proc() so /proc/self/fd is available.
    close_inherited_fds(args->enable_debug);

    // Execute target program
    // Phase 3 correction §3.4: args->envp is always set to a
    // container-appropriate environment built by build_container_env().
    // We no longer fall back to the host's environ.
    execve(args->program, args->argv, args->envp);

    // execve only returns on error
    perror("execve");
    return 127;
}

overlay_result_t overlay_exec(const overlay_config_t *config){
    overlay_result_t result = {0};
    overlay_context_t overlay_ctx = {0};
    bool overlay_active = false;

    // Validate
    if (!config || !config->program || !config->argv) {
        fprintf(stderr, "overlay_exec: invalid config\n");
        result.child_pid = -1;
        return result;
    }

    if (config->enable_debug) {
        printf("[parent] Executing:");
        for (int i = 0; config->argv[i]; i++) {
            printf(" %s", config->argv[i]);
        }
        printf("\n");

        if (config->rootfs_path) {
            printf("[parent] Rootfs: %s\n", config->rootfs_path);
        }
        if (config->enable_overlay) {
            printf("[parent] Overlay: enabled\n");
        }
    }

    // Determine the effective rootfs path
    const char *effective_rootfs = config->rootfs_path;

    // Setup overlat if requested
    if(config->enable_overlay && config->rootfs_path){
        if(setup_overlay(&overlay_ctx, config->rootfs_path, config->container_dir, config->enable_debug) < 0){
            fprintf(stderr, "[parent] Failed to setup overlay\n");
            result.child_pid = -1;
            return result;
        }

        overlay_active = true;
        effective_rootfs = overlay_ctx.merged_path;

        if(config->enable_debug){
            printf("[parent] Using merged rootfs: %s\n", effective_rootfs);
        }
    }

    // Allocate stack
    char *stack = malloc(STACK_SIZE);
    if(!stack){
        perror("malloc");
        if(overlay_active){
            teardown_overlay(&overlay_ctx, config->enable_debug);
        }
        result.child_pid = -1;
        return result;
    }

    result.stack_ptr = stack;

    // Prepare child args
    child_args_t child_args = {
        .program = config->program,
        .argv = config->argv,
        .envp = config->envp,
        .enable_debug = config->enable_debug,
        .rootfs_path = effective_rootfs
    };

    // Build flags
    int flags = SIGCHLD;
    if (config->enable_pid_namespace) {
        flags |= CLONE_NEWPID;
    }
    if (config->enable_mount_namespace) {
        flags |= CLONE_NEWNS;
    }

    if (config->enable_debug && (flags & CLONE_NEWNS)) {
        printf("[parent] Creating mount namespace\n");
    }

    /* ================================================================
     * DEBUG TEST: Simulate leaked parent file descriptors
     *
     * These fds simulate the kind of internal bookkeeping that a
     * production runtime would have open at clone() time — config
     * files, log files, overlay metadata, etc. They will be inherited
     * by the child via clone() and should appear in the debug output
     * as "[child] Closing inherited fd N" if close_inherited_fds()
     * is working correctly.
     *
     * Remove this block once fd cleanup has been verified.
     * ================================================================ */
    int test_fd_1 = open("/etc/hostname", O_RDONLY);
    int test_fd_2 = open("/etc/os-release", O_RDONLY);
    int test_fd_3 = open("/etc/passwd", O_RDONLY);
    if (config->enable_debug) {
        printf("[parent] TEST: Opened simulated leaked fds: %d, %d, %d\n",
               test_fd_1, test_fd_2, test_fd_3);
    }
    /* ============== END DEBUG TEST ================================ */

    // Clone
    pid_t pid = clone(child_func, stack + STACK_SIZE, flags, &child_args);

    /* ================================================================
     * DEBUG TEST: Close simulated leaked fds in the parent.
     * The child already has its own copies via clone().
     * ============== END DEBUG TEST ================================ */
    if (test_fd_1 >= 0) close(test_fd_1);
    if (test_fd_2 >= 0) close(test_fd_2);
    if (test_fd_3 >= 0) close(test_fd_3);

    if (pid < 0) {
        perror("clone");
        free(stack);
        if (overlay_active) {
            teardown_overlay(&overlay_ctx, config->enable_debug);
        }
        result.child_pid = -1;
        result.stack_ptr = NULL;
        return result;
    }

    result.child_pid = pid;

    if (config->enable_debug) {
        printf("[parent] Child PID: %d\n", pid);
    }

    // Wait for child
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        result.exit_status = -1;
        if (overlay_active) {
            teardown_overlay(&overlay_ctx, config->enable_debug);
        }
        return result;
    }

    // Parse status
    if (WIFEXITED(status)) {
        result.exited_normally = true;
        result.exit_status = WEXITSTATUS(status);

        if (config->enable_debug) {
            printf("[parent] Child exited: %d\n", result.exit_status);
        }
    } else if (WIFSIGNALED(status)) {
        result.exited_normally = false;
        result.signal = WTERMSIG(status);
        result.exit_status = 128 + result.signal;

        if (config->enable_debug) {
            printf("[parent] Child killed by signal: %d\n", result.signal);
        }
    }

    // Teardown overlay after container exits
    if (overlay_active) {
        teardown_overlay(&overlay_ctx, config->enable_debug);
    }

    return result;    
}

void overlay_cleanup(overlay_result_t *result) {
    if (result && result->stack_ptr) {
        free(result->stack_ptr);
        result->stack_ptr = NULL;
    }
}