// Note: _GNU_SOURCE is provided by the Makefile via -D_GNU_SOURCE.
// Do NOT redefine it here (Error #8 from decisions.md).
#include "net.h"
#include "cgroup.h"   // setup_cgroup, add_pid_to_cgroup, remove_cgroup
#include "uts.h"      // setup_uts, setup_user_namespace_mapping, uts_config_t
#include "overlay.h"  // setup_overlay, teardown_overlay, overlay_context_t
#include "mount.h"    // setup_rootfs, mount_proc
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>        // va_list / va_start / va_arg / va_end
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/resource.h>  // getrlimit for fd fallback range (Phase 3 §3.5)
#include <dirent.h>        // opendir/readdir for fd cleanup (Phase 3 §3.5)

#define STACK_SIZE (1024 * 1024)
#define MAX_IP_ARGS 16

/* Path to the `ip` binary. Discovered at runtime via find_ip_binary().
 * Three paths are checked: /sbin/ip and /usr/sbin/ip cover the typical
 * host install (Ubuntu/Debian/RHEL); /bin/ip covers the rootfs we build
 * with scripts/build_rootfs.sh, which copies every BIN into /bin/.
 *
 * find_ip_binary() is called from BOTH sides of clone():
 *  - Parent (early check in net_exec, before clone) — uses host paths.
 *  - Child (configure_container_net, after pivot_root) — uses the
 *    container's rootfs paths.
 * The same three-path check works for both because we cover every
 * canonical install location. */
#define IP_BIN_SBIN     "/sbin/ip"
#define IP_BIN_USR_SBIN "/usr/sbin/ip"
#define IP_BIN_BIN      "/bin/ip"

typedef struct {
    const char *program;
    char *const *argv;
    char *const *envp;
    bool enable_debug;
    const char *rootfs_path;
    const char *hostname;
    int sync_fd;                // Read end of sync pipe. -1 if no sync needed.
    bool user_namespace_active; // Phase 4b: for /proc graceful degradation
    bool network_active;        // Phase 6: gates configure_container_net call
    veth_config_t veth;         // Snapshot of veth config for child-side setup
    net_context_t net_ctx;      // Snapshot of net context (veth names)
} child_args_t;

/**
 * Locate /sbin/ip, /usr/sbin/ip, or /bin/ip. Returns a static path
 * or NULL if none of the three locations holds an executable `ip`.
 * Checked in that order so the host's typical install (/sbin/ip on
 * Debian/Ubuntu, /usr/sbin/ip on RHEL-style) wins over the rootfs
 * location, which means parent-side calls don't pick up a rootfs
 * binary by accident.
 */
static const char *find_ip_binary(void) {
    if (access(IP_BIN_SBIN, X_OK) == 0) return IP_BIN_SBIN;
    if (access(IP_BIN_USR_SBIN, X_OK) == 0) return IP_BIN_USR_SBIN;
    if (access(IP_BIN_BIN, X_OK) == 0) return IP_BIN_BIN;
    return NULL;
}

/**
 * Run `ip <argv...>` via fork/exec. NOT system() — no shell, no PATH
 * lookup, no metacharacter risk.
 *
 * Returns 0 on exit status 0, -1 otherwise.
 */
