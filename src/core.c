// Note: _GNU_SOURCE is provided by the Makefile via -D_GNU_SOURCE.
#include "core.h"
#include "mount.h"
#include "overlay.h"
#include "uts.h"
#include "cgroup.h"
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <dirent.h>

#define STACK_SIZE (1024 * 1024)

/* Child-side argument struct. File-local to core.c — every helper that
 * needs to talk to the child does so through this struct.
 *
 * Note: copied-by-value into the clone stack via the &child_args pointer
 * we pass to clone(). clone() without CLONE_VM gives the child a COPY of
 * the parent's address space, so anything not in child_args before clone
 * is invisible to the child (see Phase 6 §3.4.1). */
typedef struct {
    const char *program;
    char *const *argv;
    char *const *envp;
    bool enable_debug;
    const char *rootfs_path;
    const char *hostname;
    int  sync_fd;                // -1 if no sync needed
    bool user_namespace_active;  // For /proc graceful degradation
    bool network_active;         // Gates configure_container_net()
    veth_config_t veth;          // Snapshot of veth config
    net_context_t net_ctx;       // Snapshot of net context (veth names)
} child_args_t;

/**
 * Close every inherited file descriptor above stderr (except the
 * `/proc/self/fd` directory we're iterating). Mitigates
 * CVE-2024-21626 / CVE-2016-9962 (mount-namespace escapes via
 * surviving fds). See Phase 3 §3.5.
 *
 * Falls back to brute-force close of [3..RLIMIT_NOFILE) when `/proc`
 * isn't mounted — graceful-degradation case from Phase 4b.
 */
static void close_inherited_fds(bool enable_debug) {
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) {
        struct rlimit rl;
        int max_fd = 1024;
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
        if (fd <= STDERR_FILENO || fd == dir_fd) continue;
        if (enable_debug) printf("[child] Closing inherited fd %d\n", fd);
        close(fd);
    }
    closedir(dir);
}

/**
 * Child process entry point.
 *
 * Lifecycle:
 *   1. Wait on sync pipe (if active)
 *   2. setup_uts(hostname) if hostname set
 *   3. setup_rootfs(rootfs_path) if rootfs_path set
 *   4. mount_proc() with graceful degradation under user namespace
 *   5. configure_container_net() if network active
 *   6. close_inherited_fds() — CVE-2024-21626/CVE-2016-9962 mitigation
 *   7. execve(program, argv, envp)
 */
static int child_func(void *arg) {
    child_args_t *args = (child_args_t *)arg;

    /* 1. Sync wait (parent signals when UID/GID maps + veth setup done). */
    if (args->sync_fd >= 0) {
        char buf;
        if (args->enable_debug) {
            printf("[child] Waiting on sync pipe...\n");
        }
        ssize_t n = read(args->sync_fd, &buf, 1);
        close(args->sync_fd);
        if (n < 1) {
            fprintf(stderr, "[child] Sync pipe read failed\n");
            return 1;
        }
        if (args->enable_debug) {
            printf("[child] Sync complete, proceeding\n");
        }
    }

    if (args->enable_debug) {
        printf("[child] PID: %d, UID: %d, GID: %d\n",
               getpid(), getuid(), getgid());
    }

    /* 2. Hostname. */
    if (args->hostname) {
        if (setup_uts(args->hostname, args->enable_debug) < 0) {
            fprintf(stderr, "[child] Failed to setup UTS\n");
            return 1;
        }
    }

    /* 3+4. Rootfs + /proc. */
    if (args->rootfs_path) {
        if (setup_rootfs(args->rootfs_path, args->enable_debug) < 0) {
            fprintf(stderr, "[child] Failed to setup rootfs\n");
            return 1;
        }
        if (mount_proc(args->enable_debug) < 0) {
            if (args->user_namespace_active) {
                fprintf(stderr,
                    "[child] Warning: /proc mount denied "
                    "(user namespace restriction) — continuing without /proc\n");
            } else {
                fprintf(stderr, "[child] Failed to mount /proc\n");
                return 1;
            }
        }
    }

    /* 5. Network configuration (Phase 6). */
    if (args->network_active) {
        if (configure_container_net(&args->net_ctx, &args->veth,
                                    args->enable_debug) < 0) {
            fprintf(stderr, "[child] Failed to configure container network\n");
            return 1;
        }
    }

    /* 6. Close inherited fds. */
    close_inherited_fds(args->enable_debug);

    /* 7. Execute target program. */
    execve(args->program, args->argv, args->envp);
    perror("execve");
    return 127;
}

