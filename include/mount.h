#ifndef MOUNT_H
#define MOUNT_H

#include <sched.h>
#include <stdbool.h>
#include <sys/types.h>


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

#endif // MOUNT_H