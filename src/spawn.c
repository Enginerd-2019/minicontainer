#define _POSIX_C_SOURCE 200809L // Required for sigaction and SA_RESTART 
#include "spawn.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <string.h>

// Global flag to track if signals are initialized
static bool signals_initialized = false;

/**
 * Signal handler for SIGCHLD.
 * Reaps zombie children that weren't explicitly waited for.
**/
static void sigchld_handler(int signo){
    (void)signo; // Unused parameter

    int saved_errno = errno; // sigaction (3p) recommends saving errno

    // Reap all dead children (WNOHANG = non-blocking)
    while(waitpid(-1, NULL, WNOHANG) > 0){
        // Loop continues until there are no more zombies
    }

    errno = saved_errno;
}

int spawn_init_signals(void){
    if(signals_initialized){
        return 0; // Already initialized
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    // SA_RESTART: restart interrupted syscalls
    // SA_NOCLDSTOP: only notify on exit, not stop/continue

    if(sigaction(SIGCHLD, &sa, NULL) < 0){
        perror("sigaction(SIGCHLD)");
        return -1;
    }

    signals_initialized = true;
    return 0;
}

spawn_result_t spawn_process(const spawn_config_t *config){
    spawn_result_t result = {0};

    // Validate input
    if(!config || !config->program || !config->argv){
        fprintf(stderr, "spawn_process: invalid config\n");
        result.child_pid = -1;
        return result;
    }

    if(config->enable_debug){
        printf("[spawn] Executing: %s", config->program);
        for(int i = 0; config->argv[i] != NULL; i++){
            printf(" %s", config->argv[i]);
        }
        printf("\n");
    }

    // Fork the process
    pid_t pid = fork();

    if(pid < 0){
        // Fork failed
        perror("fork");
        result.child_pid = -1;
        return result;
    }

    if(pid == 0){
        // === CHILD PROCESS ===

        // Execute the program
        // Note: envp can be NULL to inherit environment
        char *const *envp = config->envp ? config->envp : __environ;

        execve(config->program, config->argv, envp);

        // If we reach here, execve failed
        perror("execve");
        exit(127); // Shell convention: command not found
    }

    // === PARENT PROCESS ===
    result.child_pid = pid;

    if(config->enable_debug){
        printf("[spawn] Child PID: %d\n", pid);
    }

    // Wait for child to exit
    int status;
    pid_t wait_result = waitpid(pid, &status, 0);

    if(wait_result < 0){
        perror("waitpid");
        result.child_pid = -1;
        return result;
    }

    // Parse exit status
    if(WIFEXITED(status)){
        result.exited_normally = true;
        result.exit_status = WEXITSTATUS(status);

        if(config->enable_debug){
            printf("[spawn] Child exited with status %d\n", result.exit_status);
        }
    } else if (WIFSIGNALED(status)) {
        result.exited_normally = false;
        result.signal = WTERMSIG(status);
        result.exit_status = 128 + result.signal;  // Bash convention

        if (config->enable_debug) {
            printf("[spawn] Child killed by signal %d\n", result.signal);
        }
    }

    return result;
}
