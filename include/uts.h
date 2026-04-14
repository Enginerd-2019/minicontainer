#ifndef UTS_H
#define UTS_H

#include <sched.h>
#include <stdbool.h>
#include <sys/types.h>
#include <limits.h>

/**
 * Configuration for container with UTS namespace support.
 * Supersedes overlay_config_t — includes all Phase 3 fields plus hostname.
 */
typedef struct {
    const char *program;
    char *const *argv;
    char *const *envp;
    bool enable_debug;

    // Namespace flags
    bool enable_pid_namespace;
    bool enable_mount_namespace;
    bool enable_uts_namespace;

    // Filesystem
    const char *rootfs_path;
    bool enable_overlay;
    const char *container_dir;

    // UTS settings
    const char *hostname;       // NULL = no hostname change
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

#endif