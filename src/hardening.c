/*
 * hardening.c — Linux security hardening primitives
 *
 * Three independent functions implementing capability drops,
 * NO_NEW_PRIVS, and a seccomp BPF allow-list. All use raw syscalls
 * (capset, prctl) — no libcap or libseccomp dependencies.
 *
 * Called from core.c's child_func when args->enable_hardening is set,
 * in this order: drop_capabilities → apply_no_new_privs →
 * apply_seccomp_filter → execve.
 */

#include "hardening.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>
#include <stddef.h>
#include <stdint.h>

/*
 * __NR_capset on x86_64 is 126. On other architectures the syscall
 * number differs — this guide assumes x86_64 (matching the seccomp
 * filter's AUDIT_ARCH_X86_64 architecture check in apply_seccomp_filter).
 * Porting to aarch64 etc. would need an architecture-specific number
 * and an AUDIT_ARCH switch in the BPF program.
 */
#ifndef __NR_capset
#define __NR_capset 126   /* x86_64 — see <asm/unistd_64.h> */
#endif

#ifndef __NR_clone3
#define __NR_clone3 435   /* x86_64 — absent from pre-5.3 kernel headers */
#endif

/* ===== Capabilities ===== */

/*
 * Capabilities to KEEP. Everything else is dropped.
 *
 * These map to constants in <linux/capability.h>. We list them by
 * macro name + numeric value for clarity. The numbering is stable
 * across kernel versions (new caps go at the end).
 */
static const int kept_capabilities[] = {
    CAP_AUDIT_WRITE,           /* 29 */
    CAP_CHOWN,                 /* 0  */
    CAP_DAC_OVERRIDE,          /* 1  */
    CAP_FOWNER,                /* 3  */
    CAP_FSETID,                /* 4  */
    CAP_KILL,                  /* 5  */
    CAP_MKNOD,                 /* 27 */
    CAP_NET_BIND_SERVICE,      /* 10 */
    CAP_NET_RAW,               /* 13 */
    CAP_SETFCAP,               /* 31 */
    CAP_SETGID,                /* 6  */
    CAP_SETPCAP,               /* 8  */
    CAP_SETUID,                /* 7  */
    CAP_SYS_CHROOT,            /* 18 */
};

#define N_KEPT (sizeof(kept_capabilities) / sizeof(kept_capabilities[0]))

static bool is_kept(int cap)
{
    for (size_t i = 0; i < N_KEPT; i++) {
        if (kept_capabilities[i] == cap) return true;
    }
    return false;
}

int drop_capabilities(bool enable_debug)
{
    /* Step 1: drop unwanted caps from the BOUNDING set.
     * This is permanent — once dropped, the cap cannot be reacquired
     * even by setuid binaries or file capabilities. */
    for (int cap = 0; cap <= CAP_LAST_CAP; cap++) {
        if (is_kept(cap)) continue;

        if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0) {
            if (errno == EINVAL) {
                /* This cap doesn't exist on this kernel — fine, skip. */
                continue;
            }
            perror("prctl(PR_CAPBSET_DROP)");
            return -1;
        }
    }

    if (enable_debug) {
        fprintf(stderr, "[hardening] Dropped bounding-set caps; kept %zu caps\n",
                N_KEPT);
    }

    /* Step 2: modify the permitted / effective / inheritable sets via
     * capset(). We use _LINUX_CAPABILITY_VERSION_3 which requires the
     * data array to have 2 elements (for caps 0-31 and caps 32-63). */
    struct __user_cap_header_struct hdr = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid     = 0,
    };
    struct __user_cap_data_struct data[2] = {0};

    /* Build bitmasks for the kept caps */
    for (size_t i = 0; i < N_KEPT; i++) {
        int cap = kept_capabilities[i];
        int idx = cap / 32;
        int bit = cap % 32;
        data[idx].effective   |= (1u << bit);
        data[idx].permitted   |= (1u << bit);
        /* Inheritable left clear — kept caps stay with this process
         * but don't propagate via execve. NO_NEW_PRIVS handles the
         * inheritance concern more aggressively. */
    }

    if (syscall(__NR_capset, &hdr, data) < 0) {
        perror("capset");
        return -1;
    }

    if (enable_debug) {
        fprintf(stderr, "[hardening] Capability sets applied: "
                "effective=0x%08x.%08x permitted=0x%08x.%08x\n",
                data[1].effective, data[0].effective,
                data[1].permitted, data[0].permitted);
    }

    return 0;
}

