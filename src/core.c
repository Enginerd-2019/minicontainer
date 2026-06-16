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
#include "pty.h"
#include "hardening.h"   // Phase 8b: drop_capabilities / apply_no_new_privs / apply_seccomp_filter
#include "init.h"        // Phase 8b+: run_init_supervisor (--init PID-1 shim)

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
    bind_mount_t mounts[MAX_MOUNTS];
    int  mount_count;
    bool enable_pty;          // Whether to allocate a PTY in the child
    int  pty_sock_fd;         // Child's end of socketpair; -1 if no PTY
    int  stdout_fd;           // -1 = leave alone
    int  stderr_fd;           // -1 = leave alone
    bool enable_hardening;    // Phase 8b: gates cap drop / NO_NEW_PRIVS / seccomp (and mount_sys_ro)
    bool enable_init;         // Phase 8b+: wrap workload in the PID-1 init supervisor
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
static int child_func(void *arg)
{
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

    /* 2. Hostname (Phase 4). */
    if (args->hostname) {
        if (setup_uts(args->hostname, args->enable_debug) < 0) {
            fprintf(stderr, "[child] Failed to setup UTS\n");
            return 1;
        }
    }

    /* 3. Rootfs + bind mounts + /proc + (8b NEW) /sys-ro.  Bind mounts
     * are applied INSIDE setup_rootfs, before pivot_root — the host
     * source path stops resolving once the old root is detached
     * (Phase 7b, decisions.md Error #22). */
    if (args->rootfs_path) {
        if (setup_rootfs(args->rootfs_path, args->mounts,
                         args->mount_count, args->enable_debug) < 0) {
            fprintf(stderr, "[child] Failed to setup rootfs\n");
            return 1;
        }

        /* 4. /proc with Phase 4b graceful degradation. */
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

        /* 4b. Private devpts instance (Phase 7b §7.2.2).  Runs whenever a
         * rootfs is set, regardless of --interactive — every container
         * gets a working pty subsystem so curses-based / pty-allocating
         * programs (apt, nano, less) work even non-interactively.
         * Graceful degradation under user namespace, same shape as
         * mount_proc. */
        if (mount_devpts(args->enable_debug,
                         args->user_namespace_active) < 0) {
            fprintf(stderr, "[child] Failed to mount devpts\n");
            return 1;
        }

        /* 5 (Phase 8b — NEW): /sys read-only when hardening.
         * Sits between the mount block and configure_container_net. */
        if (args->enable_hardening) {
            if (mount_sys_ro(args->enable_debug) < 0) {
                fprintf(stderr, "[child] Failed to mount /sys read-only\n");
                return 1;
            }
        }
    }

    /* 6. PTY / log fd wiring (Phase 7b §7.3).  The pair is allocated
     * child-side after mount_devpts and the master fd is handed back to
     * the parent via SCM_RIGHTS — there is NO pty_slave_path field on
     * container_config_t (the cycle-0 path-passing design was broken;
     * see Phase 7b guide §2.4). */
    pty_pair_t pty = { .master_fd = -1, .slave_fd = -1, .slave_path = {0} };
    if (args->enable_pty) {
        if (pty_open_in_child(&pty, args->enable_debug) < 0) {
            fprintf(stderr, "[child] Failed to allocate PTY\n");
            return 1;
        }
        if (pty_send_master(args->pty_sock_fd, pty.master_fd) < 0) {
            fprintf(stderr, "[child] Failed to send PTY master to parent\n");
            close(pty.master_fd); close(pty.slave_fd);
            return 1;
        }
        close(pty.master_fd); pty.master_fd = -1;
        close(args->pty_sock_fd); args->pty_sock_fd = -1;
        if (pty_set_ctty(pty.slave_fd) < 0) {
            fprintf(stderr, "[child] Failed to set PTY as ctty\n");
            return 1;
        }
    } else {
        if (args->stdout_fd >= 0) dup2(args->stdout_fd, STDOUT_FILENO);
        if (args->stderr_fd >= 0) dup2(args->stderr_fd, STDERR_FILENO);
    }

    /* 7. Network configuration (Phase 6 / Phase 7a). */
    if (args->network_active) {
        if (configure_container_net(&args->net_ctx, &args->veth,
                                    args->enable_debug) < 0) {
            fprintf(stderr,
                "[child] Failed to configure container network\n");
            return 1;
        }
    }

    /* 8. Close inherited fds — CVE-2024-21626 / CVE-2016-9962. */
    close_inherited_fds(args->enable_debug);

    /* 9-11 (Phase 8b — NEW): hardening, conditional on --secure.
     * Order is fixed by kernel rules:
     *   drop_capabilities BEFORE no_new_privs (just convention here)
     *   no_new_privs      BEFORE seccomp     (kernel requirement) */
    if (args->enable_hardening) {
        if (drop_capabilities(args->enable_debug) < 0) {
            fprintf(stderr, "[child] Failed to drop capabilities\n");
            return 1;
        }
        if (apply_no_new_privs(args->enable_debug) < 0) {
            fprintf(stderr, "[child] Failed to set NO_NEW_PRIVS\n");
            return 1;
        }
        if (apply_seccomp_filter(args->enable_debug) < 0) {
            fprintf(stderr, "[child] Failed to install seccomp filter\n");
            return 1;
        }
    }

    /* 12 (Phase 8b+): --init wraps the workload in a PID-1 supervisor
     * (signal forwarding + zombie reaping + status mirroring) instead of
     * exec'ing it directly.  Placed AFTER hardening so both the
     * supervisor and the workload inherit the caps/NO_NEW_PRIVS/seccomp
     * envelope.  run_init_supervisor only returns on a normal workload
     * exit (with its code) or fork failure; on a signal death it
     * re-raises and never returns. */
    if (args->enable_init) {
        return run_init_supervisor(args->program, args->argv, args->envp,
                                   args->enable_debug);
    }

    /* 13. Execute target program. */
    execve(args->program, args->argv, args->envp);
    perror("execve");
    return 127;
}

