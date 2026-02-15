#ifndef NAMESPACE_H
#define NAMESPACE_H

#include <sched.h>
#include <stdbool.h>
#include <sys/types.h>

/** 
 * Configuration for namespace-isolated process
 */
typedef struct{
    const char * program;
    char *const *argv;
    char *const *envp;
    bool enable_debug;
    bool enable_pid_namespace;
} namespace_config_t;

/**
 * Result of namespace operation
 */
typedef struct {
    pid_t child_pid;        // This is the pid in the parent namespace
    int exit_status;
    bool exited_normally;
    int signal;            
    void *stack_ptr;       // void * for cleanup
} namespace_result_t;

/**
 * Execute a process with optional PID namespace isolation.
 *
 * @param config  Configuration (includes namespace flags)
 * @return        Result with exit status and cleanup info
 *
 * Note: Caller must call namespace_cleanup() on the result.
 */
namespace_result_t namespace_exec(const namespace_config_t *config);

/**
 * Clean up resources allocated by namespace_exec().
 *
 * @param result  Result returned from namespace_exec()
 */
void namespace_cleanup(namespace_result_t *result);

#endif // NAMESPACE_H