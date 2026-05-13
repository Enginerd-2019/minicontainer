// Note: _GNU_SOURCE is provided by the Makefile.
#include "net.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_ENV_ENTRIES 256
#define DEFAULT_PATH "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

/* Phase 4c test-side helper convention. */
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

static net_config_t base_config(char **env, const char *cmd) {
    static char *argv_buf[] = {"/bin/sh", "-c", NULL, NULL};
    argv_buf[2] = (char *)cmd;
    net_config_t cfg = {
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
    /* Run something that's guaranteed available on the host (no rootfs).
     * We check exit==0 and assume the test runner is on a system where
     * /bin/sh exists in the host PATH. */
    net_config_t cfg = base_config(env, "true");
    cfg.enable_network = true;
    strcpy(cfg.veth.host_ip, "10.99.0.1");
    strcpy(cfg.veth.container_ip, "10.99.0.2");
    strcpy(cfg.veth.netmask, "24");
    cfg.veth.enable_nat = false;

    net_result_t r = net_exec(&cfg);
    net_cleanup(&r);
    free(env);

    assert(r.exited_normally);
    assert(r.exit_status == 0);
    printf("PASS: test_network_creates_namespace\n");
}

void test_no_network_backward_compat(void) {
    char **env = build_container_env(NULL, false);
    net_config_t cfg = base_config(env, "true");
    cfg.enable_network = false;

    net_result_t r = net_exec(&cfg);
    net_cleanup(&r);
    free(env);

    assert(r.exited_normally);
    assert(r.exit_status == 0);
    printf("PASS: test_no_network_backward_compat\n");
}

void test_network_with_cgroup(void) {
    char **env = build_container_env(NULL, false);
    net_config_t cfg = base_config(env, "true");
    cfg.enable_network = true;
    cfg.enable_cgroup = true;
    cfg.cgroup_limits.memory_limit = 50 * 1024 * 1024;
    cfg.cgroup_limits.pid_limit = 10;
    strcpy(cfg.veth.host_ip, "10.99.1.1");
    strcpy(cfg.veth.container_ip, "10.99.1.2");
    strcpy(cfg.veth.netmask, "24");
    cfg.veth.enable_nat = false;

    net_result_t r = net_exec(&cfg);
    net_cleanup(&r);
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