container_result_t container_exec(const container_config_t *config) {
    container_result_t result = {0};
    bool overlay_active = false;
    int sync_pipe[2] = {-1, -1};

    /* Validate */
    if (!config || !config->program || !config->argv) {
        fprintf(stderr, "container_exec: invalid config\n");
        result.child_pid = -1;
        return result;
    }

    if (config->enable_debug) {
        printf("[parent] Executing: %s", config->program);
        for (int i = 0; config->argv[i]; i++) printf(" %s", config->argv[i]);
        printf("\n");
        if (config->rootfs_path) printf("[parent] Rootfs: %s\n", config->rootfs_path);
        if (config->enable_overlay) printf("[parent] Overlay: enabled\n");
        if (config->enable_network) {
            printf("[parent] Network: enabled (%s <-> %s/%s)\n",
                   config->veth.host_ip, config->veth.container_ip,
                   config->veth.netmask);
        }
    }

    /* Phase 6 carry-forward: fail loudly here if --net is enabled but
     * no `ip` binary exists on the host. Without this early check the
     * failure surfaces mid-clone-and-sync with confusing diagnostics. */
    if (config->enable_network && !find_ip_binary()) {
        fprintf(stderr, "[parent] --net requires /sbin/ip or /usr/sbin/ip; "
                        "install the `iproute2` package\n");
        result.child_pid = -1;
        return result;
    }

    /* Step 1: cgroup */
    if (config->enable_cgroup) {
        if (setup_cgroup(&result.ctx.cgroup_ctx, &config->cgroup_limits,
                         config->enable_debug) < 0) {
            fprintf(stderr, "[parent] Failed to setup cgroup\n");
            result.child_pid = -1;
            return result;
        }
    }

    /* Step 2: sync pipe if user-ns OR network */
    bool needs_sync = config->enable_user_namespace || config->enable_network;
    if (needs_sync) {
        if (pipe(sync_pipe) < 0) {
            perror("pipe");
            if (config->enable_cgroup) {
                remove_cgroup(&result.ctx.cgroup_ctx, config->enable_debug);
            }
            result.child_pid = -1;
            return result;
        }
    }

    /* Step 3: overlay */
    const char *effective_rootfs = config->rootfs_path;
    if (config->enable_overlay && config->rootfs_path) {
        if (setup_overlay(&result.ctx.overlay_ctx, config->rootfs_path,
                          config->container_dir, config->enable_debug) < 0) {
            fprintf(stderr, "[parent] Failed to setup overlay\n");
            if (sync_pipe[0] >= 0) { close(sync_pipe[0]); close(sync_pipe[1]); }
            if (config->enable_cgroup) {
                remove_cgroup(&result.ctx.cgroup_ctx, config->enable_debug);
            }
            result.child_pid = -1;
            return result;
        }
        overlay_active = true;
        effective_rootfs = result.ctx.overlay_ctx.merged_path;
        if (config->enable_debug) {
            printf("[parent] Using merged rootfs: %s\n", effective_rootfs);
        }
    }

    /* Step 4: clone stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc");
        if (overlay_active) teardown_overlay(&result.ctx.overlay_ctx, config->enable_debug);
        if (sync_pipe[0] >= 0) { close(sync_pipe[0]); close(sync_pipe[1]); }
        if (config->enable_cgroup) {
            remove_cgroup(&result.ctx.cgroup_ctx, config->enable_debug);
        }
        result.child_pid = -1;
        return result;
    }
    result.ctx.stack_ptr = stack;

    /* Step 4b: generate veth names BEFORE clone (see Phase 6 §3.4.1) */
    if (config->enable_network) {
        generate_veth_names(&result.ctx.net_ctx);
        if (config->enable_debug) {
            printf("[parent] Generated veth names: %s <-> %s\n",
                   result.ctx.net_ctx.veth_host,
                   result.ctx.net_ctx.veth_container);
        }
    }

    /* Step 5: child_args */
    child_args_t child_args = {
        .program = config->program,
        .argv = config->argv,
        .envp = config->envp,
        .enable_debug = config->enable_debug,
        .rootfs_path = effective_rootfs,
        .hostname = config->hostname,
        .sync_fd = sync_pipe[0],
        .user_namespace_active = config->enable_user_namespace,
        .network_active = config->enable_network,
        .veth = config->veth,
        .net_ctx = result.ctx.net_ctx
    };

    /* Step 6: clone flags */
    int flags = SIGCHLD;
    if (config->enable_user_namespace) {
        flags |= CLONE_NEWUSER;
        if (config->enable_debug) printf("[parent] Creating user namespace\n");
    }
    if (config->enable_pid_namespace)   flags |= CLONE_NEWPID;
    if (config->enable_mount_namespace) flags |= CLONE_NEWNS;
    if (config->enable_uts_namespace) {
        flags |= CLONE_NEWUTS;
        if (config->enable_debug) printf("[parent] Creating UTS namespace\n");
    }
    if (config->enable_ipc_namespace) {
        flags |= CLONE_NEWIPC;
        if (config->enable_debug) printf("[parent] Creating IPC namespace\n");
    }
    if (config->enable_network) {
        flags |= CLONE_NEWNET;
        if (config->enable_debug) printf("[parent] Creating network namespace\n");
    }

    /* Step 7: clone */
    pid_t pid = clone(child_func, stack + STACK_SIZE, flags, &child_args);
    if (pid < 0) {
        perror("clone");
        free(stack);
        if (overlay_active) teardown_overlay(&result.ctx.overlay_ctx, config->enable_debug);
        if (sync_pipe[0] >= 0) { close(sync_pipe[0]); close(sync_pipe[1]); }
        if (config->enable_cgroup) {
            remove_cgroup(&result.ctx.cgroup_ctx, config->enable_debug);
        }
        result.child_pid = -1;
        result.ctx.stack_ptr = NULL;
        return result;
    }
    result.child_pid = pid;

    if (config->enable_debug) printf("[parent] Child PID: %d\n", pid);

    /* Step 8: close read end in parent */
    if (sync_pipe[0] >= 0) {
        close(sync_pipe[0]);
        sync_pipe[0] = -1;
    }

    /* Step 9: user-ns mapping */
    if (config->enable_user_namespace) {
        user_ns_mapping_t mapping = {
            .uid_map_inside  = config->uid_map_inside,
            .uid_map_outside = config->uid_map_outside,
            .uid_map_range   = config->uid_map_range,
            .gid_map_inside  = config->gid_map_inside,
            .gid_map_outside = config->gid_map_outside,
            .gid_map_range   = config->gid_map_range,
            .enable_debug    = config->enable_debug
        };
        if (setup_user_namespace_mapping(pid, &mapping) < 0) {
            fprintf(stderr, "[parent] Failed to setup user namespace mapping\n");
            if (sync_pipe[1] >= 0) close(sync_pipe[1]);
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            free(stack);
            if (overlay_active) teardown_overlay(&result.ctx.overlay_ctx, config->enable_debug);
            if (config->enable_cgroup) {
                remove_cgroup(&result.ctx.cgroup_ctx, config->enable_debug);
            }
            result.child_pid = -1;
            result.ctx.stack_ptr = NULL;
            return result;
        }
    }

    /* Step 10: setup_net (Phase 6) */
    if (config->enable_network) {
        if (setup_net(&result.ctx.net_ctx, &config->veth, pid,
                      config->enable_debug) < 0) {
            fprintf(stderr, "[parent] Failed to setup network\n");
            if (sync_pipe[1] >= 0) close(sync_pipe[1]);
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            free(stack);
            if (overlay_active) teardown_overlay(&result.ctx.overlay_ctx, config->enable_debug);
            cleanup_net(&result.ctx.net_ctx, config->enable_debug);
            if (config->enable_cgroup) {
                remove_cgroup(&result.ctx.cgroup_ctx, config->enable_debug);
            }
            result.child_pid = -1;
            result.ctx.stack_ptr = NULL;
            return result;
        }
    }

    /* Step 11: signal child */
    if (needs_sync) {
        if (write(sync_pipe[1], "1", 1) < 0) {
            perror("write(sync_pipe)");
        }
        close(sync_pipe[1]);
        sync_pipe[1] = -1;
        if (config->enable_debug) printf("[parent] Signaled child to proceed\n");
    }

    /* Step 12: add_pid_to_cgroup AFTER sync (cgroup checks mapped UID) */
    if (config->enable_cgroup) {
        if (add_pid_to_cgroup(&result.ctx.cgroup_ctx, pid,
                              config->enable_debug) < 0) {
            fprintf(stderr, "[parent] Failed to add PID to cgroup\n");
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            free(stack);
            if (overlay_active) teardown_overlay(&result.ctx.overlay_ctx, config->enable_debug);
            cleanup_net(&result.ctx.net_ctx, config->enable_debug);
            remove_cgroup(&result.ctx.cgroup_ctx, config->enable_debug);
            result.child_pid = -1;
            result.ctx.stack_ptr = NULL;
            return result;
        }
    }

    /* Step 13: waitpid */
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        result.exit_status = -1;
        if (overlay_active) teardown_overlay(&result.ctx.overlay_ctx, config->enable_debug);
        cleanup_net(&result.ctx.net_ctx, config->enable_debug);
        return result;
    }

    /* Step 14: parse status */
    if (WIFEXITED(status)) {
        result.exited_normally = true;
        result.exit_status = WEXITSTATUS(status);
        if (config->enable_debug) printf("[parent] Child exited: %d\n", result.exit_status);
    } else if (WIFSIGNALED(status)) {
        result.exited_normally = false;
        result.signal = WTERMSIG(status);
        result.exit_status = 128 + result.signal;
        if (config->enable_debug) {
            printf("[parent] Child killed by signal: %d\n", result.signal);
        }
    }

    /* Step 15: teardown overlay (cgroup + net deferred to container_cleanup) */
    if (overlay_active) teardown_overlay(&result.ctx.overlay_ctx, config->enable_debug);

    return result;
}

void container_cleanup(container_result_t *result) {
    if (!result) return;

    cleanup_net(&result->ctx.net_ctx, false);
    remove_cgroup(&result->ctx.cgroup_ctx, false);
    if (result->ctx.stack_ptr) {
        free(result->ctx.stack_ptr);
        result->ctx.stack_ptr = NULL;
    }
}