// Note: _GNU_SOURCE is provided by the Makefile via -D_GNU_SOURCE.
// Do NOT redefine it here (Error #8 from decisions.md).
#include "cgroup.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>   // calloc, free (Error #10, #16 from decisions.md)
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAX_ENV_ENTRIES 256
#define DEFAULT_PATH "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

/**
 * Test-side build_container_env helper.
 * Matches the Phase 4c convention: tests invoke as build_container_env(NULL, false).
 * Ignores custom_env/enable_debug — tests don't need them.
 */
static char **build_container_env(char *const *custom_env, bool enable_debug) {
    char *defaults[] = { "PATH=" DEFAULT_PATH, "HOME=/root", "TERM=xterm", NULL };
    int count = 0;
    while (defaults[count]) count++;
    char **env = calloc(count + 1, sizeof(char *));
    if (!env) return NULL;
    for (int i = 0; i < count; i++) env[i] = defaults[i];
    env[count] = NULL;
    (void)custom_env; (void)enable_debug;
    return env;
}

void test_cgroup_creation_and_cleanup() {
    cgroup_context_t ctx = {0};
    cgroup_limits_t limits = {
        .memory_limit = 100 * 1024 * 1024,  // 100MB
        .cpu_quota = 0,
        .cpu_period = 0,
        .pid_limit = 10
    };

    // Create cgroup
    int rc = setup_cgroup(&ctx, &limits, true);
    assert(rc == 0);
    assert(ctx.created);

    // Verify directory exists
    struct stat st;
    assert(stat(ctx.cgroup_path, &st) == 0);
    assert(S_ISDIR(st.st_mode));

    // Remove cgroup
    remove_cgroup(&ctx, true);
    assert(!ctx.created);

    // Verify directory is gone
    assert(stat(ctx.cgroup_path, &st) < 0);

    printf("PASS: test_cgroup_creation_and_cleanup\n");
}

void test_memory_limit() {
    char **env = build_container_env(NULL, false);
    // Allocate 50MB within a 100MB limit — should succeed
    char *argv[] = {"/bin/sh", "-c", "head -c 50M /dev/zero > /dev/null", NULL};
    cgroup_config_t config = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = env,
        .enable_debug = false,
        .enable_pid_namespace = true,
        .enable_mount_namespace = false,
        .enable_uts_namespace = false,
        .enable_user_namespace = false,
        .enable_ipc_namespace = false,
        .rootfs_path = NULL,
        .enable_overlay = false,
        .hostname = NULL,
        .cgroup_limits = {
            .memory_limit = 100 * 1024 * 1024,  // 100MB
        },
        .enable_cgroup = true
    };

    cgroup_result_t result = cgroup_exec(&config);
    cgroup_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);
    printf("PASS: test_memory_limit\n");
}

void test_pid_limit() {
    char **env = build_container_env(NULL, false);
    // Try to spawn more processes than allowed
    char *argv[] = {"/bin/sh", "-c",
        "for i in $(seq 1 20); do sleep 0.1 & done; wait", NULL};
    cgroup_config_t config = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = env,
        .enable_debug = false,
        .enable_pid_namespace = true,
        .enable_mount_namespace = false,
        .enable_uts_namespace = false,
        .enable_user_namespace = false,
        .enable_ipc_namespace = false,
        .rootfs_path = NULL,
        .enable_overlay = false,
        .hostname = NULL,
        .cgroup_limits = {
            .pid_limit = 5,
        },
        .enable_cgroup = true
    };

    cgroup_result_t result = cgroup_exec(&config);
    cgroup_cleanup(&result);
    free(env);

    // The shell should still exit (fork failures don't kill the shell)
    assert(result.exited_normally);
    printf("PASS: test_pid_limit\n");
}

void test_no_cgroup_backward_compat() {
    char **env = build_container_env(NULL, false);
    char *argv[] = {"/bin/echo", "hello", NULL};
    cgroup_config_t config = {
        .program = "/bin/echo",
        .argv = argv,
        .envp = env,
        .enable_debug = false,
        .enable_pid_namespace = true,
        .enable_mount_namespace = false,
        .enable_uts_namespace = false,
        .enable_user_namespace = false,
        .enable_ipc_namespace = false,
        .rootfs_path = NULL,
        .enable_overlay = false,
        .hostname = NULL,
        .enable_cgroup = false  // No cgroup
    };

    cgroup_result_t result = cgroup_exec(&config);
    cgroup_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);
    printf("PASS: test_no_cgroup_backward_compat\n");
}

void test_cgroup_with_ipc_namespace() {
    char **env = build_container_env(NULL, false);
    char *argv[] = {"/bin/sh", "-c", "ipcs && echo ok", NULL};
    cgroup_config_t config = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = env,
        .enable_debug = false,
        .enable_pid_namespace = true,
        .enable_mount_namespace = false,
        .enable_uts_namespace = false,
        .enable_user_namespace = false,
        .enable_ipc_namespace = true,    // Phase 4c carried forward
        .rootfs_path = NULL,
        .enable_overlay = false,
        .hostname = NULL,
        .cgroup_limits = {
            .memory_limit = 50 * 1024 * 1024,
            .pid_limit = 10,
        },
        .enable_cgroup = true
    };

    cgroup_result_t result = cgroup_exec(&config);
    cgroup_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);
    printf("PASS: test_cgroup_with_ipc_namespace\n");
}

int main() {
    if (geteuid() != 0) {
        fprintf(stderr, "Cgroup tests must run as root (sudo)\n");
        return 1;
    }

    test_cgroup_creation_and_cleanup();
    test_memory_limit();
    test_pid_limit();
    test_no_cgroup_backward_compat();
    test_cgroup_with_ipc_namespace();

    printf("\nAll cgroup tests passed!\n");
    return 0;
}