#include "mount.h"
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <stdbool.h>

extern char **environ;

static void usage(const char *progname) {
    fprintf(stderr, "Usage: %s [OPTIONS] <command> [args...]\n", progname);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --debug           Enable debug output\n");
    fprintf(stderr, "  --pid             Enable PID namespace\n");
    fprintf(stderr, "  --rootfs <path>   Path to root filesystem\n");
    fprintf(stderr, "  --help            Show this help\n");
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  %s --pid --rootfs ./rootfs /bin/sh\n", progname);
}

int main(int argc, char *argv[]) {
    bool enable_debug = false;
    bool enable_pid_namespace = false;
    char *rootfs_path = NULL;

    static struct option long_options[] = {
        {"debug", no_argument, NULL, 'd'},
        {"pid", no_argument, NULL, 'p'},
        {"rootfs", required_argument, NULL, 'r'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "+dpr:h", long_options, NULL)) != -1) {
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

    // Configure
    mount_config_t config = {
        .program = argv[optind],
        .argv = &argv[optind],
        .envp = NULL,
        .enable_debug = enable_debug,
        .enable_pid_namespace = enable_pid_namespace,
        .enable_mount_namespace = (rootfs_path != NULL),  // Auto-enable if rootfs provided
        .rootfs_path = rootfs_path
    };

    // Execute
    mount_result_t result = mount_exec(&config);

    // Cleanup
    mount_cleanup(&result);

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