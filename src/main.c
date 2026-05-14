// Note: _GNU_SOURCE is provided by the Makefile via -D_GNU_SOURCE.
#include "core.h" // Phase 7
#include "env.h"  // Phase 7
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>

/**
 * Parse memory limit string (e.g., "100M", "1G", "512K").
 */
static size_t parse_memory_limit(const char *str) {
    char *endptr;
    double value = strtod(str, &endptr);

    if (*endptr == 'K' || *endptr == 'k') {
        return (size_t)(value * 1024);
    } else if (*endptr == 'M' || *endptr == 'm') {
        return (size_t)(value * 1024 * 1024);
    } else if (*endptr == 'G' || *endptr == 'g') {
        return (size_t)(value * 1024 * 1024 * 1024);
    }

    return (size_t)value;
}

/**
 * Parse CPU limit string (e.g., "0.5" = 50%, "2.0" = 200%).
 */
static long parse_cpu_limit(const char *str) {
    double value = strtod(str, NULL);
    return (long)(value * 100000);
}

static void usage(const char *progname) {
    fprintf(stderr, "Usage: %s [OPTIONS] <command> [args...]\n", progname);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --debug                  Enable debug output\n");
    fprintf(stderr, "  --pid                    Enable PID namespace\n");
    fprintf(stderr, "  --rootfs <path>          Path to root filesystem\n");
    fprintf(stderr, "  --overlay                Enable copy-on-write overlay\n");
    fprintf(stderr, "  --container-dir <p>      Directory for overlay data\n");
    fprintf(stderr, "  --hostname <name>        Set container hostname\n");
    fprintf(stderr, "  --user                   Enable user namespace (run without sudo)\n");
    fprintf(stderr, "  --ipc                    Enable IPC namespace\n");
    fprintf(stderr, "  --memory <limit>         Memory limit (e.g., 100M, 1G)\n");
    fprintf(stderr, "  --cpus <fraction>        CPU limit (e.g., 0.5 = 50%%)\n");
    fprintf(stderr, "  --pids <max>             Max processes\n");
    fprintf(stderr, "  --net                    Enable network namespace + veth pair\n");
    fprintf(stderr, "  --net-host-ip <addr>     Host-side veth IP\n");
    fprintf(stderr, "  --net-container-ip <a>   Container-side veth IP\n");
    fprintf(stderr, "  --net-netmask <cidr>     CIDR suffix\n");
    fprintf(stderr, "  --no-nat                 Disable iptables MASQUERADE\n");
    fprintf(stderr, "  --env KEY=VALUE          Set environment variable (repeatable)\n");
    fprintf(stderr, "  --help                   Show this help\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  sudo %s --pid --rootfs ./rootfs --hostname web /bin/sh\n", progname);
    fprintf(stderr, "  %s --user --pid --ipc --hostname test /bin/sh  # No sudo needed\n", progname);
    fprintf(stderr, "  sudo %s --pid --memory 100M --cpus 0.5 --pids 20 /bin/sh\n", progname);
    fprintf(stderr, "  sudo %s --pid --rootfs ./rootfs --net /bin/sh  # With network\n", progname);
}

