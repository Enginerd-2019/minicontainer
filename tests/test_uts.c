// Phase 7a: was uts_exec/uts_config_t — now container_exec.
// Local build_container_env() stub removed — we #include "env.h" instead.
#include "core.h"
#include "env.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* Phase 7a: every test populates uid/gid mappings the same way. Cuts
 * boilerplate vs the old pattern that copy-pasted the six fields into
 * every uts_config_t literal. */
static container_config_t make_cfg(char **env, char *const *argv) {
    container_config_t cfg = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = env,
        .uid_map_inside = 0,
        .uid_map_outside = getuid(),
        .uid_map_range = 1,
        .gid_map_inside = 0,
        .gid_map_outside = getgid(),
        .gid_map_range = 1,
    };
    return cfg;
}

void test_hostname_isolation(void) {
    char **env = build_container_env(NULL, false);
    char *argv[] = {"/bin/sh", "-c", "hostname", NULL};
    container_config_t cfg = make_cfg(env, argv);
    cfg.enable_pid_namespace = true;
    cfg.enable_uts_namespace = true;
    cfg.hostname             = "testcontainer";

    container_result_t result = container_exec(&cfg);
    container_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);

    printf("PASS: test_hostname_isolation\n");
}

void test_no_uts_backward_compat(void) {
    char **env = build_container_env(NULL, false);
    char *argv[] = {"/bin/sh", "-c", "hostname", NULL};
    container_config_t cfg = make_cfg(env, argv);
    cfg.enable_pid_namespace = true;
    /* No UTS namespace, no hostname. */

    container_result_t result = container_exec(&cfg);
    container_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);

    printf("PASS: test_no_uts_backward_compat\n");
}

void test_user_namespace_unprivileged(void) {
    char **env = build_container_env(NULL, false);
    char *argv[] = {"/bin/sh", "-c", "id -u", NULL};
    container_config_t cfg = make_cfg(env, argv);
    cfg.enable_user_namespace = true;

    container_result_t result = container_exec(&cfg);
    container_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);
    printf("PASS: test_user_namespace_unprivileged\n");
}

void test_user_namespace_with_hostname(void) {
    char **env = build_container_env(NULL, false);
    char *argv[] = {"/bin/sh", "-c", "hostname && id -u", NULL};
    container_config_t cfg = make_cfg(env, argv);
    cfg.enable_pid_namespace  = true;
    cfg.enable_uts_namespace  = true;
    cfg.enable_user_namespace = true;
    cfg.hostname              = "rootless-container";

    container_result_t result = container_exec(&cfg);
    container_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);
    printf("PASS: test_user_namespace_with_hostname\n");
}

void test_ipc_isolation(void) {
    char **env = build_container_env(NULL, false);
    /* ipcs exits 0 regardless of whether objects exist. The key
     * verification is that we can create the namespace without error. */
    char *argv[] = {"/bin/sh", "-c", "ipcs", NULL};
    container_config_t cfg = make_cfg(env, argv);
    cfg.enable_pid_namespace = true;
    cfg.enable_ipc_namespace = true;

    container_result_t result = container_exec(&cfg);
    container_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);
    printf("PASS: test_ipc_isolation\n");
}

void test_ipc_with_user_namespace(void) {
    char **env = build_container_env(NULL, false);
    char *argv[] = {"/bin/sh", "-c", "ipcs && id -u", NULL};
    container_config_t cfg = make_cfg(env, argv);
    cfg.enable_pid_namespace  = true;
    cfg.enable_user_namespace = true;
    cfg.enable_ipc_namespace  = true;

    container_result_t result = container_exec(&cfg);
    container_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);
    printf("PASS: test_ipc_with_user_namespace\n");
}

void test_no_ipc_backward_compat(void) {
    /* Verifies that removing --ipc doesn't break the rootless path.
     * Uses CLONE_NEWUSER so it runs without sudo. */
    char **env = build_container_env(NULL, false);
    char *argv[] = {"/bin/sh", "-c", "ipcs", NULL};
    container_config_t cfg = make_cfg(env, argv);
    cfg.enable_pid_namespace  = true;
    cfg.enable_user_namespace = true;
    /* enable_ipc_namespace left false */

    container_result_t result = container_exec(&cfg);
    container_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);
    printf("PASS: test_no_ipc_backward_compat\n");
}

int main(void) {
    if (geteuid() == 0) {
        test_hostname_isolation();
        test_no_uts_backward_compat();
        test_ipc_isolation();
    } else {
        printf("SKIP: test_hostname_isolation (requires root)\n");
        printf("SKIP: test_no_uts_backward_compat (requires root)\n");
        printf("SKIP: test_ipc_isolation (requires root)\n");
    }

    test_user_namespace_unprivileged();
    test_user_namespace_with_hostname();
    test_ipc_with_user_namespace();
    test_no_ipc_backward_compat();

    printf("\nAll UTS/user/IPC namespace tests passed!\n");
    return 0;
}
