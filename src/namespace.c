#include "namespace.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>

#define STACK_SIZE (1024 * 1024)  // 1 MB stack

extern char **environ;

/**
 * Arguments passed to child function
 */
typedef struct {
    const char *program;
    char *const *argv;
    char *const *envp;
    bool enable_debug;
} child_args_t;

/**
 * Child function executed in new namespace.
 * This runs in the context of clone(), not after fork().
 *
 * @param arg  Pointer to child_args_t
 * @return     Exit status (not typically reached if execve succeeds)
 */
static int child_func(void *arg){
    child_args_t *args = (child_args_t *)arg;

    if(args->enable_debug){
        // Inside namespace, getpid() returns namespace PID
        printf("[child] PID inside namespace: %d\n", getpid());
        printf("[child] PPID inside namespace: %d\n", getppid());
    }

    // Execute the target program
    char *const *envp = args->envp ? args->envp : environ;
    execve(args->program, args->argv, envp);

    // execve only returns on error
    perror("execve");
    return 127; // Shell convention: command not found
}

namespace_result_t namespace_exec(const namespace_config_t *config){
    namespace_result_t result = {0};

    // Validate input
    if(!config || !config->program || !config->argv){
        fprintf(stderr, "namespace_exec: invalid config\n");
        result.child_pid = -1;
        return result;
    }

    if (config->enable_debug) {
        printf("[parent] Executing: %s", config->program);
        for (int i = 0; config->argv[i] != NULL; i++) {
            printf(" %s", config->argv[i]);
        }
        printf("\n");
    }

    // Allocate stack for child process
    char *stack = malloc(STACK_SIZE);
    if(!stack){
        perror("malloc");
        result.child_pid = -1;
        return result;
    }
    result.stack_ptr = stack;

    // Prepare arguments for child
    child_args_t child_args = {
        .program = config->program,
        .argv = config->argv,
        .envp = config->envp,
        .enable_debug = config->enable_debug
    };

    // Build clone flags
    int flags = SIGCHLD;  // Always send SIGCHLD on exit
    if(config->enable_pid_namespace){
        flags |= CLONE_NEWPID;

        if(config->enable_debug){
            printf("[parent] Creating PID namespace\n");
        }
    }

    // Clone the process
    // Note: Stack grows down by convention, so top of stack (stack + STACK_SIZE) should be passed
    pid_t pid = clone(child_func, stack + STACK_SIZE, flags, &child_args);

    if(pid < 0){
        perror("clone");
        free(stack);
        result.child_pid = -1;
        result.stack_ptr = NULL;
        return result;
    }

    result.child_pid = pid;

    if(config->enable_debug) {
        printf("[parent] Child PID in parent namespace: %d\n", pid);
    }

    // Wait for child to exit
    int status;
    pid_t wait_result = waitpid(pid, &status, 0);

    if(wait_result < 0){
        perror("waitpid");
        result.exit_status = -1;
        return result;
    }

    // Parse exit status
    if(WIFEXITED(status)){
        result.exited_normally = true;
        result.exit_status = WEXITSTATUS(status);

        if(config->enable_debug) {
            printf("[parent] Child exited with status %d\n", result.exit_status);
        }
    }else if(WIFSIGNALED(status)){
        result.exited_normally = false;
        result.signal = WTERMSIG(status);
        result.exit_status = 128 + result.signal;
        
        if (config->enable_debug) {
            printf("[parent] Child killed by signal %d\n", result.signal);
        }
    }

    return result;
}

void namespace_cleanup(namespace_result_t *result) {
    if (result && result->stack_ptr) {
        free(result->stack_ptr);
        result->stack_ptr = NULL;
    }
}