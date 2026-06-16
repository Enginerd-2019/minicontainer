// Phase 8b: tests/test_hardening.c
//
// Exercises the three hardening primitives (drop_capabilities,
// apply_no_new_privs, apply_seccomp_filter) plus state_claim_id's
// collision-free ID minting.
//
// WHY EVERY HARDENING CASE FORKS A CHILD
// --------------------------------------
// The three primitives are IRREVERSIBLE and PROCESS-WIDE:
//   * a capability dropped from the bounding set can never be reacquired,
//   * PR_SET_NO_NEW_PRIVS is a one-way bit inherited across fork/execve,
//   * an installed seccomp filter stays for the life of the process and
//     subjects every later syscall — including the test harness's own —
//     to the allow-list.
// So we cannot call them in the test's main process and keep running.
// Each case runs in a fork()'d child that mutates its own state, verifies
// it, and reports via exit code; the parent only inspects the wait status.
// run_child() also distinguishes a clean nonzero exit from death-by-signal,
// because a mis-built filter (default KILL instead of EPERM) would SIGSYS
// the child rather than return an errno — a failure mode worth naming.
//
// Capability dropping needs CAP_SETPCAP, so the cap tests are root-gated
// (printed as SKIP otherwise). NO_NEW_PRIVS + seccomp install need only
// NO_NEW_PRIVS, so those run unprivileged too. state_claim_id is
// reversible (it just mkdirs under state_dir_root()), so it runs inline.

#include "hardening.h"
#include "state.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>            // unshare, CLONE_NEWNS
#include <sys/prctl.h>        // prctl, PR_GET_* / PR_CAPBSET_READ
#include <sys/stat.h>
#include <sys/syscall.h>     // syscall, __NR_clone3
#include <sys/wait.h>
#include <linux/capability.h> // CAP_* constants

#ifndef __NR_clone3
#define __NR_clone3 435      // x86_64 — absent from pre-5.3 kernel headers
#endif

/* In-child check: on failure, print to stderr and return nonzero so the
 * child's exit status reports the failure to the parent. */
#define CCHECK(cond, msg)                                               \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "  [child:%s] check failed: %s (errno=%d %s)\n", \
                    __func__, msg, errno, strerror(errno));             \
            return 1;                                                   \
        }                                                               \
    } while (0)

/* Inline (non-forked) check, matching the project's other test files. */
#define CHECK(cond, msg)                                                \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL: %s — %s\n", __func__, msg);          \
            return 1;                                                   \
        }                                                               \
    } while (0)

/*
 * Run `fn` in a forked child. Returns 0 only if the child exited 0.
 * A nonzero exit (a CCHECK fired) and death-by-signal (e.g. an
 * unexpected SECCOMP_RET_KILL) are reported distinctly, since the
 * latter means the filter killed rather than EPERM'd.
 */
static int run_child(const char *name, int (*fn)(void))
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        _exit(fn() == 0 ? 0 : 1);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "FAIL: %s — child killed by signal %d "
                "(a blocked syscall was KILLed, not EPERM'd?)\n",
                name, WTERMSIG(status));
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: %s — child exited %d\n",
                name, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return 1;
    }
    printf("PASS: %s\n", name);
    return 0;
}

/* ---- NO_NEW_PRIVS: the bit is set and readable (no privilege needed). ---- */
static int child_no_new_privs(void)
{
    CCHECK(apply_no_new_privs(false) == 0, "apply_no_new_privs returned -1");
    CCHECK(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1,
           "PR_GET_NO_NEW_PRIVS != 1 after apply_no_new_privs");
    return 0;
}

/* ---- Seccomp: filter active; allowed syscall works; excluded syscall
 *      EPERMs (not killed); clone3 returns ENOSYS (the glibc-fallback
 *      contract), NOT the EPERM default. Runs unprivileged because
 *      NO_NEW_PRIVS alone authorizes the filter install. ---- */
