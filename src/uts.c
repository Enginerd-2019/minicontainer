#include "uts.h"
#include "overlay.h"  // setup_overlay(), teardown_overlay()
#include "mount.h"    // setup_rootfs(), mount_proc()
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>         // open(), O_WRONLY for /proc/<pid>/{uid_map,gid_map,setgroups}
#include <dirent.h>        // opendir/readdir for fd cleanup (Phase 3 §3.5)
#include <sys/resource.h>  // getrlimit for fd fallback range (Phase 3 §3.5)

#define STACK_SIZE (1024 * 1024)

// Phase 3 correction carried forward: no extern char **environ.
// Container environment is constructed by build_container_env() and
// passed through config->envp → child_args.envp → execve().
// hostname and and pipe syncronizer are also added in phase 4/4b
typedef struct {
    const char *program;
    char *const *argv;
    char *const *envp;
    bool enable_debug;
    const char *rootfs_path;
    const char *hostname;
    int sync_fd;                // New: read end of sync pipe (-1 if no user ns)
    bool user_namespace_active; // True if CLONE_NEWUSER was used
} child_args_t;

/**
 * Setup hostname inside UTS namespace.
 */
int setup_uts(const char *hostname, bool enable_debug) {
    if (hostname) {
        if (sethostname(hostname, strlen(hostname)) < 0) {
            perror("sethostname");
            return -1;
        }

        if (enable_debug) {
            printf("[child] Set hostname: %s\n", hostname);
        }
    }

    return 0;
}

/**
 * Setup UID/GID mapping for user namespace.
 *
 * Called from the PARENT process after clone(). Writes to
 * /proc/<child_pid>/setgroups, uid_map, and gid_map.
 */
int setup_user_namespace_mapping(pid_t child_pid, const uts_config_t *config) {
    char path[256];
    char mapping[256];
    int fd;

    // Step 1: Disable setgroups (required before gid_map for unprivileged users)
    snprintf(path, sizeof(path), "/proc/%d/setgroups", child_pid);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("open(setgroups)");
        return -1;
    }
    if (write(fd, "deny", 4) < 0) {
        perror("write(setgroups)");
        close(fd);
        return -1;
    }
    close(fd);

    if (config->enable_debug) {
        printf("[parent] Disabled setgroups for PID %d\n", child_pid);
    }

    // Step 2: Write UID map
    snprintf(path, sizeof(path), "/proc/%d/uid_map", child_pid);
    snprintf(mapping, sizeof(mapping), "%u %u %zu",
             config->uid_map_inside, config->uid_map_outside,
             config->uid_map_range);

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("open(uid_map)");
        return -1;
    }
    if (write(fd, mapping, strlen(mapping)) < 0) {
        perror("write(uid_map)");
        close(fd);
        return -1;
    }
    close(fd);

    if (config->enable_debug) {
        printf("[parent] UID map: %s\n", mapping);
    }

    // Step 3: Write GID map
    snprintf(path, sizeof(path), "/proc/%d/gid_map", child_pid);
    snprintf(mapping, sizeof(mapping), "%u %u %zu",
             config->gid_map_inside, config->gid_map_outside,
             config->gid_map_range);

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("open(gid_map)");
        return -1;
    }
    if (write(fd, mapping, strlen(mapping)) < 0) {
        perror("write(gid_map)");
        close(fd);
        return -1;
    }
    close(fd);

    if (config->enable_debug) {
        printf("[parent] GID map: %s\n", mapping);
    }

    return 0;
}


/**
 * Close all file descriptors above STDERR_FILENO.
 *
 * Phase 3 correction §3.5, carried forward: after clone(), the child
 * inherits every open fd from the parent. Any fd referencing the host
 * filesystem survives pivot_root + umount and can be used to escape
 * the mount namespace (CVE-2024-21626, CVE-2016-9962). This isn't that
 * much of a concern, given that we learned in phase 3 that only fd < 3
 * survive sudo
 * 
 * We iterate /proc/self/fd rather than brute-forcing close(3..MAX)
 * because the fd range can be enormous while the actual open fd count
 * is small.
 */
