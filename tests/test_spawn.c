#include "spawn.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void test_basic_execution() {
    char *argv[] = {"/bin/true", NULL};
    spawn_config_t config = {
        .program = "/bin/true",
        .argv = argv,
        .envp = NULL,
        .enable_debug = false
    };

    spawn_result_t result = spawn_process(&config);

    assert(result.child_pid > 0);
    assert(result.exited_normally);
    assert(result.exit_status == 0);

    printf("✓ test_basic_execution passed\n");
}

void test_exit_code() {
    char *argv[] = {"/bin/sh", "-c", "exit 42", NULL};
    spawn_config_t config = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = NULL,
        .enable_debug = false
    };

    spawn_result_t result = spawn_process(&config);

    assert(result.exited_normally);
    assert(result.exit_status == 42);

    printf("✓ test_exit_code passed\n");
}

void test_signal_death() {
    char *argv[] = {"/bin/sh", "-c", "kill -TERM $$", NULL};
    spawn_config_t config = {
        .program = "/bin/sh",
        .argv = argv,
        .envp = NULL,
        .enable_debug = false
    };

    spawn_result_t result = spawn_process(&config);

    assert(!result.exited_normally);
    assert(result.signal == 15);  // SIGTERM

    printf("✓ test_signal_death passed\n");
}

void test_execve_failure() {
    char *argv[] = {"/nonexistent/binary", NULL};
    spawn_config_t config = {
        .program = "/nonexistent/binary",
        .argv = argv,
        .envp = NULL,
        .enable_debug = false
    };

    spawn_result_t result = spawn_process(&config);

    assert(result.exited_normally);
    assert(result.exit_status == 127);  // Command not found

    printf("✓ test_execve_failure passed\n");
}

int main() {
    spawn_init_signals();

    test_basic_execution();
    test_exit_code();
    test_signal_death();
    test_execve_failure();

    printf("\nAll tests passed! ✓\n");
    return 0;
}