static int child_seccomp(void)
{
    CCHECK(apply_no_new_privs(false) == 0, "nnp failed");
    CCHECK(apply_seccomp_filter(false) == 0, "seccomp install failed");

    /* prctl is in the allow-list, so PR_GET_SECCOMP returns the mode
     * (2 = SECCOMP_MODE_FILTER) instead of killing us. */
    CCHECK(prctl(PR_GET_SECCOMP, 0, 0, 0, 0) == 2,
           "PR_GET_SECCOMP != 2 (filter mode) after install");

    /* An allow-listed syscall still works. */
    CCHECK(getpid() > 0, "getpid() failed under the filter");

    /* unshare is excluded from the allow-list -> EPERM (default DENY),
     * and the process survives (default is ERRNO, not KILL). */
    errno = 0;
    CCHECK(unshare(CLONE_NEWNS) == -1 && errno == EPERM,
           "unshare(CLONE_NEWNS) was not EPERM");

    /* clone3 gets a dedicated ENOSYS return so glibc's pthread_create
     * falls back to clone (EPERM would break threading). Regression
     * guard for the deliberate clone3 special-case. */
    errno = 0;
    long r = syscall(__NR_clone3, NULL, (size_t)0);
    CCHECK(r == -1 && errno == ENOSYS, "clone3 was not ENOSYS");
    return 0;
}

/* ---- Capability drop (root only): kept caps remain in the bounding
 *      set, dangerous caps are gone. PR_CAPBSET_READ returns 1 if the
 *      cap is still in the bounding set, 0 if dropped. ---- */
static int child_drop_caps(void)
{
    CCHECK(drop_capabilities(false) == 0, "drop_capabilities returned -1");

    /* Kept (Docker default-profile parity). */
    CCHECK(prctl(PR_CAPBSET_READ, CAP_CHOWN, 0, 0, 0) == 1,
           "CAP_CHOWN dropped (should be kept)");
    CCHECK(prctl(PR_CAPBSET_READ, CAP_NET_BIND_SERVICE, 0, 0, 0) == 1,
           "CAP_NET_BIND_SERVICE dropped (should be kept)");
    CCHECK(prctl(PR_CAPBSET_READ, CAP_SYS_CHROOT, 0, 0, 0) == 1,
           "CAP_SYS_CHROOT dropped (should be kept)");
    CCHECK(prctl(PR_CAPBSET_READ, CAP_SETUID, 0, 0, 0) == 1,
           "CAP_SETUID dropped (should be kept)");

    /* Dropped (the dangerous set). */
    CCHECK(prctl(PR_CAPBSET_READ, CAP_SYS_ADMIN, 0, 0, 0) == 0,
           "CAP_SYS_ADMIN still in bounding set");
    CCHECK(prctl(PR_CAPBSET_READ, CAP_SYS_MODULE, 0, 0, 0) == 0,
           "CAP_SYS_MODULE still in bounding set");
    CCHECK(prctl(PR_CAPBSET_READ, CAP_NET_ADMIN, 0, 0, 0) == 0,
           "CAP_NET_ADMIN still in bounding set");
    CCHECK(prctl(PR_CAPBSET_READ, CAP_SYS_PTRACE, 0, 0, 0) == 0,
           "CAP_SYS_PTRACE still in bounding set");
    return 0;
}

/* ---- Full chain in the real child_func order (root only):
 *      drop_capabilities -> apply_no_new_privs -> apply_seccomp_filter.
 *      Proves the three compose, and proves the cap/seccomp asymmetry:
 *      CAP_SYS_CHROOT is KEPT yet chroot() is still EPERM because the
 *      seccomp layer excludes it. ---- */
