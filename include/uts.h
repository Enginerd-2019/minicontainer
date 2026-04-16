#ifndef UTS_H
#define UTS_H

#include <sched.h>
#include <stdbool.h>
#include <sys/types.h>
#include <limits.h>

/**
 * Configuration for container with UTS namespace support.
 * Supersedes overlay_config_t — includes all Phase 3 fields plus hostname.
 * After phase 4b, it also includes fields that provide uid and gid mapping
 * as well as a usernamspace enable flag
 */
typedef struct {
    // Base configuration (unchanged from Phase 4)
    const char *program;
    char *const *argv;
    char *const *envp;
    bool enable_debug;

    // Namespace flags (enable_user_namespace is new)
    bool enable_pid_namespace;
    bool enable_mount_namespace;
    bool enable_uts_namespace;
    bool enable_user_namespace;      // New in Phase 4b

    // Filesystem (unchanged)
    const char *rootfs_path;
    bool enable_overlay;
    const char *container_dir;

    // UTS settings (unchanged)
    const char *hostname;

    // User namespace mapping (new in Phase 4b)
    uid_t uid_map_inside;            // UID inside namespace (typically 0)
    uid_t uid_map_outside;           // UID outside namespace (host UID)
    size_t uid_map_range;            // Number of UIDs to map (typically 1)
    gid_t gid_map_inside;            // GID inside namespace (typically 0)
    gid_t gid_map_outside;           // GID outside namespace (host GID)
    size_t gid_map_range;            // Number of GIDs to map (typically 1)
} uts_config_t;

/**
 * Result of container execution.
 */
typedef struct {
    pid_t child_pid;
    int exit_status;
    bool exited_normally;
    int signal;
    void *stack_ptr;
} uts_result_t;

/**
 * Execute process with optional UTS, mount, PID, and overlay isolation.
 *
 * Supersedes overlay_exec() — handles all Phase 3 functionality plus
 * UTS namespace for hostname isolation.
 *
 * @param config  Configuration including namespace and filesystem settings
 * @return        Result structure
 */
uts_result_t uts_exec(const uts_config_t *config);

/**
 * Cleanup resources allocated by uts_exec.
 *
 * @param result  Result from uts_exec
 */
void uts_cleanup(uts_result_t *result);

/**
 * Setup hostname inside UTS namespace.
 * Called by child process after clone(CLONE_NEWUTS).
 *
 * @param hostname     Hostname to set (NULL = no change)
 * @param enable_debug Enable debug output
 * @return             0 on success, -1 on failure
 */
int setup_uts(const char *hostname, bool enable_debug);

/**
 * Setup UID/GID mapping for user namespace.
 * Called by PARENT process after clone(), before signaling child.
 *
 * @param child_pid  PID of child process
 * @param config     Configuration with mapping settings
 * @return           0 on success, -1 on failure
 */
int setup_user_namespace_mapping(pid_t child_pid, const uts_config_t *config);

#endif