static void close_inherited_fds(bool enable_debug) {
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) {
        // /proc may not be mounted yet — fall back to rlimit range
        struct rlimit rl;
        int max_fd = 1024;  // safe default if getrlimit fails
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
            max_fd = (int)rl.rlim_cur;
        }
        if (enable_debug) {
            printf("[child] /proc/self/fd not available, closing fds 3-%d\n",
                   max_fd);
        }
        for (int fd = 3; fd < max_fd; fd++) {
            close(fd);
        }
        return;
    }

    int dir_fd = dirfd(dir);

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        int fd = atoi(entry->d_name);

        // Keep stdin/stdout/stderr and the dirfd we're iterating
        if (fd <= STDERR_FILENO || fd == dir_fd) continue;

        if (enable_debug) {
            printf("[child] Closing inherited fd %d\n", fd);
        }
        close(fd);
    }
    closedir(dir);
}

/**
 * Child function for containerized process.
 *
 * If a user namespace is active (sync_fd >= 0), the child blocks on the
 * sync pipe until the parent has finished writing UID/GID maps.
 */
static int child_func(void *arg) {
    child_args_t *args = (child_args_t *)arg;

    // If user namespace is active, wait for parent to write UID/GID maps.
    // The parent will write a single byte after maps are in place.
    if (args->sync_fd >= 0) {
        char buf;
        if (args->enable_debug) {
            printf("[child] Waiting for UID/GID mapping...\n");
        }

        ssize_t n = read(args->sync_fd, &buf, 1);
        close(args->sync_fd);

        if (n < 1) {
            fprintf(stderr, "[child] Sync pipe read failed\n");
            return 1;
        }

        if (args->enable_debug) {
            printf("[child] UID/GID mapping complete, proceeding\n");
        }
    }

    if (args->enable_debug) {
        printf("[child] PID: %d, UID: %d, GID: %d\n",
               getpid(), getuid(), getgid());
    }

    // Set hostname (must be in UTS namespace, before rootfs setup
    // so debug output shows the hostname change clearly)
    if (args->hostname) {
        if (setup_uts(args->hostname, args->enable_debug) < 0) {
            fprintf(stderr, "[child] Failed to setup UTS\n");
            return 1;
        }
    }

    // Setup rootfs if requested (carried forward from Phase 2-3)
    // rootfs_path points to merged/ when overlay is enabled,
    // or to raw rootfs/ when overlay is disabled
    if (args->rootfs_path) {
        if (setup_rootfs(args->rootfs_path, args->enable_debug) < 0) {
            fprintf(stderr, "[child] Failed to setup rootfs\n");
            return 1;
        }

        // Mount /proc after pivot_root.
        // In a user namespace, the kernel may deny proc mounts due to
        // additional restrictions (AppArmor, locked superblock flags).
        // Downgrade to a warning — the container can function without
        // /proc, and close_inherited_fds() has a brute-force fallback.
        if (mount_proc(args->enable_debug) < 0) {
            if (args->user_namespace_active) {
                fprintf(stderr,
                    "[child] Warning: /proc mount denied (user namespace restriction) "
                    "— continuing without /proc\n");
            } else {
                fprintf(stderr, "[child] Failed to mount /proc\n");
                return 1;
            }
        }
    }

    // Phase 3 correction §3.5: close all inherited fds before exec.
    // After clone(), the child has copies of every parent fd. Any fd
    // referencing the host filesystem survives pivot_root + MNT_DETACH
    // and would let the container escape the mount namespace.
    // Must be called AFTER mount_proc() so /proc/self/fd is available.
    close_inherited_fds(args->enable_debug);

    // Execute target program
    // Phase 3 correction: args->envp is always set via build_container_env().
    // No fallback to host environ.
    execve(args->program, args->argv, args->envp);

    perror("execve");
    return 127;
}