static int run_ip_command(bool enable_debug, ...) {
    /* Build argv from variadic arguments. First arg is "ip" itself (argv[0]
     * by convention); subsequent args are the parameters. NULL terminator. */
    const char *ip_path = find_ip_binary();
    if (!ip_path) {
        fprintf(stderr, "[network] No `ip` binary found at %s, %s, or %s\n",
                IP_BIN_SBIN, IP_BIN_USR_SBIN, IP_BIN_BIN);
        return -1;
    }

    const char *args[MAX_IP_ARGS];
    int argc = 0;
    args[argc++] = "ip";

    va_list ap;
    va_start(ap, enable_debug);
    const char *a;
    while ((a = va_arg(ap, const char *)) != NULL && argc < MAX_IP_ARGS - 1) {
        args[argc++] = a;
    }
    va_end(ap);
    args[argc] = NULL;

    if (enable_debug) {
        fprintf(stderr, "[network] exec:");
        for (int i = 0; args[i]; i++) fprintf(stderr, " %s", args[i]);
        fprintf(stderr, "\n");
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        /* Child: silence stderr unless debug enabled so the test output is
         * not polluted by ip's "RTNETLINK answers: File exists" noise on
         * benign retries. */
        if (!enable_debug) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        execv(ip_path, (char *const *)args);
        perror("execv(ip)");
        _exit(127);
    }

    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid(ip)");
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }
    return 0;
}

/* Convenience wrapper: same as run_ip_command but doesn't propagate
 * failure (some commands are idempotent or benignly-failing). */
static void run_ip_command_ignore(bool enable_debug, ...) {
    va_list ap;
    va_start(ap, enable_debug);
    /* Re-collect args into a buffer and forward... for simplicity we
     * implement this as a separate inline copy. */
    const char *ip_path = find_ip_binary();
    if (!ip_path) { va_end(ap); return; }

    const char *args[MAX_IP_ARGS];
    int argc = 0;
    args[argc++] = "ip";
    const char *a;
    while ((a = va_arg(ap, const char *)) != NULL && argc < MAX_IP_ARGS - 1) {
        args[argc++] = a;
    }
    va_end(ap);
    args[argc] = NULL;

    if (enable_debug) {
        fprintf(stderr, "[network] exec (ignore-fail):");
        for (int i = 0; args[i]; i++) fprintf(stderr, " %s", args[i]);
        fprintf(stderr, "\n");
    }

    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        if (!enable_debug) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        execv(ip_path, (char *const *)args);
        _exit(127);
    }
    waitpid(pid, NULL, 0);
}

/**
 * Run `iptables ...` via fork/exec. Same pattern as run_ip_command.
 * Returns 0 on success, -1 otherwise.
 */
static int run_iptables_command(bool enable_debug, char *const argv[]) {
    static const char *iptables_paths[] = {
        "/sbin/iptables", "/usr/sbin/iptables", NULL
    };
    const char *iptables = NULL;
    for (int i = 0; iptables_paths[i]; i++) {
        if (access(iptables_paths[i], X_OK) == 0) {
            iptables = iptables_paths[i];
            break;
        }
    }
    if (!iptables) {
        fprintf(stderr, "[network] No iptables binary found\n");
        return -1;
    }

    if (enable_debug) {
        fprintf(stderr, "[network] exec:");
        for (int i = 0; argv[i]; i++) fprintf(stderr, " %s", argv[i]);
        fprintf(stderr, "\n");
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        if (!enable_debug) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        }
        execv(iptables, argv);
        _exit(127);
    }

    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/**
 * Generate unique veth pair names using the low 16 bits of nanoseconds.
 *
 * CALLED BEFORE clone() so the names are visible to the child via its
 * copy of child_args.net_ctx. See §3.4.1.
 *
 * Format: veth_h_<4hex> and veth_c_<4hex>. IFNAMSIZ is 16 chars
 * including NUL; "veth_h_" (7 chars) + 4 hex chars = 11 chars, fits
 * comfortably. 65 536 unique names per millisecond.
 */
void generate_veth_names(net_context_t *ctx) {
    if (!ctx) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    unsigned int id = (unsigned int)(ts.tv_nsec & 0xFFFF);
    snprintf(ctx->veth_host,      sizeof(ctx->veth_host),      "veth_h_%04x", id);
    snprintf(ctx->veth_container, sizeof(ctx->veth_container), "veth_c_%04x", id);
}

/**
 * Setup the veth pair from the parent side. Uses names already in ctx
 * (populated by generate_veth_names() before clone — see §3.4.1).
 */
