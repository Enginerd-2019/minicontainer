#ifndef OVERLAY_H
#define OVERLAY_H

// NOTE: Define _GNU_SOURCE before including this header in your .c files
// or compile with -D_GNU_SOURCE

#include <sched.h>
#include <stdbool.h>
#include <sys/types.h>
#include <limits.h>

/**
 * Configuration for overlay filesystem container.
 * Supersedes mount_config_t — includes all Phase 2 fields plus overlay.
 */
typedef struct {
    const char *program;
    char *const *argv;
    char *const *envp;
    bool enable_debug;

    // Namespace flags
    bool enable_pid_namespace;
    bool enable_mount_namespace;

    // Filesystem
    const char *rootfs_path;       // Base image (lowerdir), NULL = no rootfs
    bool enable_overlay;           // Use overlayfs on top of rootfs
    const char *container_dir;     // Parent dir for overlay data (default: "./containers")
} overlay_config_t;

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
 * Result of overlay operation.
 */
typedef struct {
    pid_t child_pid;
    int exit_status;
    bool exited_normally;
    int signal;
    void *stack_ptr;
} overlay_result_t;

/**
 * Execute process with optional overlay filesystem isolation.
 *
 * Supersedes mount_exec() — handles all Phase 2 functionality plus
 * copy-on-write overlay filesystem.
 *
 * If enable_overlay is true, creates an overlay mount over rootfs.
 * The container sees a merged view; writes go to a disposable upper layer.
 * On exit, the overlay is torn down and the base image is untouched.
 *
 * @param config  Configuration including rootfs and overlay settings
 * @return        Result structure
 */
overlay_result_t overlay_exec(const overlay_config_t *config);

/**
 * Cleanup resources allocated by overlay_exec.
 *
 * @param result  Result from overlay_exec
 */
void overlay_cleanup(overlay_result_t *result);

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