/* ===== NO_NEW_PRIVS ===== */

int apply_no_new_privs(bool enable_debug)
{
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        perror("prctl(PR_SET_NO_NEW_PRIVS)");
        return -1;
    }

    if (enable_debug) {
        fprintf(stderr, "[hardening] NO_NEW_PRIVS set\n");
    }
    return 0;
}

/* ===== Seccomp BPF allow-list =====
 *
 * BPF program structure:
 *   Validate arch (x86_64) — kill if mismatch
 *   Load syscall nr
 *   For each allowed syscall: if (nr == syscall) return ALLOW
 *   clone3: return ERRNO(ENOSYS) — glibc only falls back to clone
 *           on ENOSYS, never on EPERM (see §3.2.1)
 *   Default: return ERRNO(EPERM)
 *
 * clone is in the allow-list unconditionally; CLONE_NEW* flags are
 * blocked by the preceding capability drop (CAP_SYS_ADMIN gone),
 * not by seccomp arg inspection. See §3.2.1.
 */

/* List of syscall numbers we explicitly allow. Mirrors the array in
 * the guide §4.2 — keep the two in sync. */
static const int allowed_syscalls[] = {
    /* Process — note: clone is allowed unconditionally; CAP_SYS_ADMIN
     * was just dropped by drop_capabilities(), so any CLONE_NEW* flags
     * the container tries will fail at the kernel's cap check.
     * See §3.2.1. clone3 and unshare/setns are deliberately omitted. */
    __NR_clone, __NR_fork, __NR_vfork, __NR_execve, __NR_execveat,
    __NR_exit, __NR_exit_group, __NR_wait4, __NR_waitid,
    __NR_getpid, __NR_gettid, __NR_getppid,
    __NR_setpgid, __NR_getpgid, __NR_getpgrp, __NR_setsid, __NR_getsid,
    __NR_setuid, __NR_setgid, __NR_geteuid, __NR_getegid,
    __NR_getuid, __NR_getgid, __NR_setresuid, __NR_setresgid,
    __NR_getresuid, __NR_getresgid, __NR_setgroups, __NR_getgroups,
    __NR_capget, __NR_capset, __NR_kill, __NR_tkill, __NR_tgkill,
    /* File I/O */
    __NR_read, __NR_write, __NR_pread64, __NR_pwrite64,
    __NR_readv, __NR_writev, __NR_preadv, __NR_pwritev,
    __NR_open, __NR_openat, __NR_close, __NR_creat,
    __NR_stat, __NR_fstat, __NR_lstat, __NR_newfstatat,
    __NR_statx,   /* modern coreutils stat through statx; glibc has NO
                   * EPERM fallback to fstatat — omit this and `ls` dies */
    __NR_lseek, __NR_pipe, __NR_pipe2,
    __NR_dup, __NR_dup2, __NR_dup3,
    __NR_fcntl, __NR_ioctl,
    __NR_getdents, __NR_getdents64,
    __NR_select, __NR_pselect6, __NR_poll, __NR_ppoll,
    __NR_epoll_create, __NR_epoll_create1, __NR_epoll_wait,
    __NR_epoll_pwait, __NR_epoll_ctl,
    __NR_eventfd, __NR_eventfd2,
    __NR_signalfd, __NR_signalfd4,
    __NR_timerfd_create, __NR_timerfd_settime, __NR_timerfd_gettime,
    /* Filesystem */
    __NR_chdir, __NR_fchdir, __NR_getcwd,
    __NR_mkdir, __NR_mkdirat, __NR_rmdir,
    __NR_unlink, __NR_unlinkat,
    __NR_rename, __NR_renameat, __NR_renameat2,
    __NR_link, __NR_linkat, __NR_symlink, __NR_symlinkat,
    __NR_readlink, __NR_readlinkat,
    __NR_chmod, __NR_fchmod, __NR_fchmodat,
    __NR_chown, __NR_fchown, __NR_lchown, __NR_fchownat,
    __NR_access, __NR_faccessat, __NR_faccessat2,
    __NR_umask, __NR_statfs, __NR_fstatfs,
    __NR_truncate, __NR_ftruncate,
    __NR_sync, __NR_fsync, __NR_fdatasync, __NR_syncfs,
    __NR_utime, __NR_utimes, __NR_utimensat, __NR_futimesat,
    __NR_flock, __NR_fallocate, __NR_fadvise64,
    /* xattrs — tar/cp -a metadata, and the syscall path behind kept
     * CAP_SETFCAP.  Note NO_NEW_PRIVS neuters file caps on execve, so
     * allowing setxattr does not reopen the privilege-escalation door. */
    __NR_setxattr, __NR_lsetxattr, __NR_fsetxattr,
    __NR_getxattr, __NR_lgetxattr, __NR_fgetxattr,
    __NR_listxattr, __NR_llistxattr, __NR_flistxattr,
    __NR_removexattr, __NR_lremovexattr, __NR_fremovexattr,
    /* NOTE: chroot, mknod, mknodat are deliberately ABSENT even though
     * CAP_SYS_CHROOT / CAP_MKNOD are kept — see §3.1's asymmetry note. */
    /* Memory */
    __NR_mmap, __NR_mprotect, __NR_munmap, __NR_brk,
    __NR_madvise, __NR_mlock, __NR_munlock, __NR_mlockall,
    __NR_munlockall, __NR_mremap, __NR_msync, __NR_mincore,
    /* Signals */
    __NR_rt_sigaction, __NR_rt_sigprocmask, __NR_rt_sigreturn,
    __NR_rt_sigtimedwait, __NR_rt_sigsuspend, __NR_rt_sigpending,
    __NR_rt_sigqueueinfo, __NR_rt_tgsigqueueinfo,
    __NR_sigaltstack, __NR_pause, __NR_alarm,
    __NR_setitimer, __NR_getitimer,
    /* Networking */
    __NR_socket, __NR_socketpair, __NR_connect, __NR_accept, __NR_accept4,
    __NR_bind, __NR_listen, __NR_getsockname, __NR_getpeername,
    __NR_sendto, __NR_sendmsg, __NR_sendmmsg,
    __NR_recvfrom, __NR_recvmsg, __NR_recvmmsg,
    __NR_setsockopt, __NR_getsockopt, __NR_shutdown,
    /* Sync */
    __NR_futex, __NR_set_robust_list, __NR_get_robust_list,
    __NR_membarrier,
    __NR_rseq,    /* glibc ≥ 2.35 registers rseq at startup */
    /* Time */
    __NR_time, __NR_gettimeofday,
    __NR_clock_gettime, __NR_clock_getres, __NR_clock_nanosleep,
    __NR_nanosleep, __NR_times,
    /* Misc */
    __NR_uname, __NR_sysinfo, __NR_sched_yield,
    __NR_sched_getparam, __NR_sched_setparam,
    __NR_sched_getscheduler, __NR_sched_setscheduler,
    __NR_sched_get_priority_max, __NR_sched_get_priority_min,
    __NR_sched_rr_get_interval, __NR_sched_getaffinity,
    __NR_getrlimit, __NR_setrlimit, __NR_prlimit64,
    __NR_getrusage, __NR_personality,
    __NR_arch_prctl, __NR_prctl,
    __NR_set_tid_address, __NR_set_thread_area, __NR_get_thread_area,
    __NR_getrandom, __NR_seccomp,
};
#define N_ALLOWED (sizeof(allowed_syscalls) / sizeof(allowed_syscalls[0]))