int setup_net(net_context_t *ctx, const veth_config_t *veth,
              pid_t child_pid, bool enable_debug) {
    if (!ctx || !veth) return -1;

    /* Defensive: if the caller forgot to call generate_veth_names first,
     * the names will be zero-init empty. Fail loudly rather than create
     * an interface named "". */
    if (ctx->veth_host[0] == '\0' || ctx->veth_container[0] == '\0') {
        fprintf(stderr, "[network] setup_net: veth names not generated "
                        "(call generate_veth_names before clone)\n");
        return -1;
    }

    if (enable_debug) {
        printf("[network] Creating veth pair: %s <-> %s\n",
               ctx->veth_host, ctx->veth_container);
    }

    /* 1. Create the veth pair (both ends in host netns) */
    if (run_ip_command(enable_debug,
            "link", "add", ctx->veth_host,
            "type", "veth", "peer", "name", ctx->veth_container,
            (const char *)NULL) < 0) {
        fprintf(stderr, "[network] Failed to create veth pair\n");
        return -1;
    }
    ctx->veth_created = true;

    /* 2. Move the container end into the child's netns */
    char pid_str[16];
    snprintf(pid_str, sizeof(pid_str), "%d", (int)child_pid);
    if (run_ip_command(enable_debug,
            "link", "set", ctx->veth_container, "netns", pid_str,
            (const char *)NULL) < 0) {
        fprintf(stderr, "[network] Failed to move %s into netns %d\n",
                ctx->veth_container, (int)child_pid);
        cleanup_net(ctx, enable_debug);
        return -1;
    }

    /* 3. Configure host end: assign IP and bring up */
    char host_addr[INET_ADDRSTRLEN + 8];
    snprintf(host_addr, sizeof(host_addr), "%s/%s", veth->host_ip, veth->netmask);

    if (run_ip_command(enable_debug,
            "addr", "add", host_addr, "dev", ctx->veth_host,
            (const char *)NULL) < 0) {
        fprintf(stderr, "[network] Failed to assign %s to %s\n",
                host_addr, ctx->veth_host);
        cleanup_net(ctx, enable_debug);
        return -1;
    }

    if (run_ip_command(enable_debug,
            "link", "set", ctx->veth_host, "up",
            (const char *)NULL) < 0) {
        fprintf(stderr, "[network] Failed to bring up %s\n", ctx->veth_host);
        cleanup_net(ctx, enable_debug);
        return -1;
    }

    if (enable_debug) {
        printf("[network] Host veth %s configured at %s\n",
               ctx->veth_host, host_addr);
    }

    /* 4. Optional NAT (so container can reach internet via host) */
    if (veth->enable_nat) {
        if (enable_debug) printf("[network] Enabling NAT\n");

        /* Enable IPv4 forwarding. Write directly to sysctl path rather
         * than shelling out — no external binary needed for this. */
        int fd = open("/proc/sys/net/ipv4/ip_forward", O_WRONLY);
        if (fd >= 0) {
            if (write(fd, "1", 1) != 1) {
                fprintf(stderr, "[network] Warning: failed to enable ip_forward\n");
            }
            close(fd);
        } else if (enable_debug) {
            fprintf(stderr, "[network] Warning: cannot open ip_forward: %s\n",
                    strerror(errno));
        }

        /* Add MASQUERADE rule for the container subnet. Store the source
         * CIDR in ctx so cleanup_net() can delete the same rule later
         * without needing the original veth_config_t. */
        snprintf(ctx->nat_source_cidr, sizeof(ctx->nat_source_cidr),
                 "%s/%s", veth->container_ip, veth->netmask);
        char *iptables_argv[] = {
            "iptables", "-t", "nat", "-A", "POSTROUTING",
            "-s", ctx->nat_source_cidr, "-j", "MASQUERADE", NULL
        };
        if (run_iptables_command(enable_debug, iptables_argv) == 0) {
            ctx->nat_added = true;
        } else {
            /* Clear nat_source_cidr if the rule failed to add — we don't
             * want cleanup_net to try deleting a rule that doesn't exist. */
            ctx->nat_source_cidr[0] = '\0';
            fprintf(stderr, "[network] Warning: iptables MASQUERADE failed; "
                    "container may have no internet access\n");
        }
    }

    return 0;
}