int main(int argc, char *argv[]) {
    bool enable_debug = false;
    bool enable_pid_namespace = false;
    bool enable_overlay = false;
    bool enable_user_namespace = false;
    bool enable_ipc_namespace = false;
    char *rootfs_path = NULL;
    char *container_dir = NULL;
    char *hostname = NULL;

    cgroup_limits_t limits = {0};
    bool enable_cgroup = false;

    /* Phase 6: network namespace + veth locals */
    bool enable_network = false;
    char *net_host_ip = NULL;
    char *net_container_ip = NULL;
    char *net_netmask = NULL;
    bool no_nat = false;

    // Phase 3 correction: collect --env flags
    char *custom_env[MAX_ENV_ENTRIES];
    int env_count = 0;
    memset(custom_env, 0, sizeof(custom_env));

    static struct option long_options[] = {
        {"debug",            no_argument,       NULL, 'd'},
        {"pid",              no_argument,       NULL, 'p'},
        {"rootfs",           required_argument, NULL, 'r'},
        {"overlay",          no_argument,       NULL, 'o'},
        {"container-dir",    required_argument, NULL, 'c'},
        {"hostname",         required_argument, NULL, 'H'},
        {"user",             no_argument,       NULL, 'u'},
        {"ipc",              no_argument,       NULL, 'I'},
        {"memory",           required_argument, NULL, 'm'},
        {"cpus",             required_argument, NULL, 'C'},
        {"pids",             required_argument, NULL, 'P'},
        /* Phase 6: new flags. Numeric short-codes >= 1 follow the
         * getopt_long(3) convention for long-only options. */
        {"net",              no_argument,       NULL, 'N'},
        {"net-host-ip",      required_argument, NULL,  1 },
        {"net-container-ip", required_argument, NULL,  2 },
        {"net-netmask",      required_argument, NULL,  3 },
        {"no-nat",           no_argument,       NULL,  4 },
        {"env",              required_argument, NULL, 'e'},
        {"help",             no_argument,       NULL, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    /* Phase 6: added 'N' to the short-option string for --net */
    while ((opt = getopt_long(argc, argv, "+dpr:oc:H:uIm:C:P:Ne:h",
                              long_options, NULL)) != -1) {
        switch (opt) {
            case 'd':
                enable_debug = true;
                break;
            case 'p':
                enable_pid_namespace = true;
                break;
            case 'r':
                rootfs_path = optarg;
                break;
            case 'o':
                enable_overlay = true;
                break;
            case 'c':
                container_dir = optarg;
                break;
            case 'H':
                hostname = optarg;
                break;
            case 'u':
                enable_user_namespace = true;
                break;
            case 'I':
                enable_ipc_namespace = true;
                break;
            case 'm':
                limits.memory_limit = parse_memory_limit(optarg);
                enable_cgroup = true;
                break;
            case 'C':
                limits.cpu_quota = parse_cpu_limit(optarg);
                limits.cpu_period = 100000;
                enable_cgroup = true;
                break;
            case 'P':
                limits.pid_limit = atoi(optarg);
                enable_cgroup = true;
                break;
            /* Phase 6: new cases */
            case 'N':
                enable_network = true;
                break;
            case 1:
                net_host_ip = optarg;
                break;
            case 2:
                net_container_ip = optarg;
                break;
            case 3:
                net_netmask = optarg;
                break;
            case 4:
                no_nat = true;
                break;
            case 'e':
                if (env_count >= MAX_ENV_ENTRIES - 1) {
                    fprintf(stderr, "Too many --env entries\n");
                    return 1;
                }
                custom_env[env_count++] = optarg;
                break;
            case 'h':
                usage(argv[0]);
                return 0;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: No command specified\n");
        usage(argv[0]);
        return 1;
    }

    /* Phase 3 invariant: --overlay requires --rootfs. The overlay filesystem
     * needs a lowerdir (base image), which only exists when --rootfs is
     * provided. Without this check, setup_overlay() would fail with a
     * confusing error deeper in the call stack — better to reject the
     * invalid combination up front. */
    if (enable_overlay && !rootfs_path) {
        fprintf(stderr, "Error: --overlay requires --rootfs\n");
        return 1;
    }

    /* Phase 6 invariant: --net sub-flags require --net. Without this check
     * the IPs/netmask/no-nat would be silently ignored if --net is missing,
     * which is confusing. Fail loudly instead. */
    if ((net_host_ip || net_container_ip || net_netmask || no_nat)
        && !enable_network) {
        fprintf(stderr, "Error: --net-host-ip / --net-container-ip / "
                        "--net-netmask / --no-nat require --net\n");
        return 1;
    }

    // Phase 3 correction §3.7: detect minicontainer flags that landed
    // after the command due to POSIX-strict (+) getopt stopping at the
    // first non-option argument. The '--' separator suppresses this check
    // so child commands can use flag names like --help without conflict.
    bool explicit_separator = false;
    if (optind > 1 && strcmp(argv[optind - 1], "--") == 0) {
        explicit_separator = true;
    }

    if (!explicit_separator) {
        // Keep in sync with long_options[].
        // Phase 5: added "--memory", "--cpus", "--pids"
        // Phase 6: added "--net", "--net-host-ip", "--net-container-ip",
        //                "--net-netmask", "--no-nat"
        static const char *known_flags[] = {
            "--debug", "--pid", "--rootfs", "--overlay",
            "--container-dir", "--hostname", "--user",
            "--ipc", "--memory", "--cpus", "--pids",
            "--net", "--net-host-ip", "--net-container-ip",
            "--net-netmask", "--no-nat",
            "--env", "--help", NULL
        };

        for (int i = optind + 1; i < argc; i++) {
            for (int j = 0; known_flags[j]; j++) {
                if (strcmp(argv[i], known_flags[j]) == 0) {
                    fprintf(stderr,
                        "Error: '%s' appears after command '%s' (argv[%d])\n"
                        "All minicontainer options must precede the command.\n"
                        "Use '--' to pass flags to the child: "
                        "%s [options] -- %s %s\n",
                        argv[i], argv[optind], i,
                        argv[0], argv[optind], argv[i]);
                    return 1;
                }
            }
        }
    }

    // Build container environment (Phase 3 correction)
    char **container_env = build_container_env(
        env_count > 0 ? custom_env : NULL,
        enable_debug
    );
    if (!container_env) {
        return 1;
    }

    // Configure
    container_config_t config = {
        .program = argv[optind],
        .argv = &argv[optind],
        .envp = container_env,
        .enable_debug = enable_debug,
        .enable_pid_namespace = enable_pid_namespace || (rootfs_path != NULL),
        .enable_mount_namespace = (rootfs_path != NULL),
        .enable_uts_namespace = (hostname != NULL),
        .enable_user_namespace = enable_user_namespace,
        .enable_ipc_namespace = enable_ipc_namespace,
        .enable_network = enable_network,
        .rootfs_path = rootfs_path,
        .enable_overlay = enable_overlay,
        .container_dir = container_dir,
        .hostname = hostname,
        .uid_map_inside = 0,
        .uid_map_outside = getuid(),
        .uid_map_range = 1,
        .gid_map_inside = 0,
        .gid_map_outside = getgid(),
        .gid_map_range = 1,
        .cgroup_limits = limits,
        .enable_cgroup = enable_cgroup,
        .veth = {
            .host_ip      = "",
            .container_ip = "",
            .netmask      = "",
            .enable_nat   = !no_nat,
        }
    };

    /* Phase 6: fill veth address fields only when --net is enabled.
     * strncpy + the inline-zeroed buffers above keep the string
     * NUL-terminated even at the buffer's maximum length. */
    if (enable_network) {
        strncpy(config.veth.host_ip,
                net_host_ip ? net_host_ip : "10.0.0.1",
                sizeof(config.veth.host_ip) - 1);
        strncpy(config.veth.container_ip,
                net_container_ip ? net_container_ip : "10.0.0.2",
                sizeof(config.veth.container_ip) - 1);
        strncpy(config.veth.netmask,
                net_netmask ? net_netmask : "24",
                sizeof(config.veth.netmask) - 1);
    }

    // Execute (Phase 7: Unified execution via config struct)
     container_result_t result = container_exec(&config);

    
    container_cleanup(&result);
    free(container_env);

    // Handle result
    if (result.child_pid < 0) {
        fprintf(stderr, "Failed to spawn process\n");
        return 1;
    }

    if (!result.exited_normally) {
        fprintf(stderr, "Process killed by signal %d\n", result.signal);
        return 128 + result.signal;
    }

    return result.exit_status;
}