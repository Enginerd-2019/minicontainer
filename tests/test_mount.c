#include "mount.h"
#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

void test_rootfs_isolation() {
    char *argv[] = {"/bin/sh", "-c", "ls / | wc -l", NULL};
    mount_config_t config = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = NULL,
        .enable_debug = false,
        .enable_pid_namespace = true,
        .enable_mount_namespace = true,
        .rootfs_path = "./rootfs"
    };

    mount_result_t result = mount_exec(&config);
    mount_cleanup(&result);

    assert(result.child_pid > 0);
    assert(result.exited_normally);
    assert(result.exit_status == 0);

    printf("✓ test_rootfs_isolation passed\n");
}

void test_proc_mount() {
    char *argv[] = {"/bin/sh", "-c", "mount | grep proc", NULL};
    mount_config_t config = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = NULL,
        .enable_debug = false,
        .enable_pid_namespace = true,
        .enable_mount_namespace = true,
        .rootfs_path = "./rootfs"
    };

    mount_result_t result = mount_exec(&config);
    mount_cleanup(&result);

    assert(result.exited_normally);
    assert(result.exit_status == 0);  // grep found "proc"

    printf("✓ test_proc_mount passed\n");
}

int main() {
    if (geteuid() != 0) {
        fprintf(stderr, "Tests must run as root\n");
        return 1;
    }

    // Check if rootfs exists
    struct stat st;
    if (stat("./rootfs/bin/sh", &st) < 0) {
        fprintf(stderr, "Error: ./rootfs not found\n");
        fprintf(stderr, "Please build the rootfs first (see Section 10.1)\n");
        return 1;
    }

    test_rootfs_isolation();
    test_proc_mount();

    printf("\nAll tests passed! ✓\n");
    return 0;
}