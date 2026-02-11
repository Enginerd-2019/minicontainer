#include "spawn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <stdbool.h>

// External environ variable (provided by libc)
extern char **environ;

static void usage(const char *progname) {
    fprintf(stderr, "Usage: %s [OPTIONS] <command> [args...]\n", progname);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --debug         Enable debug output\n");
    fprintf(stderr, "  --env KEY=VALUE Set environment variable\n");
    fprintf(stderr, "  --help          Show this help\n");
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  %s /bin/ls -la\n", progname);
    fprintf(stderr, "  %s --env PATH=/bin --env HOME=/root /bin/sh\n", progname);
}

int main(int argc, char *argv[]) {
    bool enable_debug = false;

    // Simple environment variable storage (stretch goal)
    char *custom_env[256] = {NULL};
    int env_count = 0;

    // Parse options
    static struct option long_options[] = {
        {"debug", no_argument, NULL, 'd'},
        {"env", required_argument, NULL, 'e'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "+de:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'd':
                enable_debug = true;
                break;
            case 'e':
                if (env_count >= 255) {
                    fprintf(stderr, "Too many environment variables\n");
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

    // Command is required
    if (optind >= argc) {
        fprintf(stderr, "Error: No command specified\n");
        usage(argv[0]);
        return 1;
    }

    // Setup signal handling
    if (spawn_init_signals() < 0) {
        return 1;
    }

    // Build argv for child
    const char *program = argv[optind];
    char **child_argv = &argv[optind];

    // Build envp (either custom or inherit)
    char **envp = NULL;
    if (env_count > 0) {
        // Copy existing environment and append custom vars
        int existing_count = 0;
        while (environ[existing_count]) existing_count++;

        envp = malloc((existing_count + env_count + 1) * sizeof(char *));
        if (!envp) {
            perror("malloc");
            return 1;
        }

        for (int i = 0; i < existing_count; i++) {
            envp[i] = environ[i];
        }
        for (int i = 0; i < env_count; i++) {
            envp[existing_count + i] = custom_env[i];
        }
        envp[existing_count + env_count] = NULL;
    }

    // Configure spawn
    spawn_config_t config = {
        .program = program,
        .argv = child_argv,
        .envp = envp,
        .enable_debug = enable_debug
    };

    // Spawn process
    spawn_result_t result = spawn_process(&config);

    // Cleanup
    if (envp) {
        free(envp);
    }

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