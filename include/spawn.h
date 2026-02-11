#ifndef SPAWN_H
#define SPAWN_H

#include <stdbool.h>
#include <sys/types.h>

/**
 * Configuration for spawning a process.
 *
 * Example usage:
 *   char *argv[] = {"/bin/ls", "-la", NULL};
 *   char *envp[] = {NULL};  // Inherit environment
 *   spawn_config_t config = {
 *       .program = "/bin/ls",
 *       .argv = argv,
 *       .envp = envp,
 *       .enable_debug = false
 *   };
 */
typedef struct {
    const char *program;        // Path to executable
    char *const *argv;          // Argument vector (NULL-terminated)
    char *const *envp;          // Environment variables (NULL-terminated)
    bool enable_debug;          // Enable debug output
} spawn_config_t;

/**
 * Result of a spawn operation.
 */
typedef struct {
    pid_t child_pid;            // Child PID (from parent's view)
    int exit_status;            // Exit code (0-255) or signal number
    bool exited_normally;       // true if exited, false if killed by signal
    int signal;                 // Signal number (only valid if !exited_normally)
} spawn_result_t;

/**
 * Spawn a process using fork/execve pattern.
 *
 * @param config  Configuration for the process
 * @return        Result structure (exit status, etc.)
 *
 * Note: This function blocks until the child exits.
 *
 * Errors:
 *   - If fork() fails, prints error and returns result with child_pid = -1
 *   - If execve() fails in child, child exits with status 127
 *   - Parent always waits for child before returning
 */
spawn_result_t spawn_process(const spawn_config_t *config);

/**
 * Setup signal handlers for process management.
 * Must be called once at program start.
 *
 * @return 0 on success, -1 on failure
 */
int spawn_init_signals(void);

#endif // SPAWN_H