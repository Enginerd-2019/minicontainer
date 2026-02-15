#define _GNU_SOURCE
#include "namespace.h"
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <stdbool.h>

extern char **environ;

static void usage(const char *progname) {
    fprintf(stderr, "Usage: %s [OPTIONS] <command> [args...]\n", progname);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --debug         Enable debug output\n");
    fprintf(stderr, "  --pid           Enable PID namespace\n");
    fprintf(stderr, "  --env KEY=VALUE Set environment variable\n");
    fprintf(stderr, "  --help          Show this help\n");
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  %s --pid /bin/sh -c 'echo $$'  # Should print 1\n", progname);
    fprintf(stderr, "  %s --pid --env FOO=bar /bin/sh -c 'echo $FOO'\n", progname);
}

int main(int argc, char *argv[]) {
    bool enable_debug = false;
    bool enable_pid_namespace = false;

    // Environment variable storage (from Phase 0)
    char *custom_env[256] = {NULL};
    int env_count = 0;

    static struct option long_options[] = {
        {"debug", no_argument, NULL, 'd'},
        {"pid", no_argument, NULL, 'p'},
        {"env", required_argument, NULL, 'e'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    // NOTE: '+' prefix stops parsing at first non-option (fixes Phase 0 bug)
    while ((opt = getopt_long(argc, argv, "+dpe:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'd':
                enable_debug = true;
                break;
            case 'p':
                enable_pid_namespace = true;
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

    // Build envp (either custom or inherit) - from Phase 0
    char **envp = NULL;
    if (env_count > 0) {
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

    // Configure namespace
    namespace_config_t config = {
        .program = argv[optind],
        .argv = &argv[optind],
        .envp = envp,
        .enable_debug = enable_debug,
        .enable_pid_namespace = enable_pid_namespace
    };

    // Execute
    namespace_result_t result = namespace_exec(&config);

    // Cleanup
    namespace_cleanup(&result);
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