/* Convenience macros for BPF program assembly */
#define ALLOW       BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
#define DENY        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM  & SECCOMP_RET_DATA))
#define DENY_ENOSYS BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (ENOSYS & SECCOMP_RET_DATA))
#define KILL        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS)

int apply_seccomp_filter(bool enable_debug)
{
    /*
     * Build the BPF program dynamically:
     *   1. Validate arch (x86_64). If not, KILL.
     *   2. Load syscall nr.
     *   3. For each allowed syscall, JEQ check; on match, ALLOW.
     *   4. clone3: ENOSYS, not the EPERM default (glibc fallback rule).
     *   5. Default: DENY (EPERM).
     *
     * Instruction count — keep this arithmetic EXACTLY in step with
     * the emission code below; the i != n_inst sanity check fails the
     * whole filter (and thus every --secure run) on any drift:
     *     3  arch check (load + jump + KILL)
     *   + 1  load syscall nr        (ONE instruction, not two)
     *   + 2 * N_ALLOWED             (JEQ + ALLOW per syscall)
     *   + 2  clone3 special case    (JEQ + ENOSYS)
     *   + 1  final DENY
     *   = 7 + 2 * N_ALLOWED   (~340 instructions for ~165 syscalls)
     */

    size_t n_inst = 3 + 1 + 2 * N_ALLOWED + 2 + 1;
    struct sock_filter *filter = calloc(n_inst, sizeof(*filter));
    if (filter == NULL) {
        perror("calloc(seccomp filter)");
        return -1;
    }

    size_t i = 0;

    /* === 1. Validate arch === */
    /* Load arch field */
    filter[i++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                                offsetof(struct seccomp_data, arch));
    /* If arch == AUDIT_ARCH_X86_64, fall through to next instruction.
     * Else, kill. */
    filter[i++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                                AUDIT_ARCH_X86_64, 1, 0);
    filter[i++] = (struct sock_filter)KILL;

    /* === 2. Load syscall nr === */
    filter[i++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                                offsetof(struct seccomp_data, nr));

    /* === 3. Allow-list check === */
    /* For each allowed syscall, emit:
     *   JEQ nr, allowed[i], jt=1, jf=0   ; if equal, jump 1 (to ALLOW)
     *   (continue to next check)
     *
     * But we need to ALLOW after the match. Pattern:
     *   JEQ nr, syscall_N, jt=0, jf=1   ; if equal, fall through to next inst (ALLOW)
     *   ALLOW
     *
     * That's 2 instructions per allowed syscall. After all are checked,
     * fall through to the final DENY. */
    for (size_t s = 0; s < N_ALLOWED; s++) {
        filter[i++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                                    allowed_syscalls[s], 0, 1);
        filter[i++] = (struct sock_filter)ALLOW;
    }

    /* === 4. clone3 → ENOSYS (NOT the EPERM default) === */
    /* glibc ≥ 2.34's pthread_create tries clone3 first and falls back
     * to plain clone ONLY on ENOSYS.  Letting clone3 hit the EPERM
     * default breaks thread creation in every threaded program.
     * ENOSYS grants nothing — it just steers glibc onto the clone
     * path, which the capability drop constrains.  See §3.2.1. */
    filter[i++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                                __NR_clone3, 0, 1);
    filter[i++] = (struct sock_filter)DENY_ENOSYS;

    /* === 5. Default: DENY === */
    filter[i++] = (struct sock_filter)DENY;

    /* Sanity: did we fill the expected number of instructions? */
    if (i != n_inst) {
        fprintf(stderr, "[hardening] BPF program size mismatch: %zu vs %zu\n",
                i, n_inst);
        free(filter);
        return -1;
    }

    struct sock_fprog prog = {
        .len    = (unsigned short)n_inst,
        .filter = filter,
    };

    /* PR_SET_NO_NEW_PRIVS must already be set (or we must hold
     * CAP_SYS_ADMIN — but we just dropped it). */
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) < 0) {
        perror("prctl(PR_SET_SECCOMP)");
        if (errno == EACCES) {
            fprintf(stderr, "[hardening] EACCES — did you call "
                    "apply_no_new_privs() first?\n");
        }
        free(filter);
        return -1;
    }

    if (enable_debug) {
        fprintf(stderr, "[hardening] Seccomp filter installed: %zu instructions, "
                "%zu allow-listed syscalls\n", n_inst, N_ALLOWED);
    }

    /* The kernel made its own copy of the BPF program — we can free ours. */
    free(filter);
    return 0;
}