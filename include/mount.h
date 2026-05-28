#ifndef MOUNT_H
#define MOUNT_H

#include <limits.h>
#include <sched.h>
#include <stdbool.h>
#include <sys/types.h>

#define MAX_MOUNTS 32

typedef struct {
    char host_path[PATH_MAX];
    char container_path[PATH_MAX];
    bool readonly;
} bind_mount_t;


/**
 * Setup rootfs using pivot_root.
 * Called by child process before execve.
 *
 * @param rootfs_path  Path to new root filesystem
 * @param enable_debug Enable debug output
 * @return             0 on success, -1 on failure
 */
int setup_rootfs(const char *rootfs_path, bool enable_debug);

/**
 * Mount /proc filesystem inside container.
 *
 * @param enable_debug Enable debug output
 * @return             0 on success, -1 on failure
 */
int mount_proc(bool enable_debug);

/**
 * Apply one bind mount inside the container's mount namespace.
 * Called by child_func after setup_rootfs, before mount_proc.
 *
 * @param m             Bind mount specification (host_path absolute)
 * @param enable_debug  Verbose
 * @return  0 on success, -1 on failure
 */
int bind_mount_apply(const bind_mount_t *m, bool enable_debug);

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

#endif // MOUNT_H