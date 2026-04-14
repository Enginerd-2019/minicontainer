#include "overlay.h"
#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_ENV_ENTRIES 256
#define DEFAULT_PATH "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

static char **build_test_env(void) {
    char *defaults[] = { "PATH=" DEFAULT_PATH, "HOME=/root", "TERM=xterm", NULL };
    int count = 0;
    while (defaults[count]) count++;
    char **env = calloc(count + 1, sizeof(char *));
    if (!env) return NULL;
    for (int i = 0; i < count; i++) env[i] = defaults[i];
    env[count] = NULL;
    return env;
}

void test_overlay_base_image_untouched() {
    // Create a marker file
    FILE *fp = fopen("./rootfs/tmp/test_marker.txt", "w");
    assert(fp);
    fprintf(fp, "original");
    fclose(fp);

    // Run container that modifies the marker
    char **env = build_test_env();
    char *argv[] = {"/bin/sh", "-c",
        "echo modified > /tmp/test_marker.txt", NULL};
    overlay_config_t config = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = env,
        .enable_debug = false,
        .enable_pid_namespace = true,
        .enable_mount_namespace = true,
        .rootfs_path = "./rootfs",
        .enable_overlay = true,
        .container_dir = "./test_containers"
    };

    overlay_result_t result = overlay_exec(&config);
    overlay_cleanup(&result);
    free(env);

    assert(result.child_pid > 0);
    assert(result.exited_normally);
    assert(result.exit_status == 0);

    // Verify base image is untouched
    fp = fopen("./rootfs/tmp/test_marker.txt", "r");
    assert(fp);
    char buf[64];
    fgets(buf, sizeof(buf), fp);
    fclose(fp);
    assert(strncmp(buf, "original", 8) == 0);

    // Cleanup marker
    unlink("./rootfs/tmp/test_marker.txt");

    printf("PASS: test_overlay_base_image_untouched\n");
}

void test_overlay_cleanup() {
    char **env = build_test_env();
    char *argv[] = {"/bin/sh", "-c", "echo hello", NULL};
    overlay_config_t config = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = env,
        .enable_debug = false,
        .enable_pid_namespace = true,
        .enable_mount_namespace = true,
        .rootfs_path = "./rootfs",
        .enable_overlay = true,
        .container_dir = "./test_containers"
    };

    overlay_result_t result = overlay_exec(&config);
    overlay_cleanup(&result);
    free(env);

    assert(result.exited_normally);

    printf("PASS: test_overlay_cleanup\n");
}

void test_no_overlay_backward_compat() {
    char **env = build_test_env();
    char *argv[] = {"/bin/sh", "-c", "ls /", NULL};
    overlay_config_t config = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = env,
        .enable_debug = false,
        .enable_pid_namespace = true,
        .enable_mount_namespace = true,
        .rootfs_path = "./rootfs",
        .enable_overlay = false,   // No overlay
        .container_dir = NULL
    };

    overlay_result_t result = overlay_exec(&config);
    overlay_cleanup(&result);
    free(env);

    assert(result.child_pid > 0);
    assert(result.exited_normally);
    assert(result.exit_status == 0);

    printf("PASS: test_no_overlay_backward_compat\n");
}

int main() {
    if (geteuid() != 0) {
        fprintf(stderr, "Tests must run as root (sudo)\n");
        return 1;
    }

    // Check if rootfs exists
    struct stat st;
    if (stat("./rootfs/bin/sh", &st) < 0) {
        fprintf(stderr, "Error: ./rootfs not found\n");
        fprintf(stderr, "Please set up rootfs first (see Phase 2)\n");
        return 1;
    }

    test_overlay_base_image_untouched();
    test_overlay_cleanup();
    test_no_overlay_backward_compat();

    printf("\nAll overlay tests passed!\n");
    return 0;
}