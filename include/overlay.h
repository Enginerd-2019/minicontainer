#ifndef OVERLAY_H
#define OVERLAY_H

// NOTE: Define _GNU_SOURCE before including this header in your .c files
// or compile with -D_GNU_SOURCE

#include <sched.h>
#include <stdbool.h>
#include <sys/types.h>
#include <limits.h>

/**
 * Overlay mount context (for setup and teardown).
 */
typedef struct {
    char container_id[13];
    char lower_path[PATH_MAX];
    char container_base[PATH_MAX];
    char upper_path[PATH_MAX];
    char work_path[PATH_MAX];
    char merged_path[PATH_MAX];
    bool is_mounted;
} overlay_context_t;

/**
 * Setup overlay filesystem.
 * Creates directories and mounts overlayfs with MS_NODEV | MS_NOSUID.
 *
 * @param ctx          Overlay context (populated on success)
 * @param rootfs_path  Path to base image (lowerdir)
 * @param container_dir Parent directory for overlay data
 * @param enable_debug Enable debug output
 * @return             0 on success, -1 on failure
 */
int setup_overlay(overlay_context_t *ctx, const char *rootfs_path,
                  const char *container_dir, bool enable_debug);

/**
 * Teardown overlay filesystem.
 * Unmounts overlayfs and removes directories.
 *
 * @param ctx          Overlay context from setup_overlay
 * @param enable_debug Enable debug output
 * @return             0 on success, -1 on failure
 */
int teardown_overlay(overlay_context_t *ctx, bool enable_debug);

#endif // OVERLAY_H