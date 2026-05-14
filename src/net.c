// Note: _GNU_SOURCE is provided by the Makefile via -D_GNU_SOURCE.
// Do NOT redefine it here (Error #8 from decisions.md).
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>        // va_list / va_start / va_arg / va_end
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>

#define MAX_IP_ARGS 16

/* Path to the `ip` binary. Discovered at runtime via find_ip_binary().
 * Three paths are checked: /sbin/ip and /usr/sbin/ip cover the typical
 * host install (Ubuntu/Debian/RHEL); /bin/ip covers the rootfs we build
 * with scripts/build_rootfs.sh, which copies every BIN into /bin/.
 *
 * find_ip_binary() is called from BOTH sides of clone():
 *  - Parent (early check in container_exec, before clone) — uses host paths.
 *  - Child (configure_container_net, after pivot_root) — uses the
 *    container's rootfs paths.
 * Phase 7a: promoted from `static` to a public function so core.c's
 * early-failure check can call it. */
#define IP_BIN_SBIN     "/sbin/ip"
#define IP_BIN_USR_SBIN "/usr/sbin/ip"
#define IP_BIN_BIN      "/bin/ip"

/**
 * Locate /sbin/ip, /usr/sbin/ip, or /bin/ip. Returns a static path
 * or NULL if none of the three locations holds an executable `ip`.
 * Checked in that order so the host's typical install (/sbin/ip on
 * Debian/Ubuntu, /usr/sbin/ip on RHEL-style) wins over the rootfs
 * location, which means parent-side calls don't pick up a rootfs
 * binary by accident.
 */
const char *find_ip_binary(void) {
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
