// Phase 7a: was overlay_exec/overlay_config_t — now container_exec.
#include "core.h"
#include "env.h"
#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static container_config_t base_overlay_config(char **env, char *const *argv) {
    container_config_t cfg = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = env,
        .enable_debug = false,
        .enable_pid_namespace = true,
        .enable_mount_namespace = true,
        .rootfs_path = "./rootfs",
        .enable_overlay = true,
        .container_dir = "./test_containers",
        .uid_map_inside = 0,
        .uid_map_outside = getuid(),
        .uid_map_range = 1,
        .gid_map_inside = 0,
        .gid_map_outside = getgid(),
        .gid_map_range = 1,
    };
    return cfg;
}

void test_overlay_base_image_untouched(void) {
    // Create a marker file
    FILE *fp = fopen("./rootfs/tmp/test_marker.txt", "w");
    assert(fp);
    fprintf(fp, "original");
    fclose(fp);

    // Run container that modifies the marker
    char **env = build_container_env(NULL, false);
    char *argv[] = {"/bin/sh", "-c",
        "echo modified > /tmp/test_marker.txt", NULL};
    container_config_t cfg = base_overlay_config(env, argv);

    container_result_t result = container_exec(&cfg);
    container_cleanup(&result);
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

void test_overlay_cleanup(void) {
    char **env = build_container_env(NULL, false);
    char *argv[] = {"/bin/sh", "-c", "echo hello", NULL};
    container_config_t cfg = base_overlay_config(env, argv);

    container_result_t result = container_exec(&cfg);
    container_cleanup(&result);
    free(env);

    assert(result.exited_normally);

    printf("PASS: test_overlay_cleanup\n");
}

void test_no_overlay_backward_compat(void) {
    char **env = build_container_env(NULL, false);
    char *argv[] = {"/bin/sh", "-c", "ls /", NULL};
    container_config_t cfg = base_overlay_config(env, argv);
    cfg.enable_overlay = false;       // disable overlay
    cfg.container_dir  = NULL;

    container_result_t result = container_exec(&cfg);
    container_cleanup(&result);
    free(env);

    assert(result.child_pid > 0);
    assert(result.exited_normally);
    assert(result.exit_status == 0);

    printf("PASS: test_no_overlay_backward_compat\n");
}

int main(void) {
    if (geteuid() != 0) {
        fprintf(stderr, "Tests must run as root (sudo)\n");
        return 1;
    }

    // Check if rootfs exists
    struct stat st;
    if (stat("./rootfs/bin/sh", &st) < 0) {
        fprintf(stderr, "Error: ./rootfs not found\n");
        fprintf(stderr, "Run scripts/build_rootfs.sh first.\n");
        return 1;
    }

    test_overlay_base_image_untouched();
    test_overlay_cleanup();
    test_no_overlay_backward_compat();

    printf("\nAll overlay tests passed!\n");
    return 0;
}
