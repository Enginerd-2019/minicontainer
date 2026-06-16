#ifndef INIT_H
#define INIT_H

#include <stdbool.h>

/*
 * Phase 8b+ : minimal in-process "init" (PID 1) supervisor — the
 * equivalent of `docker run --init` (tini / dumb-init), compiled into
 * minicontainer rather than shipped as a separate binary.
 *
 * Called from child_func when --init is set, in place of the direct
 * execve.  It forks:
 *   - the child execve's the workload exactly as it would have without
 *     --init (same program/argv/envp);
 *   - the parent stays as PID 1 of the container's PID namespace and:
 *       * forwards SIGTERM/SIGINT/SIGQUIT/SIGHUP/SIGUSR1/SIGUSR2 to the
 *         workload — so a signal from an ancestor (e.g. `minicontainer
 *         stop`'s kill(pid, SIGTERM)) actually reaches a workload that,
 *         were it PID 1 itself, would silently drop the signal unless
 *         it installed a handler (man 7 pid_namespaces);
 *       * reaps orphaned grandchildren that reparent to PID 1;
 *       * exits with the workload's status: returns WEXITSTATUS for a
 *         normal exit, or re-raises the workload's terminating signal so
 *         the host-side waitpid observes the same WIFSIGNALED result as
 *         the no-init path.
 *
 * Runs AFTER hardening in child_func, so both the supervisor and the
 * workload inherit the dropped caps / NO_NEW_PRIVS / seccomp filter.
 *
 * Does NOT return on the workload-killed-by-signal path (it re-raises).
 * Otherwise returns the workload's exit code, or 127 if the fork failed.
 */
int run_init_supervisor(const char *program, char *const argv[],
                        char *const envp[], bool enable_debug);

#endif /* INIT_H */