/**
 * Phase 7b: parent-side waitpid + overlay teardown.  Companion to
 * container_start.  Steps 13-15 of the original Phase 7a
 * container_exec pipeline, lifted into a separate function so the
 * CLI can interleave a state-file write between container_start and
 * container_wait (cmd_run) or skip container_wait entirely
 * (cmd_start, which detaches the child).
 *
 * On waitpid failure (e.g. EINTR storm), result->exit_status is set
 * to -1 and overlay/net teardown still runs.
 */
void container_wait(container_result_t *result, bool enable_debug) {
    if (!result || result->child_pid < 0) return;

    bool overlay_active = result->ctx.overlay_ctx.is_mounted;

    /* Step 13: waitpid */
    int status;
    if (waitpid(result->child_pid, &status, 0) < 0) {
        perror("waitpid");
        result->exit_status = -1;
        if (overlay_active) {
            teardown_overlay(&result->ctx.overlay_ctx, enable_debug);
        }
        cleanup_net(&result->ctx.net_ctx, enable_debug);
        return;
    }

    /* Step 14: parse status */
    if (WIFEXITED(status)) {
        result->exited_normally = true;
        result->exit_status = WEXITSTATUS(status);
        if (enable_debug) {
            printf("[parent] Child exited: %d\n", result->exit_status);
        }
    } else if (WIFSIGNALED(status)) {
        result->exited_normally = false;
        result->signal = WTERMSIG(status);
        result->exit_status = 128 + result->signal;
        if (enable_debug) {
            printf("[parent] Child killed by signal: %d\n", result->signal);
        }
    }

    /* Step 15: teardown overlay (cgroup + net deferred to container_cleanup) */
    if (overlay_active) {
        teardown_overlay(&result->ctx.overlay_ctx, enable_debug);
    }
}