/**
 * Configure the container side of the veth. Runs INSIDE the child, after
 * the sync pipe unblocks.
 */
int configure_container_net(const net_context_t *ctx,
                            const veth_config_t *veth,
                            bool enable_debug) {
    if (!ctx || !veth) return -1;

    if (enable_debug) {
        printf("[child] Configuring container network: %s at %s/%s\n",
               ctx->veth_container, veth->container_ip, veth->netmask);
    }

    /* 1. Bring up loopback. Many programs assume 127.0.0.1 works. */
    if (run_ip_command(enable_debug,
            "link", "set", "lo", "up",
            (const char *)NULL) < 0) {
        fprintf(stderr, "[child] Failed to bring up lo\n");
        return -1;
    }

    /* 2. Assign IP to the container end of the veth */
    char container_addr[INET_ADDRSTRLEN + 8];
    snprintf(container_addr, sizeof(container_addr),
             "%s/%s", veth->container_ip, veth->netmask);
    if (run_ip_command(enable_debug,
            "addr", "add", container_addr, "dev", ctx->veth_container,
            (const char *)NULL) < 0) {
        fprintf(stderr, "[child] Failed to assign %s to %s\n",
                container_addr, ctx->veth_container);
        return -1;
    }

    /* 3. Bring the container end up */
    if (run_ip_command(enable_debug,
            "link", "set", ctx->veth_container, "up",
            (const char *)NULL) < 0) {
        fprintf(stderr, "[child] Failed to bring up %s\n", ctx->veth_container);
        return -1;
    }

    /* 4. Default route via host */
    if (run_ip_command(enable_debug,
            "route", "add", "default", "via", veth->host_ip,
            (const char *)NULL) < 0) {
        fprintf(stderr, "[child] Failed to add default route via %s\n",
                veth->host_ip);
        return -1;
    }

    if (enable_debug) {
        printf("[child] Container network configured: %s, default via %s\n",
               container_addr, veth->host_ip);
    }

    return 0;
}

/**
 * Cleanup veth and NAT rule. Self-contained — uses ctx->nat_source_cidr
 * stored by setup_net so it does not need the original veth_config_t.
 *
 * Deleting the host veth automatically destroys the container veth
 * (kernel removes one end when the other goes). If the child's netns
 * already exited, the container veth is already gone — deleting the
 * host veth still works.
 */
void cleanup_net(net_context_t *ctx, bool enable_debug) {
    if (!ctx) return;

    if (ctx->nat_added && ctx->nat_source_cidr[0] != '\0') {
        if (enable_debug) {
            printf("[network] Removing NAT rule for %s\n",
                   ctx->nat_source_cidr);
        }
        char *iptables_argv[] = {
            "iptables", "-t", "nat", "-D", "POSTROUTING",
            "-s", ctx->nat_source_cidr, "-j", "MASQUERADE", NULL
        };
        run_iptables_command(enable_debug, iptables_argv);
        ctx->nat_added = false;
        ctx->nat_source_cidr[0] = '\0';
    }

    if (ctx->veth_created) {
        if (enable_debug) {
            printf("[network] Deleting veth: %s\n", ctx->veth_host);
        }
        run_ip_command_ignore(enable_debug,
            "link", "delete", ctx->veth_host,
            (const char *)NULL);
        ctx->veth_created = false;
    }
}

