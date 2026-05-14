// Note: _GNU_SOURCE is provided by the Makefile.
// Phase 7a: was net_exec/net_config_t — now container_exec.
// Local build_container_env() stub removed — we #include "env.h" instead.
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
        .enable_pid_namespace = true,
        .uid_map_inside = 0,
        .uid_map_outside = getuid(),
        .uid_map_range = 1,
        .gid_map_inside = 0,
        .gid_map_outside = getgid(),
        .gid_map_range = 1,
    };
    return cfg;
}

void test_network_creates_namespace(void) {
    char **env = build_container_env(NULL, false);
    /* Run something guaranteed available on the host (no rootfs). */
    container_config_t cfg = base_config(env, "true");
    cfg.enable_network = true;
    strcpy(cfg.veth.host_ip, "10.99.0.1");
    strcpy(cfg.veth.container_ip, "10.99.0.2");
    strcpy(cfg.veth.netmask, "24");
    cfg.veth.enable_nat = false;

    container_result_t r = container_exec(&cfg);
    container_cleanup(&r);
    free(env);

    assert(r.exited_normally);
    assert(r.exit_status == 0);
    printf("PASS: test_network_creates_namespace\n");
}

void test_no_network_backward_compat(void) {
    char **env = build_container_env(NULL, false);
    container_config_t cfg = base_config(env, "true");
    cfg.enable_network = false;

    container_result_t r = container_exec(&cfg);
    container_cleanup(&r);
    free(env);

    assert(r.exited_normally);
    assert(r.exit_status == 0);
    printf("PASS: test_no_network_backward_compat\n");
}

void test_network_with_cgroup(void) {
    char **env = build_container_env(NULL, false);
    container_config_t cfg = base_config(env, "true");
    cfg.enable_network = true;
    cfg.enable_cgroup = true;
    cfg.cgroup_limits.memory_limit = 50 * 1024 * 1024;
    cfg.cgroup_limits.pid_limit    = 10;
    strcpy(cfg.veth.host_ip, "10.99.1.1");
    strcpy(cfg.veth.container_ip, "10.99.1.2");
    strcpy(cfg.veth.netmask, "24");
    cfg.veth.enable_nat = false;

    container_result_t r = container_exec(&cfg);
    container_cleanup(&r);
    free(env);

    assert(r.exited_normally);
    assert(r.exit_status == 0);
    printf("PASS: test_network_with_cgroup\n");
}

int main(void) {
    if (geteuid() != 0) {
        fprintf(stderr, "Network tests require root (veth + iptables)\n");
        return 1;
    }
    test_network_creates_namespace();
    test_no_network_backward_compat();
    test_network_with_cgroup();
    printf("\nAll network tests passed!\n");
    return 0;
}
