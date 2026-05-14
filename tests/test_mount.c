// Note: _GNU_SOURCE is provided by the Makefile.
// Phase 7a: this test was originally written against mount_exec() with
// mount_config_t. After the execution-core consolidation, it calls
// container_exec() with container_config_t — same semantics, unified API.
#include "core.h"
#include "env.h"
#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>

void test_rootfs_isolation(void) {
    char *argv[] = {"/bin/sh", "-c", "ls / | wc -l", NULL};
    char **env = build_container_env(NULL, false);

    container_config_t cfg = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = env,
        .enable_debug = false,
        .enable_pid_namespace = true,
        .enable_mount_namespace = true,
        .rootfs_path = "./rootfs",
        .uid_map_inside = 0,
        .uid_map_outside = getuid(),
        .uid_map_range = 1,
        .gid_map_inside = 0,
        .gid_map_outside = getgid(),
        .gid_map_range = 1,
    };

    container_result_t result = container_exec(&cfg);
    container_cleanup(&result);
    free(env);

    assert(result.child_pid > 0);
    assert(result.exited_normally);
    assert(result.exit_status == 0);

    printf("PASS: test_rootfs_isolation\n");
}

void test_proc_mount(void) {
    char *argv[] = {"/bin/sh", "-c", "mount | grep proc", NULL};
    char **env = build_container_env(NULL, false);

    container_config_t cfg = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = env,
        .enable_debug = false,
        .enable_pid_namespace = true,
        .enable_mount_namespace = true,
        .rootfs_path = "./rootfs",
        .uid_map_inside = 0,
        .uid_map_outside = getuid(),
        .uid_map_range = 1,
        .gid_map_inside = 0,
        .gid_map_outside = getgid(),
        .gid_map_range = 1,
    };

    container_result_t result = container_exec(&cfg);
    container_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);  // grep found "proc"

    printf("PASS: test_proc_mount\n");
}

int main(void) {
    if (geteuid() != 0) {
        fprintf(stderr, "Tests must run as root\n");
        return 1;
    }

    // Check if rootfs exists
    struct stat st;
    if (stat("./rootfs/bin/sh", &st) < 0) {
        fprintf(stderr, "Error: ./rootfs not found\n");
        fprintf(stderr, "Run scripts/build_rootfs.sh first.\n");
        return 1;
    }

    test_rootfs_isolation();
    test_proc_mount();

    printf("\nAll mount tests passed!\n");
    return 0;
}
