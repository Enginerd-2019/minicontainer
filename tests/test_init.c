// Phase 8b+ : tests/test_init.c
//
// Exercises run_init_supervisor() — the --init PID-1 supervisor.
//
// No root / no namespaces needed: the supervisor is just fork + exec +
// signal forwarding + waitpid, all of which work in the plain host PID
// namespace.  Each case forks a child that BECOMES the supervisor
// (calling run_init_supervisor, which itself forks the workload); the
// test parent inspects the child's wait status, and for the forwarding
// case sends a signal to the supervisor mid-run.  A bounded wait means
// a broken implementation FAILS the test rather than hanging.

#include "init.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL: %s — %s\n", __func__, msg);          \
            return 1;                                                   \
        }                                                               \
    } while (0)

static char *const ENVP[] = { (char *)"PATH=/bin:/usr/bin", NULL };

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/*
 * Fork a child that runs run_init_supervisor(argv).  If `sig` is
 * non-zero, send it to the supervisor after `delay_ms`.  Reap the
 * child (bounded to ~5s; SIGKILL + fail on timeout).  Returns 0 on a
 * clean reap with *out_status set, -1 on timeout.
 */
static int run_supervised(char *const argv[], int sig, int delay_ms,
                          int *out_status)
{
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -2; }
    if (pid == 0) {
        _exit(run_init_supervisor(argv[0], argv, ENVP, false));
    }

    if (sig) {
        sleep_ms(delay_ms);
        kill(pid, sig);
    }

    for (int i = 0; i < 500; i++) {       /* up to ~5s */
        pid_t r = waitpid(pid, out_status, WNOHANG);
        if (r == pid) return 0;
        if (r < 0) { perror("waitpid"); return -2; }
        sleep_ms(10);
    }
    kill(pid, SIGKILL);
    waitpid(pid, out_status, 0);
    return -1;
}

/* The supervisor must exit with the workload's exit code. */
static int test_exit_code_propagation(void)
{
    char *const argv[] = { (char *)"/bin/sh", (char *)"-c",
                           (char *)"exit 42", NULL };
    int st;
    CHECK(run_supervised(argv, 0, 0, &st) == 0, "supervisor timed out");
    CHECK(WIFEXITED(st), "supervisor did not exit normally");
    CHECK(WEXITSTATUS(st) == 42, "workload exit code not propagated (want 42)");
    printf("PASS: test_exit_code_propagation\n");
    return 0;
}

/* A workload killed by a signal must make PID 1 die by the same signal
 * (re-raise), so the host sees WIFSIGNALED — as in the no-init path. */
static int test_signal_death_mirrored(void)
{
    char *const argv[] = { (char *)"/bin/sh", (char *)"-c",
                           (char *)"kill -TERM $$", NULL };
    int st;
    CHECK(run_supervised(argv, 0, 0, &st) == 0, "supervisor timed out");
    CHECK(WIFSIGNALED(st), "supervisor did not die by signal");
    CHECK(WTERMSIG(st) == SIGTERM, "supervisor died by the wrong signal");
    printf("PASS: test_signal_death_mirrored\n");
    return 0;
}

/* The headline behavior: a signal sent to the SUPERVISOR is forwarded
 * to the workload.  The workload traps TERM and exits 7; without
 * forwarding it would sleep out the bounded wait (or the supervisor
 * would die by the unforwarded TERM), either of which fails the test. */
static int test_signal_forwarding(void)
{
    char *const argv[] = { (char *)"/bin/sh", (char *)"-c",
                           (char *)"trap 'exit 7' TERM; sleep 5", NULL };
    int st;
    int rc = run_supervised(argv, SIGTERM, 300, &st);
    CHECK(rc == 0, "supervisor timed out (signal not forwarded?)");
    CHECK(WIFEXITED(st), "supervisor did not exit normally (TERM not forwarded?)");
    CHECK(WEXITSTATUS(st) == 7, "forwarded-signal workload exit code wrong (want 7)");
    printf("PASS: test_signal_forwarding\n");
    return 0;
}

int main(void)
{
    int rc = 0;
    printf("=== test_init ===\n");
    rc |= test_exit_code_propagation();
    rc |= test_signal_death_mirrored();
    rc |= test_signal_forwarding();
    if (rc == 0) {
        printf("\nAll init tests passed!\n");
    }
    return rc;
}
