#include "uts.h"    // New in phase 4
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>

#define MAX_ENV_ENTRIES 256
#define DEFAULT_PATH "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

/**
 * Build container environment (Phase 3 correction, carried forward).
 * See Phase 3 §3.4 for motivation.
 */
static char **build_container_env(char *const *custom_env, bool enable_debug) {
    char *defaults[] = {
        "PATH=" DEFAULT_PATH,
        "HOME=/root",
        "TERM=xterm",
        NULL
    };

    // Count defaults
    int count = 0;
    while (defaults[count]) count++;

    // Count custom entries
    int custom_count = 0;
    if (custom_env) {
        while (custom_env[custom_count]) custom_count++;
    }

    // Allocate array (defaults + custom + NULL terminator)
    char **env = malloc((count + custom_count + 1) * sizeof(char *));
    if (!env) {
        perror("malloc");
        return NULL;
    }

    // Copy defaults
    for (int i = 0; i < count; i++) {
        env[i] = defaults[i];
    }
    env[count] = NULL;

    // Apply custom entries — replace on matching key, append if new
    for (int i = 0; i < custom_count; i++) {
        // Find the key length (everything before '=')
        const char *eq = strchr(custom_env[i], '=');
        if (!eq) continue;  // Skip malformed entries
        size_t key_len = eq - custom_env[i];

        // Search existing entries for matching key
        bool replaced = false;
        for (int j = 0; env[j]; j++) {
            if (strncmp(env[j], custom_env[i], key_len + 1) == 0) {
                // Key matches (including the '=') — replace
                env[j] = custom_env[i];
                replaced = true;
                break;
            }
        }

        if (!replaced) {
            // New key — append
            env[count] = custom_env[i];
            count++;
            env[count] = NULL;
        }
    }

    if (enable_debug) {
        printf("[parent] Container environment:\n");
        for (int i = 0; env[i]; i++) {
            printf("[parent]   %s\n", env[i]);
        }
    }

    return env;
}

static void usage(const char *progname) {
    fprintf(stderr, "Usage: %s [OPTIONS] <command> [args...]\n", progname);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --debug              Enable debug output\n");
    fprintf(stderr, "  --pid                Enable PID namespace\n");
    fprintf(stderr, "  --rootfs <path>      Path to root filesystem\n");
    fprintf(stderr, "  --overlay            Enable copy-on-write overlay\n");
    fprintf(stderr, "  --container-dir <p>  Directory for overlay data (default: ./containers)\n");
    fprintf(stderr, "  --hostname <name>  Set container hostname\n");
    fprintf(stderr, "  --env KEY=VALUE      Set environment variable (repeatable)\n");
    fprintf(stderr, "  --help               Show this help\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s --pid --rootfs ./rootfs /bin/sh\n", progname);
    fprintf(stderr, "  %s --pid --rootfs ./rootfs --overlay /bin/sh\n", progname);
    fprintf(stderr, "  %s --pid --rootfs ./rootfs --env PATH=/custom --env FOO=bar /bin/sh\n", progname);
    fprintf(stderr, "  sudo %s --pid --rootfs ./rootfs --hostname web /bin/sh\n", progname); // New in phase 4
}

int main(int argc, char *argv[]) {
    bool enable_debug = false;
    bool enable_pid_namespace = false;
    bool enable_overlay = false;
    char *rootfs_path = NULL;
    char *container_dir = NULL;
    char *hostname = NULL; // New in phase 4

    // Phase 3 correction §3.4: collect --env flags for build_container_env()
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
        {"env",           required_argument, NULL, 'e'},
        {"help",          no_argument,       NULL, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "+dpr:oc:e:h", long_options, NULL)) != -1) {
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

    // Validate: overlay requires rootfs
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
        // Keep known_flags in sync with long_options[].
        static const char *known_flags[] = {
            "--debug", "--pid", "--rootfs", "--overlay",
            "--container-dir",  "--hostname", "--env", "--help", NULL
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

    // Phase 3 correction §3.4: build container environment
    char **container_env = build_container_env(
        env_count > 0 ? custom_env : NULL,
        enable_debug
    );
    if (!container_env) {
        return 1;
    }

    // Configure
    uts_config_t config = {
        .program = argv[optind],
        .argv = &argv[optind],
        .envp = container_env,
        .enable_debug = enable_debug,
        .enable_pid_namespace = enable_pid_namespace || (rootfs_path != NULL),
        .enable_mount_namespace = (rootfs_path != NULL),
        .enable_uts_namespace = (hostname != NULL),
        .rootfs_path = rootfs_path,
        .enable_overlay = enable_overlay,
        .container_dir = container_dir,
        .hostname = hostname
    };

    // Execute
    uts_result_t result = uts_exec(&config);

    // Cleanup
    uts_cleanup(&result);
    free(container_env);  // Free the pointer array (not the strings)

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