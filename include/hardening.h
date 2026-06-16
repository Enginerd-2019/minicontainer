#ifndef HARDENING_H
#define HARDENING_H

#include <stdbool.h>

/*
 * Phase 8b — Production Hardening
 *
 * Three independent security primitives. All assume the calling
 * process is the soon-to-execve child of clone(). All are intended
 * to be called in this order:
 *
 *   1. drop_capabilities(enable_debug);
 *   2. apply_no_new_privs(enable_debug);
 *   3. apply_seccomp_filter(enable_debug);
 *   4. execve(...);
 *
 * The order matters: PR_SET_NO_NEW_PRIVS is a kernel prerequisite
 * for installing an unprivileged seccomp filter.
 */

/*
 * Drop all Linux capabilities except a minimal whitelist (~14).
 * Modifies all four cap sets (permitted, effective, inheritable,
 * bounding) so dropped caps can never be reacquired.
 *
 * Uses prctl(PR_CAPBSET_DROP) and capset(2) directly — no libcap.
 *
 * Returns 0 on success, -1 on failure. Failure modes in practice:
 *   - capset(2) returns EPERM (caller is in a process with no
 *     CAP_SETPCAP and is trying to set caps it doesn't currently hold)
 *   - prctl(PR_CAPBSET_DROP) returns EPERM (rare; lacking CAP_SETPCAP)
 * EINVAL on PR_CAPBSET_DROP for a non-existent cap number is handled
 * internally (skipped, not propagated).
 */
int drop_capabilities(bool enable_debug);

/*
 * Set the NO_NEW_PRIVS bit via prctl(PR_SET_NO_NEW_PRIVS, 1).
 * After this call, execve() can no longer grant new privileges
 * through setuid bits, file capabilities, or LSM transitions.
 *
 * Required as a precondition for unprivileged seccomp filter
 * installation (see apply_seccomp_filter below).
 *
 * Returns 0 on success, -1 on failure.
 */
int apply_no_new_privs(bool enable_debug);

/*
 * Install a seccomp BPF filter allowing ~250 of the ~400 kernel
 * syscalls. Dangerous syscalls (mount, unshare, kexec, ptrace, etc.)
 * are either denied with EPERM or kill the process.
 *
 * MUST be called AFTER apply_no_new_privs() — the kernel requires
 * either CAP_SYS_ADMIN or NO_NEW_PRIVS to install a filter.
 *
 * Returns 0 on success, -1 on failure.
 */
int apply_seccomp_filter(bool enable_debug);

#endif /* HARDENING_H */