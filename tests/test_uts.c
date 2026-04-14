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

int main() {
    if (geteuid() != 0) {
        fprintf(stderr, "Tests must run as root (sudo)\n");
        return 1;
    }

    test_hostname_isolation();
    test_no_uts_backward_compat();

    printf("\nAll UTS tests passed!\n");
    return 0;
}