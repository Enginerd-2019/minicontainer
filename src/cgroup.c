// Note: _GNU_SOURCE is provided by the Makefile via -D_GNU_SOURCE.
// Do NOT redefine it here (Error #8 from decisions.md).
#include "cgroup.h"
#include "uts.h"      // setup_uts(), setup_user_namespace_mapping()
#include "overlay.h"  // setup_overlay(), teardown_overlay()
#include "mount.h"    // setup_rootfs(), mount_proc()
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <dirent.h>        // opendir/readdir for fd cleanup (Phase 3 §3.5)
#include <sys/resource.h>  // getrlimit for fd fallback range (Phase 3 §3.5)

#define STACK_SIZE (1024 * 1024)
#define CGROUP_ROOT "/sys/fs/cgroup"
#define CGROUP_PREFIX "minicontainer"

// Phase 3 correction carried forward: no extern char **environ.
// Container environment is constructed by build_container_env() and
// passed through config->envp → child_args.envp → execve().

typedef struct {
    const char *program;
    char *const *argv;
    char *const *envp;
    bool enable_debug;
    const char *rootfs_path;    // merged/ (overlay) or raw rootfs
    const char *hostname;
    int sync_fd;                // Read end of sync pipe. -1 if no user namespace.
    bool user_namespace_active; // Phase 4b: for /proc graceful degradation
} child_args_t;

/**
 * Generate unique cgroup name using timestamp + nanoseconds.
 */
static void generate_cgroup_name(char *name, size_t size) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(name, size, "%s_%ld_%ld", CGROUP_PREFIX, ts.tv_sec, ts.tv_nsec);
}

/**
 * Write string to a cgroup file.
 * Returns 0 on success, -1 on failure.
 */
