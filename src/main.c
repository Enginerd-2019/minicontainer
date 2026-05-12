// Note: _GNU_SOURCE is provided by the Makefile via -D_GNU_SOURCE.
// Do NOT redefine it here (Error #8 from decisions.md).
#include "cgroup.h"
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>

#define MAX_ENV_ENTRIES 256
#define DEFAULT_PATH "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

/**
 * Build container environment.
 *
 * Originally introduced in Phase 3 (§3.4) to replace the host environ leak
 * after pivot_root. Refactored in Phase 5 (§3.7) to drop caller-invariant
 * assumptions: this version is safe to call from any context, not just
 * main.c with its --env validation.
 *
 * Algorithm:
 *   1. Start with a minimal container baseline (PATH, HOME, TERM).
 *   2. Merge custom entries: if a key already exists, replace; otherwise append.
 *   3. Return a NULL-terminated argv-style array suitable for execve().
 *
 * Ownership: caller owns the returned pointer and must free() it. The
 * function does not duplicate the string contents — they point into
 * defaults[] (static storage) or custom_env[] (caller's storage).
 *
 * @param custom_env  NULL-terminated array of "KEY=VALUE" strings, or NULL.
 * @param enable_debug  Print the final environment for inspection.
 * @return  Heap-allocated, NULL-terminated env array; NULL on allocation failure.
 */