static int child_full_chain(void)
{
    CCHECK(drop_capabilities(false) == 0, "drop_capabilities failed");
    CCHECK(apply_no_new_privs(false) == 0, "nnp failed");
    CCHECK(apply_seccomp_filter(false) == 0, "seccomp install failed");

    CCHECK(prctl(PR_GET_SECCOMP, 0, 0, 0, 0) == 2,
           "seccomp not in filter mode after the full chain");

    /* The asymmetry: cap kept, syscall denied. */
    errno = 0;
    CCHECK(chroot(".") == -1 && errno == EPERM,
           "chroot() not EPERM despite kept CAP_SYS_CHROOT (asymmetry broken)");

    errno = 0;
    CCHECK(unshare(CLONE_NEWNS) == -1 && errno == EPERM,
           "unshare(CLONE_NEWNS) not EPERM after full chain");
    return 0;
}

/* ---- Seccomp install must FAIL without NO_NEW_PRIVS when the caller
 *      lacks CAP_SYS_ADMIN. Only meaningful unprivileged: as root,
 *      CAP_SYS_ADMIN authorizes the install regardless, so the parent
 *      gates this on non-root. (errno after the failed call is not
 *      asserted — perror/free between the prctl and the return may
 *      perturb it; the -1 return is the reliable signal.) ---- */
static int child_seccomp_requires_nnp(void)
{
    /* Intentionally NO apply_no_new_privs() first. Expect the install to
     * be refused (the perror it prints is expected test noise). */
    CCHECK(apply_seccomp_filter(false) == -1,
           "seccomp install unexpectedly succeeded without NO_NEW_PRIVS");
    return 0;
}

/* ---- state_claim_id: reversible, so no fork. Each claim mints a fresh
 *      ID and creates its state dir; two claims must differ. The 3-retry
 *      EEXIST path can't be forced deterministically (it needs two
 *      /dev/urandom reads to collide), so we verify the success path and
 *      uniqueness, matching the guide's honesty about that limit. ---- */
static int test_claim_id_unique_and_creates_dir(void)
{
    char id1[CONTAINER_ID_LEN + 1] = {0};
    char id2[CONTAINER_ID_LEN + 1] = {0};

    CHECK(state_claim_id(id1) == 0, "first state_claim_id failed");
    CHECK(strlen(id1) == CONTAINER_ID_LEN, "claimed id has wrong length");

    char dir[PATH_MAX];
    struct stat st;
    state_dir_path(id1, dir, sizeof(dir));
    CHECK(stat(dir, &st) == 0 && S_ISDIR(st.st_mode),
          "state dir not created by state_claim_id");

    CHECK(state_claim_id(id2) == 0, "second state_claim_id failed");
    CHECK(strcmp(id1, id2) != 0, "two claims returned identical IDs");

    state_remove(id1);
    state_remove(id2);
    printf("PASS: test_claim_id_unique_and_creates_dir\n");
    return 0;
}

int main(void)
{
    int rc = 0;
    bool root = (geteuid() == 0);
    printf("=== test_hardening (%s) ===\n",
           root ? "root" : "unprivileged");

    /* Need only NO_NEW_PRIVS -> run regardless of privilege. */
    rc |= run_child("no_new_privs", child_no_new_privs);
    rc |= run_child("seccomp_allow_deny_clone3", child_seccomp);

    /* Capability dropping needs CAP_SETPCAP -> root only. */
    if (root) {
        rc |= run_child("drop_capabilities", child_drop_caps);
        rc |= run_child("full_hardening_chain", child_full_chain);
    } else {
        printf("SKIP: drop_capabilities (needs root / CAP_SETPCAP)\n");
        printf("SKIP: full_hardening_chain (needs root)\n");
        /* The nnp-ordering rule is only observable unprivileged. */
        rc |= run_child("seccomp_requires_nnp_unpriv",
                        child_seccomp_requires_nnp);
    }

    /* Reversible; uses state_dir_root() (root: /run, else XDG path). */
    rc |= test_claim_id_unique_and_creates_dir();

    if (rc == 0) {
        printf("\nAll hardening tests passed!\n");
    }
    return rc;
}