static int write_cgroup_file(const char *path, const char *content,
                             bool enable_debug) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        if (enable_debug) {
            fprintf(stderr, "[cgroup] Failed to open %s: %s\n",
                    path, strerror(errno));
        }
        return -1;
    }

    ssize_t len = strlen(content);
    if (write(fd, content, len) != len) {
        if (enable_debug) {
            fprintf(stderr, "[cgroup] Failed to write to %s: %s\n",
                    path, strerror(errno));
        }
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

/**
 * Create and setup cgroup.
 *
 * Steps:
 * 1. Generate unique name
 * 2. Create directory in /sys/fs/cgroup/
 * 3. Enable controllers in parent (may fail if already enabled — that's fine)
 * 4. Write limits to memory.max, cpu.max, pids.max
 */
int setup_cgroup(cgroup_context_t *ctx, const cgroup_limits_t *limits,
                 bool enable_debug) {
    // Generate unique name
    generate_cgroup_name(ctx->cgroup_name, sizeof(ctx->cgroup_name));
    snprintf(ctx->cgroup_path, sizeof(ctx->cgroup_path),
             "%s/%s", CGROUP_ROOT, ctx->cgroup_name);

    if (enable_debug) {
        printf("[cgroup] Creating cgroup: %s\n", ctx->cgroup_path);
    }

    // Create cgroup directory
    if (mkdir(ctx->cgroup_path, 0755) < 0) {
        perror("mkdir(cgroup)");
        return -1;
    }
    ctx->created = true;

    // Enable controllers in root cgroup (may fail if already enabled)
    char subtree_path[512];
    snprintf(subtree_path, sizeof(subtree_path),
             "%s/cgroup.subtree_control", CGROUP_ROOT);
    write_cgroup_file(subtree_path, "+cpu +memory +pids", enable_debug);
    // Ignore errors — controllers might already be enabled

    // Set memory limit
    if (limits->memory_limit > 0) {
        char path[512];
        char content[64];

        snprintf(path, sizeof(path), "%s/memory.max", ctx->cgroup_path);
        snprintf(content, sizeof(content), "%zu", limits->memory_limit);

        if (write_cgroup_file(path, content, enable_debug) < 0) {
            fprintf(stderr, "[cgroup] Failed to set memory limit\n");
            return -1;
        }

        if (enable_debug) {
            printf("[cgroup] Memory limit: %zu bytes\n", limits->memory_limit);
        }
    }

    // Set CPU limit
    if (limits->cpu_quota > 0) {
        char path[512];
        char content[64];
        long period = limits->cpu_period > 0 ? limits->cpu_period : 100000;

        snprintf(path, sizeof(path), "%s/cpu.max", ctx->cgroup_path);
        snprintf(content, sizeof(content), "%ld %ld",
                 limits->cpu_quota, period);

        if (write_cgroup_file(path, content, enable_debug) < 0) {
            fprintf(stderr, "[cgroup] Failed to set CPU limit\n");
            return -1;
        }

        if (enable_debug) {
            printf("[cgroup] CPU limit: %ld/%ld µs\n",
                   limits->cpu_quota, period);
        }
    }

    // Set PID limit
    if (limits->pid_limit > 0) {
        char path[512];
        char content[64];

        snprintf(path, sizeof(path), "%s/pids.max", ctx->cgroup_path);
        snprintf(content, sizeof(content), "%zu", limits->pid_limit);

        if (write_cgroup_file(path, content, enable_debug) < 0) {
            fprintf(stderr, "[cgroup] Failed to set PID limit\n");
            return -1;
        }

        if (enable_debug) {
            printf("[cgroup] PID limit: %zu\n", limits->pid_limit);
        }
    }

    return 0;
}

/**
 * Add PID to cgroup.
 *
 * Writes the PID to cgroup.procs. All child processes automatically
 * inherit the cgroup membership.
 */
int add_pid_to_cgroup(const cgroup_context_t *ctx, pid_t pid,
                      bool enable_debug) {
    char path[512];
    char content[32];

    snprintf(path, sizeof(path), "%s/cgroup.procs", ctx->cgroup_path);
    snprintf(content, sizeof(content), "%d", pid);

    if (write_cgroup_file(path, content, enable_debug) < 0) {
        fprintf(stderr, "[cgroup] Failed to add PID %d to cgroup\n", pid);
        return -1;
    }

    if (enable_debug) {
        printf("[cgroup] Added PID %d to cgroup\n", pid);
    }

    return 0;
}

/**
 * Remove cgroup directory.
 *
 * Only succeeds when the cgroup is empty (no processes). This is why
 * we call it after waitpid() — the child has already exited.
 */
void remove_cgroup(cgroup_context_t *ctx, bool enable_debug) {
    if (!ctx->created) {
        return;
    }

    if (enable_debug) {
        printf("[cgroup] Removing cgroup: %s\n", ctx->cgroup_path);
    }

    if (rmdir(ctx->cgroup_path) < 0) {
        if (enable_debug) {
            fprintf(stderr, "[cgroup] Failed to remove cgroup: %s\n",
                    strerror(errno));
        }
    }

    ctx->created = false;
}

/**
 * Close all file descriptors above STDERR_FILENO.
 *
 * Phase 3 correction §3.5, carried forward: after clone(), the child
 * inherits every open fd from the parent. Any fd referencing the host
 * filesystem survives pivot_root + umount and can be used to escape
 * the mount namespace (CVE-2024-21626, CVE-2016-9962).
 *
 * Note: `static` duplicate of the function in uts.c. The superseding
 * module pattern accepts this duplication (see architecture decisions).
 * Phase 7's refactor into a shared execution core will consolidate this.
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
 * Unchanged in spirit from Phase 4b/4c. Cgroups are entirely parent-side —
 * the child doesn't know or care that it's in a cgroup. All corrections
 * from previous phases are carried forward: sync pipe for UID/GID maps,
 * graceful /proc degradation in user namespace, inherited fd cleanup
 * (CVE-2024-21626/CVE-2016-9962).
 */
static int child_func(void *arg) {
    child_args_t *args = (child_args_t *)arg;

    // If user namespace is active, wait for parent to write UID/GID maps.
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

    // Set hostname (in UTS namespace)
    if (args->hostname) {
        if (setup_uts(args->hostname, args->enable_debug) < 0) {
            fprintf(stderr, "[child] Failed to setup UTS\n");
            return 1;
        }
    }

    // Setup rootfs (pivot_root dance, carried forward from Phase 2-3)
    if (args->rootfs_path) {
        if (setup_rootfs(args->rootfs_path, args->enable_debug) < 0) {
            fprintf(stderr, "[child] Failed to setup rootfs\n");
            return 1;
        }

        // Mount /proc after pivot_root (Phase 4b graceful degradation).
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

/**
 * Execute process with optional cgroup resource limits.
 *
 * Supersedes uts_exec() — handles all Phase 4c functionality plus
 * cgroup creation, PID assignment, and cleanup.
 */
cgroup_result_t cgroup_exec(const cgroup_config_t *config) {
    cgroup_result_t result = {0};
    overlay_context_t overlay_ctx = {0};
    bool overlay_active = false;
    int sync_pipe[2] = {-1, -1};

    // Validate
    if (!config || !config->program || !config->argv) {
        fprintf(stderr, "cgroup_exec: invalid config\n");
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

    // Step 1: Setup cgroup BEFORE creating process
    if (config->enable_cgroup) {
        if (setup_cgroup(&result.cgroup_ctx, &config->cgroup_limits,
                         config->enable_debug) < 0) {
            fprintf(stderr, "[parent] Failed to setup cgroup\n");
            result.child_pid = -1;
            return result;
        }
    }

    // Create sync pipe for user namespace
    if (config->enable_user_namespace) {
        if (pipe(sync_pipe) < 0) {
            perror("pipe");
            if (config->enable_cgroup) {
                remove_cgroup(&result.cgroup_ctx, config->enable_debug);
            }
            result.child_pid = -1;
            return result;
        }
    }

    // Determine the effective rootfs path
    const char *effective_rootfs = config->rootfs_path;

    // Setup overlay if requested (Phase 3 functionality)
    if (config->enable_overlay && config->rootfs_path) {
        if (setup_overlay(&overlay_ctx, config->rootfs_path,
                          config->container_dir, config->enable_debug) < 0) {
            fprintf(stderr, "[parent] Failed to setup overlay\n");
            if (sync_pipe[0] >= 0) { close(sync_pipe[0]); close(sync_pipe[1]); }
            if (config->enable_cgroup) {
                remove_cgroup(&result.cgroup_ctx, config->enable_debug);
            }
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
        if (config->enable_cgroup) {
            remove_cgroup(&result.cgroup_ctx, config->enable_debug);
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
        .rootfs_path = effective_rootfs,
        .hostname = config->hostname,
        .sync_fd = sync_pipe[0],    // Child gets read end (-1 if no user ns)
        .user_namespace_active = config->enable_user_namespace
    };

    // Build flags
    int flags = SIGCHLD;
    if (config->enable_user_namespace) {
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
    if (config->enable_ipc_namespace) {
        flags |= CLONE_NEWIPC;

        if (config->enable_debug) {
            printf("[parent] Creating IPC namespace\n");
        }
    }

    // Clone
    pid_t pid = clone(child_func, stack + STACK_SIZE, flags, &child_args);

    if (pid < 0) {
        perror("clone");
        free(stack);
        if (overlay_active) {
            teardown_overlay(&overlay_ctx, config->enable_debug);
        }
        if (sync_pipe[0] >= 0) { close(sync_pipe[0]); close(sync_pipe[1]); }
        if (config->enable_cgroup) {
            remove_cgroup(&result.cgroup_ctx, config->enable_debug);
        }
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
        // API-stability reuse (see §3.1): setup_user_namespace_mapping()
        // is declared in uts.h to take a uts_config_t. Rather than break
        // that signature, we construct a partial uts_config_t with only
        // the fields the function reads — enable_debug + the six mapping
        // fields. The other uts_config_t fields are left zero-initialized;
        // setup_user_namespace_mapping() never touches them.
        //
        // Phase 7's *_exec() refactor will hoist these mapping fields into
        // a dedicated user_ns_mapping_t shared by both modules, removing
        // the synthetic-config workaround.
        uts_config_t uts_cfg = {
            .enable_debug = config->enable_debug,
            .uid_map_inside = config->uid_map_inside,
            .uid_map_outside = config->uid_map_outside,
            .uid_map_range = config->uid_map_range,
            .gid_map_inside = config->gid_map_inside,
            .gid_map_outside = config->gid_map_outside,
            .gid_map_range = config->gid_map_range
        };

        if (setup_user_namespace_mapping(pid, &uts_cfg) < 0) {
            fprintf(stderr, "[parent] Failed to setup user namespace mapping\n");
            close(sync_pipe[1]);
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            free(stack);
            if (overlay_active) {
                teardown_overlay(&overlay_ctx, config->enable_debug);
            }
            if (config->enable_cgroup) {
                remove_cgroup(&result.cgroup_ctx, config->enable_debug);
            }
            result.child_pid = -1;
            result.stack_ptr = NULL;
            return result;
        }

        if (write(sync_pipe[1], "1", 1) < 0) {
            perror("write(sync_pipe)");
        }
        close(sync_pipe[1]);
        sync_pipe[1] = -1;

        if (config->enable_debug) {
            printf("[parent] Signaled child to proceed\n");
        }
    }

    // Step 5: Add child to cgroup (after clone, after user namespace mapping)
    if (config->enable_cgroup) {
        if (add_pid_to_cgroup(&result.cgroup_ctx, pid,
                              config->enable_debug) < 0) {
            fprintf(stderr, "[parent] Failed to add PID to cgroup\n");
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            free(stack);
            if (overlay_active) {
                teardown_overlay(&overlay_ctx, config->enable_debug);
            }
            remove_cgroup(&result.cgroup_ctx, config->enable_debug);
            result.child_pid = -1;
            result.stack_ptr = NULL;
            return result;
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

void cgroup_cleanup(cgroup_result_t *result) {
    if (result) {
        // Remove cgroup (must be empty — child already exited)
        remove_cgroup(&result->cgroup_ctx, false);

        if (result->stack_ptr) {
            free(result->stack_ptr);
            result->stack_ptr = NULL;
        }
    }
}
