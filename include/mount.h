#ifndef MOUNT_H
#define MOUNT_H

#include <limits.h>
#include <sched.h>
#include <stdbool.h>
#include <sys/types.h>
#include "oci.h"   // Phase 8c: oci_mount_t for apply_oci_mount (one-directional: mount.h -> oci.h)

#define MAX_MOUNTS 32

typedef struct {
    char host_path[PATH_MAX];
    char container_path[PATH_MAX];
    bool readonly;
} bind_mount_t;


/**
 * Setup rootfs using pivot_root, applying any user bind mounts onto
 * the new root BEFORE the pivot.  Called by child process before
 * execve.
 *
 * The bind mounts MUST be applied here, while the host source paths
 * are still reachable in the child's mount namespace: pivot_root
 * detaches the old (host) root, after which an absolute host source
 * path no longer resolves.  Each mount lands under the new root so the
 * pivot carries it along.  (See decisions.md Error #22.)
 *
 * @param rootfs_path  Path to new root filesystem
 * @param mounts       Array of bind mounts to apply (may be NULL)
 * @param mount_count  Number of entries in `mounts` (0 = none)
 * @param enable_debug Enable debug output
 * @return             0 on success, -1 on failure
 */
int setup_rootfs(const char *rootfs_path, const bind_mount_t *mounts,
                 int mount_count, bool enable_debug);

/**
 * Mount /proc filesystem inside container.
 *
 * @param enable_debug Enable debug output
 * @return             0 on success, -1 on failure
 */
int mount_proc(bool enable_debug);

/**
 * Apply one bind mount inside the container's mount namespace.
 * Called by setup_rootfs before pivot_root (so the host source is
 * still reachable).
 *
 * The on-disk target is `root_prefix` + `m->container_path`.  At the
 * normal call site the bind runs before pivot_root, so `root_prefix`
 * is the absolute path of the new root (e.g. "/path/to/rootfs") and
 * the target resolves inside the future container root rather than
 * against the still-current host root.  Pass NULL/empty to use
 * `m->container_path` verbatim — the form the unit tests use, since
 * they fork into a private mount namespace without pivot_root.
 *
 * @param m             Bind mount specification (host_path absolute)
 * @param root_prefix   Path prepended to container_path (may be NULL)
 * @param enable_debug  Verbose
 * @return  0 on success, -1 on failure
 */
int bind_mount_apply(const bind_mount_t *m, const char *root_prefix,
                     bool enable_debug);

/**
 * Mount a private devpts instance on /dev/pts inside the container's
 * mount namespace, then create /dev/ptmx as a symlink to
 * /dev/pts/ptmx so posix_openpt(2) opens the newinstance master
 * multiplexer.
 *
 * Called by child_func unconditionally after mount_proc (when a
 * rootfs was set, so a mount namespace exists). Required for any
 * pty-allocating program inside the container, including but not
 * limited to --interactive.
 *
 * On failure with user_namespace_active, prints a warning and
 * returns 0 — same graceful-degradation pattern mount_proc uses,
 * because AppArmor on Ubuntu can deny devpts mounts inside an
 * unprivileged user namespace.
 *
 * @param enable_debug          Verbose
 * @param user_namespace_active Whether CLONE_NEWUSER was set
 *                              (controls warn-vs-fail behavior)
 * @return  0 on success or graceful degradation, -1 on hard failure
 */
int mount_devpts(bool enable_debug, bool user_namespace_active);

/*
 * Mount /sys as read-only inside the container's mount namespace.
 * Called AFTER mount_proc but BEFORE configure_container_net.
 *
 * Creates /sys first (idempotent mkdir) — the build_rootfs.sh
 * minimal rootfs has no /sys directory.  If sysfs is already
 * mounted (EBUSY), remounts it read-only instead.
 *
 * Phase 8b — used only when enable_hardening is true.  Fails closed:
 * no user-namespace graceful degradation (see §11.10).
 *
 * Returns 0 on success, -1 on failure.
 */
int mount_sys_ro(bool enable_debug);

/*
 * Phase 8c — Apply a single NON-bind OCI mount entry from config.json's
 * mounts[] array.  Dispatches by mount.type: "proc"/"devpts" defer to
 * mount_proc/mount_devpts (skip), "tmpfs" → tmpfs mount (mkdir target
 * first, EEXIST-tolerant), "sysfs" → sysfs RW (mount_sys_ro handles the
 * --secure RO case separately).
 *
 * "bind"-type entries are NOT handled here: their host source is
 * unreachable post-pivot_root (decisions.md Error #22), so they are
 * converted to bind_mount_t at parse time and applied by setup_rootfs
 * before the pivot — the same path as --volume.  Reaching this function
 * with a bind entry is treated as a bug (returns -1, EINVAL).
 *
 * Called from child_func per non-bind OCI mount entry, AFTER pivot_root.
 * Returns 0 on success, -1 on failure.
 */
int apply_oci_mount(const oci_mount_t *m, bool enable_debug);

/*
 * Phase 8c — Mask a path for OCI linux.maskedPaths[] entries.  A regular
 * file is masked by bind-mounting /dev/null over it; a directory by
 * mounting an empty read-only tmpfs over it.  Missing targets are
 * skipped silently (OCI semantics).  Returns 0 on success/skip, -1 on
 * failure.
 */
int apply_masked_path(const char *path, bool enable_debug);

/*
 * Phase 8c — Remount a path read-only for OCI linux.readonlyPaths[]
 * entries via self-bind then MS_BIND | MS_REMOUNT | MS_RDONLY (the same
 * kernel two-step quirk Phase 7b's read-only bind mounts use).  Missing
 * targets are skipped silently.  Returns 0 on success/skip, -1 on
 * failure.
 */
int apply_readonly_path(const char *path, bool enable_debug);

#endif // MOUNT_H