container_result_t container_start(const container_config_t *config) {
    container_result_t result = {0};
    result.pty_master_fd = -1;
    bool overlay_active = false;
    int sync_pipe[2] = {-1, -1};
    int pty_sock[2]  = {-1, -1};

    /* Validate */
    if (!config || !config->program || !config->argv) {
        fprintf(stderr, "container_start: invalid config\n");
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

    /* Step 2: sync pipe if user-ns OR network OR cgroup.  cgroup is
     * included so the parent can place the child in the cgroup BEFORE
     * the child runs a single instruction — otherwise the child races
     * ahead and forks/faults while still in the PARENT's cgroup, so
     * pids/memory limits silently don't apply and memory.current
     * under-counts (charge-at-fault to the parent cgroup).
     * (decisions.md Error #25) */
    bool needs_sync = config->enable_user_namespace ||
                      config->enable_network ||
                      config->enable_cgroup;
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
                          config->container_dir, config->container_id,
                          config->enable_debug) < 0) {
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

    /* Step 4c (Phase 7b): PTY socketpair if --interactive.  Must exist
     * before clone() so the child inherits sv[1] in its fd table.  The
     * child opens the master with posix_openpt after mount_devpts and
     * sends the master fd back via SCM_RIGHTS on this socketpair; the
     * parent receives it in step 12b below. */
    if (config->enable_pty) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pty_sock) < 0) {
            perror("socketpair");
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
        if (config->enable_debug) {
            printf("[parent] PTY socketpair: parent fd %d, child fd %d\n",
                   pty_sock[0], pty_sock[1]);
        }
    }

    /* Step 5: child_args.  Phase 7b: explicit -1 sentinels for the new
     * fd fields are load-bearing — zero-initialization would leave them
     * at STDIN_FILENO and the child's `dup2(args->stdout_fd, ...)` would
     * dup stdin over stdout.  mounts[] is filled via memcpy below
     * because designated init can't express an array copy. */
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
        .net_ctx = result.ctx.net_ctx,
        .mount_count = config->mount_count,
        .enable_pty = config->enable_pty,
        .pty_sock_fd = pty_sock[1],            // -1 if !enable_pty (init value)
        .stdout_fd = config->stdout_fd,
        .stderr_fd = config->stderr_fd,
        .enable_hardening = config->enable_hardening,  // Phase 8b
        .enable_init = config->enable_init             // Phase 8b+
    };
    if (config->mount_count > 0) {
        memcpy(child_args.mounts, config->mounts,
               sizeof(bind_mount_t) * (size_t)config->mount_count);
    }

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
        if (pty_sock[0] >= 0) { close(pty_sock[0]); close(pty_sock[1]); }
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

    /* Step 8: close fds the child now owns. */
    if (sync_pipe[0] >= 0) {
        close(sync_pipe[0]);
        sync_pipe[0] = -1;
    }
    if (pty_sock[1] >= 0) {
        close(pty_sock[1]);
        pty_sock[1] = -1;
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
            if (pty_sock[0] >= 0) close(pty_sock[0]);
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
            if (pty_sock[0] >= 0) close(pty_sock[0]);
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

    /* Step 11: add_pid_to_cgroup BEFORE signaling the child.  The child
     * blocks on the sync pipe (needs_sync includes enable_cgroup), so
     * adding it here — before the sync write below — guarantees it is a
     * cgroup member before it runs any workload, which is what makes
     * pids/memory limits actually apply and memory.current account
     * correctly.  This runs AFTER the user-ns mapping (step 9) so a
     * delegated (rootless) cgroup.procs write still sees the mapped UID.
     * (decisions.md Error #25) */
    if (config->enable_cgroup) {
        if (add_pid_to_cgroup(&result.ctx.cgroup_ctx, pid,
                              config->enable_debug) < 0) {
            fprintf(stderr, "[parent] Failed to add PID to cgroup\n");
            if (sync_pipe[1] >= 0) close(sync_pipe[1]);
            if (pty_sock[0] >= 0) close(pty_sock[0]);
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

    /* Step 12: signal child to proceed (now that it is in the cgroup). */
    if (needs_sync) {
        if (write(sync_pipe[1], "1", 1) < 0) {
            perror("write(sync_pipe)");
        }
        close(sync_pipe[1]);
        sync_pipe[1] = -1;
        if (config->enable_debug) printf("[parent] Signaled child to proceed\n");
    }

    /* Step 12b (Phase 7b): receive PTY master fd from child via
     * SCM_RIGHTS.  Runs AFTER write-sync (step 11) because the child
     * waits on the sync pipe before mounting devpts + allocating the
     * pty + calling pty_send_master.  Closing pty_sock[0] before
     * pty_recv_master returns would cause the recvmsg to fail. */
    if (config->enable_pty) {
        if (pty_recv_master(pty_sock[0], &result.pty_master_fd) < 0) {
            fprintf(stderr, "[parent] Failed to receive PTY master from child\n");
            close(pty_sock[0]);
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
        close(pty_sock[0]);
        pty_sock[0] = -1;
        if (config->enable_debug) {
            printf("[parent] Received PTY master fd %d from child\n",
                   result.pty_master_fd);
        }
    }

    /* container_start ends here.  Steps 13-15 (waitpid + parse status +
     * teardown overlay) live in container_wait, which the caller invokes
     * separately so it can interleave a state-file write between launch
     * and reap (cmd_run) or skip the wait entirely (cmd_start). */
    return result;
}

/**
 * Phase 7a's monolithic entry point.  After the Phase 7b split this is
 * a thin wrapper: container_start() to launch, container_wait() to reap
 * (only if the child actually launched).  Kept for backwards
 * compatibility with the transitional main.c and the existing
 * tests/test_*.c suite — cmd_run does NOT call this because it needs
 * the state-file write between the two halves.
 */
container_result_t container_exec(const container_config_t *config) {
    container_result_t result = container_start(config);
    if (result.child_pid > 0) {
        container_wait(&result, config ? config->enable_debug : false);
    }
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