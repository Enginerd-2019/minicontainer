// Note: _GNU_SOURCE is provided by the Makefile via -D_GNU_SOURCE.
//
// Phase 8b+ : the --init PID-1 supervisor.  See include/init.h for the
// rationale (docker run --init / tini, in-process).
#include "init.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

/* The workload PID, readable from the async-signal context of the
 * forwarding handler.  Set once, before the handlers are installed. */
static volatile sig_atomic_t g_workload_pid = 0;

/* Forward a received signal to the workload.  async-signal-safe:
 * kill(2) is on the safe list. */
static void forward_signal(int signo)
{
    pid_t p = (pid_t)g_workload_pid;
    if (p > 0) {
        kill(p, signo);
    }
}

int run_init_supervisor(const char *program, char *const argv[],
                        char *const envp[], bool enable_debug)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("[init] fork");
        return 127;
    }

    if (pid == 0) {
        /* Workload child — exec exactly as child_func would have done
         * without --init.  Never falls back to the host environ. */
        execve(program, argv, envp);
        perror("[init] execve");
        _exit(127);
    }

    /* Supervisor (PID 1 of the container's PID namespace when --pid /
     * --rootfs is in effect).  Record the workload PID first, then
     * install the forwarding handlers, so a signal can't be dropped
     * for lack of a target. */
    g_workload_pid = pid;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = forward_signal;
    sigemptyset(&sa.sa_mask);
    /* Deliberately no SA_RESTART: a forwarded signal interrupts the
     * blocking waitpid below (EINTR), which we simply retry. */
    const int forwarded[] = {
        SIGTERM, SIGINT, SIGQUIT, SIGHUP, SIGUSR1, SIGUSR2
    };
    for (size_t i = 0; i < sizeof(forwarded) / sizeof(forwarded[0]); i++) {
        sigaction(forwarded[i], &sa, NULL);
    }

    if (enable_debug) {
        fprintf(stderr, "[init] supervisor pid %d, workload pid %d\n",
                (int)getpid(), (int)pid);
    }

    /* Reap children as they exit.  The blocking waitpid(-1) also reaps
     * orphaned grandchildren that reparent to us (PID 1).  When the
     * workload itself is reaped, capture its status, drain any pending
     * zombies, and stop. */
    int workload_status = 0;
    for (;;) {
        int status;
        pid_t r = waitpid(-1, &status, 0);
        if (r < 0) {
            if (errno == EINTR) continue;   /* forwarded a signal; retry */
            if (errno == ECHILD) break;      /* nothing left to wait for */
            perror("[init] waitpid");
            break;
        }
        if (r == pid) {
            workload_status = status;
            while (waitpid(-1, NULL, WNOHANG) > 0) { /* drain zombies */ }
            break;
        }
        /* Otherwise we reaped an orphan — keep going. */
    }

    if (WIFSIGNALED(workload_status)) {
        int s = WTERMSIG(workload_status);
        /* Re-raise so PID 1 terminates the same way the workload did,
         * and the host-side container_wait reports WIFSIGNALED with this
         * signal — matching the no-init path where the workload WAS
         * PID 1.  Reset to default first (we installed a handler for
         * some of these). */
        signal(s, SIG_DFL);
        raise(s);
        _exit(128 + s);   /* fallback if the signal wasn't fatal */
    }

    return WEXITSTATUS(workload_status);
}
