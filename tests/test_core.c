// Note: _GNU_SOURCE is provided by the Makefile.
#include "core.h"
#include "env.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static container_config_t base_config(char **env, const char *cmd) {
    static char *argv_buf[] = {"/bin/sh", "-c", NULL, NULL};
    argv_buf[2] = (char *)cmd;
    container_config_t cfg = {
        .program = "/bin/sh",
        .argv = argv_buf,
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

/* No namespaces, no rootfs — equivalent to old test_spawn. */
void test_bare_exec(void) {
    char **env = build_container_env(NULL, false);
    container_config_t cfg = base_config(env, "true");

    container_result_t r = container_exec(&cfg);
    container_cleanup(&r);
    free(env);

    assert(r.exited_normally);
    assert(r.exit_status == 0);
    printf("PASS: test_bare_exec\n");
}

/* PID namespace only — equivalent to old test_namespace. */
void test_pid_only(void) {
    char **env = build_container_env(NULL, false);
    container_config_t cfg = base_config(env, "echo $$");
    cfg.enable_pid_namespace = true;

    container_result_t r = container_exec(&cfg);
    container_cleanup(&r);
    free(env);

    assert(r.exited_normally);
    assert(r.exit_status == 0);
    printf("PASS: test_pid_only\n");
}

int main(void) {
    if (geteuid() != 0) {
        fprintf(stderr, "test_core requires root\n");
        return 1;
    }
    test_bare_exec();
    test_pid_only();
    printf("\nAll core tests passed!\n");
    return 0;
}