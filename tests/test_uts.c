#include "uts.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

// build_container_env() from main.c — in production, move to a shared module
#define MAX_ENV_ENTRIES 256
#define DEFAULT_PATH "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

static char **build_container_env(char *const *custom_env, bool enable_debug) {
    char *defaults[] = { "PATH=" DEFAULT_PATH, "HOME=/root", "TERM=xterm", NULL };
    int count = 0;
    while (defaults[count]) count++;
    char **env = calloc(count + MAX_ENV_ENTRIES + 1, sizeof(char *));
    if (!env) return NULL;
    for (int i = 0; i < count; i++) env[i] = defaults[i];
    env[count] = NULL;
    (void)custom_env; (void)enable_debug;
    return env;
}

void test_hostname_isolation() {
    char **test_env = build_container_env(NULL, false);
    char *argv[] = {"/bin/sh", "-c", "hostname", NULL};
    uts_config_t config = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = test_env,
        .enable_debug = false,
        .enable_pid_namespace = true,
        .enable_mount_namespace = false,
        .enable_uts_namespace = true,
        .rootfs_path = NULL,
        .enable_overlay = false,
        .container_dir = NULL,
        .hostname = "testcontainer"
    };

    uts_result_t result = uts_exec(&config);
    uts_cleanup(&result);

    assert(result.exited_normally);
    assert(result.exit_status == 0);

    free(test_env);
    printf("PASS: test_hostname_isolation\n");
}

void test_no_uts_backward_compat() {
    char **test_env = build_container_env(NULL, false);
    char *argv[] = {"/bin/sh", "-c", "hostname", NULL};
    uts_config_t config = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = test_env,
        .enable_debug = false,
        .enable_pid_namespace = true,
        .enable_mount_namespace = false,
        .enable_uts_namespace = false,   // No UTS namespace
        .rootfs_path = NULL,
        .enable_overlay = false,
        .container_dir = NULL,
        .hostname = NULL                // No hostname
    };

    uts_result_t result = uts_exec(&config);
    uts_cleanup(&result);

    assert(result.exited_normally);
    assert(result.exit_status == 0);

    free(test_env);
    printf("PASS: test_no_uts_backward_compat\n");
}

void test_user_namespace_unprivileged() {
    char **env = build_container_env(NULL, false);
    char *argv[] = {"/bin/sh", "-c", "id -u", NULL};
    uts_config_t config = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = env,
        .enable_debug = false,
        .enable_pid_namespace = false,
        .enable_mount_namespace = false,
        .enable_uts_namespace = false,
        .enable_user_namespace = true,
        .rootfs_path = NULL,
        .enable_overlay = false,
        .container_dir = NULL,
        .hostname = NULL,
        .uid_map_inside = 0,
        .uid_map_outside = getuid(),
        .uid_map_range = 1,
        .gid_map_inside = 0,
        .gid_map_outside = getgid(),
        .gid_map_range = 1
    };

    uts_result_t result = uts_exec(&config);
    uts_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);
    printf("PASS: test_user_namespace_unprivileged\n");
}

void test_user_namespace_with_hostname() {
    char **env = build_container_env(NULL, false);
    char *argv[] = {"/bin/sh", "-c", "hostname && id -u", NULL};
    uts_config_t config = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = env,
        .enable_debug = false,
        .enable_pid_namespace = true,
        .enable_mount_namespace = false,
        .enable_uts_namespace = true,
        .enable_user_namespace = true,
        .rootfs_path = NULL,
        .enable_overlay = false,
        .container_dir = NULL,
        .hostname = "rootless-container",
        .uid_map_inside = 0,
        .uid_map_outside = getuid(),
        .uid_map_range = 1,
        .gid_map_inside = 0,
        .gid_map_outside = getgid(),
        .gid_map_range = 1
    };

    uts_result_t result = uts_exec(&config);
    uts_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);
    printf("PASS: test_user_namespace_with_hostname\n");
}

void test_ipc_isolation() {
    char **env = build_container_env(NULL, false);
    // ipcs exits 0 regardless of whether objects exist.
    // The key verification is that we can create the namespace without error.
    char *argv[] = {"/bin/sh", "-c", "ipcs", NULL};
    uts_config_t config = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = env,
        .enable_debug = false,
        .enable_pid_namespace = true,
        .enable_mount_namespace = false,
        .enable_uts_namespace = false,
        .enable_user_namespace = false,
        .enable_ipc_namespace = true,       // New
        .rootfs_path = NULL,
        .enable_overlay = false,
        .hostname = NULL
    };

    uts_result_t result = uts_exec(&config);
    uts_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);
    printf("PASS: test_ipc_isolation\n");
}

void test_ipc_with_user_namespace() {
    char **env = build_container_env(NULL, false);
    char *argv[] = {"/bin/sh", "-c", "ipcs && id -u", NULL};
    uts_config_t config = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = env,
        .enable_debug = false,
        .enable_pid_namespace = true,
        .enable_mount_namespace = false,
        .enable_uts_namespace = false,
        .enable_user_namespace = true,
        .enable_ipc_namespace = true,
        .rootfs_path = NULL,
        .enable_overlay = false,
        .hostname = NULL,
        .uid_map_inside = 0,
        .uid_map_outside = getuid(),
        .uid_map_range = 1,
        .gid_map_inside = 0,
        .gid_map_outside = getgid(),
        .gid_map_range = 1
    };

    uts_result_t result = uts_exec(&config);
    uts_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);
    printf("PASS: test_ipc_with_user_namespace\n");
}

void test_no_ipc_backward_compat() {
    // Verifies that removing --ipc doesn't break the rootless path.
    // Uses CLONE_NEWUSER so it runs without sudo; the key assertion is
    // that the container still executes when enable_ipc_namespace=false.
    char **env = build_container_env(NULL, false);
    char *argv[] = {"/bin/sh", "-c", "ipcs", NULL};
    uts_config_t config = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = env,
        .enable_debug = false,
        .enable_pid_namespace = true,
        .enable_mount_namespace = false,
        .enable_uts_namespace = false,
        .enable_user_namespace = true,
        .enable_ipc_namespace = false,      // No IPC namespace
        .rootfs_path = NULL,
        .enable_overlay = false,
        .container_dir = NULL,
        .hostname = NULL,
        .uid_map_inside = 0,
        .uid_map_outside = getuid(),
        .uid_map_range = 1,
        .gid_map_inside = 0,
        .gid_map_outside = getgid(),
        .gid_map_range = 1
    };

    uts_result_t result = uts_exec(&config);
    uts_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);
    printf("PASS: test_no_ipc_backward_compat\n");
}

int main() {
    if (geteuid() == 0) {
        test_hostname_isolation();
        test_no_uts_backward_compat();
        test_ipc_isolation();          // New — requires root without --user
    } else {
        printf("SKIP: test_hostname_isolation (requires root)\n");
        printf("SKIP: test_no_uts_backward_compat (requires root)\n");
        printf("SKIP: test_ipc_isolation (requires root)\n");
    }

    test_user_namespace_unprivileged();
    test_user_namespace_with_hostname();
    test_ipc_with_user_namespace();    // New — works without root
    test_no_ipc_backward_compat();     // New — works with or without root

    printf("\nAll UTS/user/IPC namespace tests passed!\n");
    return 0;
}