/**
 * Close all file descriptors above STDERR_FILENO.
 *
 * Phase 3 correction §3.5, carried forward: after clone(), the child
 * inherits every open fd from the parent. Any fd referencing the host
 * filesystem survives pivot_root + umount and can be used to escape
 * the mount namespace (CVE-2024-21626, CVE-2016-9962).
 *
 * Each new exec module needs its own static copy because uts.c/cgroup.c
 * declare theirs as `static`. Phase 7's *_exec() refactor will consolidate.
 */
static void close_inherited_fds(bool enable_debug) {
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) {
        /* /proc may not be mounted (user namespace restriction) — fall back
         * to a brute-force close of fds 3..RLIMIT_NOFILE. */
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
 * Child function for containerized process.
 *
 * Lifecycle:
 *   1. Wait on sync pipe (if user namespace OR network active — see §3.2)
 *   2. setup_uts(hostname) if hostname set
 *   3. setup_rootfs(rootfs_path) if rootfs_path set
 *   4. mount_proc() with graceful degradation under user namespace
 *   5. configure_container_net() if network active. If rootfs was set,
 *      pivot_root has already run and /sbin/ip must exist inside the
 *      rootfs (see §3.4 and §0 Prerequisites); without rootfs the host's
 *      /sbin/ip is used.
 *   6. close_inherited_fds() — CVE-2024-21626/CVE-2016-9962 fix
 *   7. execve(program, argv, envp)
 *
 * The veth name `args->net_ctx.veth_container` was generated by the
 * parent BEFORE clone() (§3.4.1), so it's already populated in the
 * child's copy of child_args.
 */
static int child_func(void *arg) {
    child_args_t *args = (child_args_t *)arg;

    /* If a sync pipe is present (user namespace OR network), wait for the
     * parent's signal that all parent-side setup is done. */
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

    /* Set hostname (in UTS namespace if active) */
    if (args->hostname) {
        if (setup_uts(args->hostname, args->enable_debug) < 0) {
            fprintf(stderr, "[child] Failed to setup UTS\n");
            return 1;
        }
    }

    /* Setup rootfs (pivot_root dance, Phase 2-3) */
    if (args->rootfs_path) {
        if (setup_rootfs(args->rootfs_path, args->enable_debug) < 0) {
            fprintf(stderr, "[child] Failed to setup rootfs\n");
            return 1;
        }

        /* Mount /proc with Phase 4b graceful degradation. AppArmor on Ubuntu
         * may deny proc mounts inside an unprivileged user namespace; in
         * that case downgrade to warning rather than failing the container. */
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

    /* Configure network (Phase 6). Uses /sbin/ip from inside the rootfs
     * when rootfs was set (pivot_root has already happened), or from the
     * host otherwise. */
    if (args->network_active) {
        if (configure_container_net(&args->net_ctx, &args->veth,
                                    args->enable_debug) < 0) {
            fprintf(stderr, "[child] Failed to configure container network\n");
            return 1;
        }
    }

    /* Phase 3 §3.5: close inherited fds before execve.
     * Must be AFTER mount_proc so /proc/self/fd is available (fast path). */
    close_inherited_fds(args->enable_debug);

    /* Phase 3 correction: args->envp is always the clean container env
     * from build_container_env(). Never fall back to host environ. */
    execve(args->program, args->argv, args->envp);

    perror("execve");
    return 127;
}

/**
 * Execute process with optional network namespace isolation.
 *
 * Three-phase lifecycle (matches Phase 5 cgroup pattern):
 *   PRE-CLONE: setup_cgroup, setup_overlay
 *   AT-CLONE:  clone() with all configured namespace flags
 *   POST-CLONE-PRE-SYNC: write UID/GID maps, move veth into netns
 *   AT-SYNC:   signal child to proceed
 *   POST-CLONE-POST-SYNC: add_pid_to_cgroup
 *   AT-EXIT:   teardown_overlay, cleanup_net (in net_cleanup)
 */
net_result_t net_exec(const net_config_t *config) {
    net_result_t result = {0};
    overlay_context_t overlay_ctx = {0};
    bool overlay_active = false;
    int sync_pipe[2] = {-1, -1};

    /* Validate */
    if (!config || !config->program || !config->argv) {
        fprintf(stderr, "net_exec: invalid config\n");
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

    /* If network is enabled, verify `ip` is present early so we fail loudly
     * here instead of midway through clone+sync. */
    if (config->enable_network && !find_ip_binary()) {
        fprintf(stderr, "[parent] --net requires /sbin/ip, /usr/sbin/ip, "
                        "or /bin/ip; install the `iproute2` package\n");
        result.child_pid = -1;
        return result;
    }

    /* Step 1: Setup cgroup BEFORE clone (Phase 5) */
    if (config->enable_cgroup) {
        if (setup_cgroup(&result.cgroup_ctx, &config->cgroup_limits,
                         config->enable_debug) < 0) {
            fprintf(stderr, "[parent] Failed to setup cgroup\n");
            result.child_pid = -1;
            return result;
        }
    }

    /* Step 2: Create sync pipe if user namespace OR network is active.
     * Both subsystems need the parent-after-clone handshake. */
    bool needs_sync = config->enable_user_namespace || config->enable_network;
    if (needs_sync) {
        if (pipe(sync_pipe) < 0) {
            perror("pipe");
            if (config->enable_cgroup) {
                remove_cgroup(&result.cgroup_ctx, config->enable_debug);
            }
            result.child_pid = -1;
            return result;
        }
    }

    /* Step 3: Setup overlay if requested (Phase 3) */
    const char *effective_rootfs = config->rootfs_path;
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

    /* Step 4: Allocate clone stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc");
        if (overlay_active) teardown_overlay(&overlay_ctx, config->enable_debug);
        if (sync_pipe[0] >= 0) { close(sync_pipe[0]); close(sync_pipe[1]); }
        if (config->enable_cgroup) {
            remove_cgroup(&result.cgroup_ctx, config->enable_debug);
        }
        result.child_pid = -1;
        return result;
    }
    result.stack_ptr = stack;

    /* Step 4b: Generate veth names NOW (before clone). The child reads
     * args->net_ctx.veth_container after the sync pipe unblocks; since
     * clone() copies the address space, anything we set AFTER clone()
     * is invisible to the child. See §3.4.1. */
    if (config->enable_network) {
        generate_veth_names(&result.net_ctx);
        if (config->enable_debug) {
            printf("[parent] Generated veth names: %s <-> %s\n",
                   result.net_ctx.veth_host,
                   result.net_ctx.veth_container);
        }
    }

    /* Step 5: Prepare child args. Copy veth config + net context (with
     * pre-generated names) into the args struct so the child gets a
     * stable snapshot. */
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
        .net_ctx = result.net_ctx   /* Names already populated */
    };

    /* Step 6: Build clone flags from config */
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

    /* Step 7: Clone */
    pid_t pid = clone(child_func, stack + STACK_SIZE, flags, &child_args);
    if (pid < 0) {
        perror("clone");
        free(stack);
        if (overlay_active) teardown_overlay(&overlay_ctx, config->enable_debug);
        if (sync_pipe[0] >= 0) { close(sync_pipe[0]); close(sync_pipe[1]); }
        if (config->enable_cgroup) {
            remove_cgroup(&result.cgroup_ctx, config->enable_debug);
        }
        result.child_pid = -1;
        result.stack_ptr = NULL;
        return result;
    }
    result.child_pid = pid;

    if (config->enable_debug) printf("[parent] Child PID: %d\n", pid);

    /* Step 8: Close read end in parent (child owns it now) */
    if (sync_pipe[0] >= 0) {
        close(sync_pipe[0]);
        sync_pipe[0] = -1;
    }

    /* Step 9: Setup user namespace mapping if requested */
    if (config->enable_user_namespace) {
        /* See cgroup.c §3.1 / decisions.md #21 for the synthetic-config
         * rationale. Phase 7 will hoist these fields into a shared mapping
         * struct used by both modules. */
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
            if (sync_pipe[1] >= 0) close(sync_pipe[1]);
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            free(stack);
            if (overlay_active) teardown_overlay(&overlay_ctx, config->enable_debug);
            if (config->enable_cgroup) {
                remove_cgroup(&result.cgroup_ctx, config->enable_debug);
            }
            result.child_pid = -1;
            result.stack_ptr = NULL;
            return result;
        }
    }

    /* Step 10: Setup network (veth pair + move container end into netns) */
    if (config->enable_network) {
        if (setup_net(&result.net_ctx, &config->veth, pid,
                      config->enable_debug) < 0) {
            fprintf(stderr, "[parent] Failed to setup network\n");
            if (sync_pipe[1] >= 0) close(sync_pipe[1]);
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            free(stack);
            if (overlay_active) teardown_overlay(&overlay_ctx, config->enable_debug);
            cleanup_net(&result.net_ctx, config->enable_debug);
            if (config->enable_cgroup) {
                remove_cgroup(&result.cgroup_ctx, config->enable_debug);
            }
            result.child_pid = -1;
            result.stack_ptr = NULL;
            return result;
        }

        /* No post-clone copy of net_ctx into child_args is needed —
         * the veth names were generated at Step 4b (before clone) and
         * are already in the child's copy of child_args.net_ctx. The
         * other fields setup_net populates (veth_created, nat_added,
         * nat_source_cidr) are not read by the child; only the parent
         * uses them during cleanup. */
    }

    /* Step 11: Signal child to proceed (after maps + veth move) */
    if (needs_sync) {
        if (write(sync_pipe[1], "1", 1) < 0) {
            perror("write(sync_pipe)");
        }
        close(sync_pipe[1]);
        sync_pipe[1] = -1;
        if (config->enable_debug) printf("[parent] Signaled child to proceed\n");
    }

    /* Step 12: Add child to cgroup AFTER sync (Phase 5 §3.2 ordering).
     * The cgroup filesystem checks permissions against the mapped UID;
     * if we did this before the maps were written we'd see EACCES. */
    if (config->enable_cgroup) {
        if (add_pid_to_cgroup(&result.cgroup_ctx, pid,
                              config->enable_debug) < 0) {
            fprintf(stderr, "[parent] Failed to add PID to cgroup\n");
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            free(stack);
            if (overlay_active) teardown_overlay(&overlay_ctx, config->enable_debug);
            cleanup_net(&result.net_ctx, config->enable_debug);
            remove_cgroup(&result.cgroup_ctx, config->enable_debug);
            result.child_pid = -1;
            result.stack_ptr = NULL;
            return result;
        }
    }

    /* Step 13: Wait for child */
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        result.exit_status = -1;
        if (overlay_active) teardown_overlay(&overlay_ctx, config->enable_debug);
        cleanup_net(&result.net_ctx, config->enable_debug);
        return result;
    }

    /* Step 14: Parse status */
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

    /* Step 15: Teardown overlay (cgroup and net cleanup deferred to
     * net_cleanup() so callers can inspect the contexts first) */
    if (overlay_active) teardown_overlay(&overlay_ctx, config->enable_debug);

    return result;
}

void net_cleanup(net_result_t *result) {
    if (!result) return;

    /* Delete host veth + iptables rule. cleanup_net is self-contained:
     * it uses ctx->nat_source_cidr (populated by setup_net) to delete
     * the matching iptables rule, and deletes the host veth (which
     * also removes the container end). */
    cleanup_net(&result->net_ctx, false);

    /* Phase 5: remove the cgroup directory. Must be empty (no procs);
     * the child has already exited by the time we reach here. */
    remove_cgroup(&result->cgroup_ctx, false);

    /* Phase 1: free the clone stack. */
    if (result->stack_ptr) {
        free(result->stack_ptr);
        result->stack_ptr = NULL;
    }
}