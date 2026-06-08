// Note: _GNU_SOURCE is provided by the Makefile via -D_GNU_SOURCE.
// Phase 7a: was cgroup_exec/cgroup_config_t — now container_exec.
// Local build_container_env() stub removed — we #include "env.h" instead.
#include "core.h"
#include "env.h"
#include "cgroup.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

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

void test_cgroup_creation_and_cleanup(void) {
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

void test_memory_limit(void) {
    char **env = build_container_env(NULL, false);
    // Allocate 50MB within a 100MB limit — should succeed
    char *argv[] = {"/bin/sh", "-c", "head -c 50M /dev/zero > /dev/null", NULL};
    container_config_t cfg = make_cfg(env, argv);
    cfg.enable_pid_namespace = true;
    cfg.enable_cgroup        = true;
    cfg.cgroup_limits.memory_limit = 100 * 1024 * 1024;

    container_result_t result = container_exec(&cfg);
    container_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);
    printf("PASS: test_memory_limit\n");
}

void test_pid_limit(void) {
    char **env = build_container_env(NULL, false);
    // Spawn far more processes than allowed so the cap denies some forks.
    // Pure sh builtins + sleep (no `seq` dependency).
    char *argv[] = {"/bin/sh", "-c",
        "i=0; while [ $i -lt 30 ]; do sleep 1 & i=$((i+1)); done; wait", NULL};
    container_config_t cfg = make_cfg(env, argv);
    cfg.enable_pid_namespace = true;
    cfg.enable_cgroup        = true;
    cfg.cgroup_limits.pid_limit = 5;

    container_result_t result = container_exec(&cfg);

    /* ENFORCEMENT assertion (Error #25 regression). Read pids.events
     * BEFORE container_cleanup removes the cgroup. The child has already
     * exited (container_wait reaped it), but pids.events "max" — the
     * cumulative count of forks the kernel DENIED because of pids.max —
     * persists until the cgroup is rmdir'd. A nonzero value proves the
     * child was placed in its cgroup before it forked; the pre-fix race
     * let it fork freely in the parent cgroup, leaving max == 0. Earlier
     * this test only checked that the shell exited, so it never actually
     * verified the limit was enforced. */
    unsigned long long denied = 0;
    char ev_path[512];
    snprintf(ev_path, sizeof(ev_path), "%s/pids.events",
             result.ctx.cgroup_ctx.cgroup_path);
    FILE *evf = fopen(ev_path, "r");
    if (evf) {
        char line[128];
        while (fgets(line, sizeof(line), evf)) {
            if (sscanf(line, "max %llu", &denied) == 1) break;
        }
        fclose(evf);
    }

    container_cleanup(&result);
    free(env);

    // The shell should still exit (fork failures don't kill the shell)...
    assert(result.exited_normally);
    // ...and the cap must have actually denied forks.
    assert(denied > 0);
    printf("PASS: test_pid_limit (cap 5 denied %llu forks)\n", denied);
}

void test_no_cgroup_backward_compat(void) {
    char **env = build_container_env(NULL, false);
    char *argv[] = {"/bin/echo", "hello", NULL};
    container_config_t cfg = make_cfg(env, argv);
    cfg.program              = "/bin/echo";
    cfg.enable_pid_namespace = true;
    /* enable_cgroup left false */

    container_result_t result = container_exec(&cfg);
    container_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);
    printf("PASS: test_no_cgroup_backward_compat\n");
}

void test_cgroup_with_ipc_namespace(void) {
    char **env = build_container_env(NULL, false);
    char *argv[] = {"/bin/sh", "-c", "ipcs && echo ok", NULL};
    container_config_t cfg = make_cfg(env, argv);
    cfg.enable_pid_namespace = true;
    cfg.enable_ipc_namespace = true;
    cfg.enable_cgroup        = true;
    cfg.cgroup_limits.memory_limit = 50 * 1024 * 1024;
    cfg.cgroup_limits.pid_limit    = 10;

    container_result_t result = container_exec(&cfg);
    container_cleanup(&result);
    free(env);

    assert(result.exited_normally);
    assert(result.exit_status == 0);
    printf("PASS: test_cgroup_with_ipc_namespace\n");
}

int main(void) {
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