static char **build_container_env(char *const *custom_env, bool enable_debug) {
    /* Minimal container baseline. Matches Docker's default-environment policy:
     * never leak the host's PATH/HOME/SHELL, since those reference host paths
     * that don't exist inside the container rootfs. */
    char *defaults[] = {
        "PATH=" DEFAULT_PATH,
        "HOME=/root",
        "TERM=xterm",
        NULL
    };

    /* Count baseline entries so we can index past them in the merge loop. */
    int count = 0;
    while (defaults[count]) count++;

    /* Over-allocate to a compile-time ceiling rather than sizing exactly to
     * fit custom_env. Reasons (see §3.7):
     *   - calloc zero-initializes every slot, providing an implicit NULL
     *     terminator everywhere we haven't written yet.
     *   - MAX_ENV_ENTRIES is a constant — the size calculation cannot be
     *     wrong, even under integer overflow or a misformed input array.
     *   - The bounds check below caps total entries at max_entries - 1,
     *     so a malformed custom_env can drop entries but never overflow.
     */
    int max_entries = count + MAX_ENV_ENTRIES + 1;
    char **env = calloc(max_entries, sizeof(char *));
    if (!env) {
        perror("calloc");
        return NULL;
    }

    /* Copy the baseline. No NULL-terminator placement needed — calloc set
     * every slot to NULL already. */
    for (int i = 0; i < count; i++) {
        env[i] = defaults[i];
    }

    /* Merge custom entries. Replace existing keys; append new ones. */
    if (custom_env) {
        for (int i = 0; custom_env[i]; i++) {
            /* Locate the key/value separator. Entries without '=' are
             * malformed (no KEY=VALUE form) — skip silently rather than
             * abort, matching shell-style environment behavior. */
            const char *eq = strchr(custom_env[i], '=');
            if (!eq) continue;

            /* Compare on "KEY=" (including the '=') so prefix matches like
             * "PATH" vs "PATHINFO" don't collide. */
            size_t key_len = eq - custom_env[i];
            bool replaced = false;

            for (int j = 0; j < count; j++) {
                if (strncmp(env[j], custom_env[i], key_len + 1) == 0) {
                    env[j] = custom_env[i];  /* In-place key replacement. */
                    replaced = true;
                    break;
                }
            }

            /* Defense-in-depth: even though main.c caps custom_env at
             * MAX_ENV_ENTRIES - 1, check at the append site so this function
             * stays safe when extracted into a shared utility module
             * (Phase 7) and called from contexts that don't pre-validate. */
            if (!replaced && count < max_entries - 1) {
                env[count++] = custom_env[i];
            }
        }
    }

    /* Final NULL terminator. Strictly speaking calloc already placed one
     * here, but explicit termination makes the contract clear and survives
     * any future refactoring that might change the allocator. */
    env[count] = NULL;

    if (enable_debug) {
        printf("[parent] Container environment:\n");
        for (int i = 0; env[i]; i++) {
            printf("[parent]   %s\n", env[i]);
        }
    }

    return env;
}

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
    fprintf(stderr, "  --debug              Enable debug output\n");
    fprintf(stderr, "  --pid                Enable PID namespace\n");
    fprintf(stderr, "  --rootfs <path>      Path to root filesystem\n");
    fprintf(stderr, "  --overlay            Enable copy-on-write overlay\n");
    fprintf(stderr, "  --container-dir <p>  Directory for overlay data (default: ./containers)\n");
    fprintf(stderr, "  --hostname <name>    Set container hostname\n");
    fprintf(stderr, "  --user               Enable user namespace (run without sudo)\n");
    fprintf(stderr, "  --ipc                Enable IPC namespace\n");
    fprintf(stderr, "  --memory <limit>   Memory limit (e.g., 100M, 1G)\n");
    fprintf(stderr, "  --cpus <fraction>  CPU limit (e.g., 0.5 = 50%%)\n");
    fprintf(stderr, "  --pids <max>       Max number of processes\n");
    fprintf(stderr, "  --env KEY=VALUE      Set environment variable (repeatable)\n");
    fprintf(stderr, "  --help               Show this help\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  sudo %s --pid --rootfs ./rootfs --hostname web /bin/sh\n", progname);
    fprintf(stderr, "  %s --user --pid --ipc --hostname test /bin/sh  # No sudo needed\n", progname);
    fprintf(stderr, "  sudo %s --pid --memory 100M --cpus 0.5 --pids 20 /bin/sh\n", progname);
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

    // Phase 3 correction: collect --env flags
    char *custom_env[MAX_ENV_ENTRIES];
    int env_count = 0;
    memset(custom_env, 0, sizeof(custom_env));

    static struct option long_options[] = {
        {"debug",         no_argument,       NULL, 'd'},
        {"pid",           no_argument,       NULL, 'p'},
        {"rootfs",        required_argument, NULL, 'r'},
        {"overlay",       no_argument,       NULL, 'o'},
        {"container-dir", required_argument, NULL, 'c'},
        {"hostname",      required_argument, NULL, 'H'},
        {"user",          no_argument,       NULL, 'u'},
        {"ipc",           no_argument,       NULL, 'I'},
        {"memory",        required_argument, NULL, 'm'},
        {"cpus",          required_argument, NULL, 'C'},
        {"pids",          required_argument, NULL, 'P'},
        {"env",           required_argument, NULL, 'e'},
        {"help",          no_argument,       NULL, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "+dpr:oc:H:uIm:C:P:e:h",
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
        static const char *known_flags[] = {
            "--debug", "--pid", "--rootfs", "--overlay",
            "--container-dir", "--hostname", "--user",
            "--ipc", "--memory", "--cpus", "--pids",
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
    cgroup_config_t config = {
        .program = argv[optind],
        .argv = &argv[optind],
        .envp = container_env,
        .enable_debug = enable_debug,
        // Phase 4b auto-enable: --rootfs implies --pid (carried forward)
        .enable_pid_namespace = enable_pid_namespace || (rootfs_path != NULL),
        .enable_mount_namespace = (rootfs_path != NULL),
        .enable_uts_namespace = (hostname != NULL),
        .enable_user_namespace = enable_user_namespace,
        .enable_ipc_namespace = enable_ipc_namespace,
        .rootfs_path = rootfs_path,
        .enable_overlay = enable_overlay,
        .container_dir = container_dir,
        .hostname = hostname,
        // Default user mapping: container root → current host user
        .uid_map_inside = 0,
        .uid_map_outside = getuid(),
        .uid_map_range = 1,
        .gid_map_inside = 0,
        .gid_map_outside = getgid(),
        .gid_map_range = 1,
        // Cgroup
        .cgroup_limits = limits,
        .enable_cgroup = enable_cgroup
    };

    // Execute
    cgroup_result_t result = cgroup_exec(&config);

    // Cleanup
    cgroup_cleanup(&result);
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