uts_result_t uts_exec(const uts_config_t *config) {
    uts_result_t result = {0};
    overlay_context_t overlay_ctx = {0};
    bool overlay_active = false;
    int sync_pipe[2] = {-1, -1};

    // Validate
    if (!config || !config->program || !config->argv) {
        fprintf(stderr, "uts_exec: invalid config\n");
        result.child_pid = -1;
        return result;
    }

    if (config->enable_debug) {
        printf("[parent] Executing: %s", config->program);
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

    // Create sync pipe for user namespace (parent signals child after
    // writing UID/GID maps)
    if (config->enable_user_namespace) {
        if (pipe(sync_pipe) < 0) {
            perror("pipe");
            result.child_pid = -1;
            return result;
        }
    }

    // Determine the effective rootfs path
    const char *effective_rootfs = config->rootfs_path;

    // Setup overlay if requested (Phase 3 functionality, carried forward)
    if (config->enable_overlay && config->rootfs_path) {
        if (setup_overlay(&overlay_ctx, config->rootfs_path,
                          config->container_dir, config->enable_debug) < 0) {
            fprintf(stderr, "[parent] Failed to setup overlay\n");
            if (sync_pipe[0] >= 0) { close(sync_pipe[0]); close(sync_pipe[1]); }
            result.child_pid = -1;
            return result;
        }
        overlay_active = true;
        effective_rootfs = overlay_ctx.merged_path;

        if (config->enable_debug) {
            printf("[parent] Using merged rootfs: %s\n", effective_rootfs);
        }
    }

    // Allocate stack
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc");
        if (overlay_active) {
            teardown_overlay(&overlay_ctx, config->enable_debug);
        }
        if (sync_pipe[0] >= 0) { close(sync_pipe[0]); close(sync_pipe[1]); }
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
        .rootfs_path = effective_rootfs,
        .hostname = config->hostname,
        .sync_fd = sync_pipe[0],    // Child gets read end (-1 if no user ns)
        .user_namespace_active = config->enable_user_namespace
    };

    // Build flags
    int flags = SIGCHLD;
    if (config->enable_user_namespace) {
        // CLONE_NEWUSER must be included in the same clone() call.
        // The kernel creates the user namespace first, then uses its
        // capabilities to create other namespaces.
        flags |= CLONE_NEWUSER;

        if (config->enable_debug) {
            printf("[parent] Creating user namespace\n");
        }
    }
    if (config->enable_pid_namespace) {
        flags |= CLONE_NEWPID;
    }
    if (config->enable_mount_namespace) {
        flags |= CLONE_NEWNS;
    }
    if (config->enable_uts_namespace) {
        flags |= CLONE_NEWUTS;

        if (config->enable_debug) {
            printf("[parent] Creating UTS namespace\n");
        }
    }

    // Note: Phase 3 included a DEBUG TEST block here that opened simulated
    // leaked fds to verify close_inherited_fds(). That block has been removed
    // now that fd cleanup has been verified. close_inherited_fds() remains
    // in child_func() as the permanent fix for CVE-2024-21626/CVE-2016-9962.

    // Clone
    pid_t pid = clone(child_func, stack + STACK_SIZE, flags, &child_args);

    if (pid < 0) {
        perror("clone");
        free(stack);
        if (overlay_active) {
            teardown_overlay(&overlay_ctx, config->enable_debug);
        }
        if (sync_pipe[0] >= 0) { close(sync_pipe[0]); close(sync_pipe[1]); }
        result.child_pid = -1;
        result.stack_ptr = NULL;
        return result;
    }

    result.child_pid = pid;

    if (config->enable_debug) {
        printf("[parent] Child PID: %d\n", pid);
    }

    // Close read end in parent (child owns it now)
    if (sync_pipe[0] >= 0) {
        close(sync_pipe[0]);
        sync_pipe[0] = -1;
    }

    // Setup user namespace mapping and signal child
    if (config->enable_user_namespace) {
        if (setup_user_namespace_mapping(pid, config) < 0) {
            fprintf(stderr, "[parent] Failed to setup user namespace mapping\n");
            // Signal child to exit (close pipe without writing)
            close(sync_pipe[1]);
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            free(stack);
            if (overlay_active) {
                teardown_overlay(&overlay_ctx, config->enable_debug);
            }
            result.child_pid = -1;
            result.stack_ptr = NULL;
            return result;
        }

        // Signal child: mappings are ready, you can proceed
        if (write(sync_pipe[1], "1", 1) < 0) {
            perror("write(sync_pipe)");
        }
        close(sync_pipe[1]);
        sync_pipe[1] = -1;

        if (config->enable_debug) {
            printf("[parent] Signaled child to proceed\n");
        }
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

    // Teardown overlay after container exits (Phase 3 functionality)
    if (overlay_active) {
        teardown_overlay(&overlay_ctx, config->enable_debug);
    }

    return result;
}

void uts_cleanup(uts_result_t *result) {
    if (result && result->stack_ptr) {
        free(result->stack_ptr);
        result->stack_ptr = NULL;
    }
}