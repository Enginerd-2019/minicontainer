#define _GNU_SOURCE
#include "namespace.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void test_pid_namespace() {
    char *argv[] = {"/bin/sh", "-c", "echo $$", NULL};
    namespace_config_t config = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = NULL,
        .enable_debug = false,
        .enable_pid_namespace = true
    };

    namespace_result_t result = namespace_exec(&config);
    namespace_cleanup(&result);

    assert(result.child_pid > 0);
    assert(result.exited_normally);
    assert(result.exit_status == 0);

    printf("✓ test_pid_namespace passed\n");
}

void test_no_namespace() {
    char *argv[] = {"/bin/true", NULL};
    namespace_config_t config = {
        .program = "/bin/true",
        .argv = argv,
        .envp = NULL,
        .enable_debug = false,
        .enable_pid_namespace = false  // No namespace
    };

    namespace_result_t result = namespace_exec(&config);
    namespace_cleanup(&result);

    assert(result.child_pid > 0);
    assert(result.exited_normally);
    assert(result.exit_status == 0);

    printf("✓ test_no_namespace passed\n");
}

void test_stack_cleanup() {
    char *argv[] = {"/bin/true", NULL};
    namespace_config_t config = {
        .program = "/bin/true",
        .argv = argv,
        .envp = NULL,
        .enable_debug = false,
        .enable_pid_namespace = true
    };

    namespace_result_t result = namespace_exec(&config);

    // Stack should be allocated
    assert(result.stack_ptr != NULL);

    namespace_cleanup(&result);

    // Stack should be freed (pointer nulled)
    assert(result.stack_ptr == NULL);

    printf("✓ test_stack_cleanup passed\n");
}

int main() {
    // Must run as root or with CAP_SYS_ADMIN
    if (geteuid() != 0) {
        fprintf(stderr, "Tests must run as root\n");
        return 1;
    }

    test_pid_namespace();
    test_no_namespace();
    test_stack_cleanup();

    printf("\nAll tests passed! ✓\n");
    return 0;
}