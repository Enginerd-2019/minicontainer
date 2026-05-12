#ifndef CGROUP_H
#define CGROUP_H

// NOTE: Define _GNU_SOURCE before including this header in your .c files
// or compile with -D_GNU_SOURCE

#include <sched.h>
#include <stdbool.h>
#include <sys/types.h>

/**
 * Resource limits for cgroup.
 */
typedef struct {
    size_t memory_limit;     // Bytes, 0 = unlimited
    long cpu_quota;          // Microseconds, 0 = unlimited
    long cpu_period;         // Microseconds, default 100000
    size_t pid_limit;        // Max processes, 0 = unlimited
} cgroup_limits_t;

/**
 * Cgroup runtime context.
 */
typedef struct {
    char cgroup_path[256];
    char cgroup_name[64];
    bool created;
} cgroup_context_t;

/**
 * Configuration for container with cgroup resource limits.
 * Supersedes uts_config_t — includes all Phase 4c fields plus cgroup.
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
    bool enable_user_namespace;
    bool enable_ipc_namespace;

    // Filesystem
    const char *rootfs_path;
    bool enable_overlay;
    const char *container_dir;

    // UTS settings
    const char *hostname;

    // User namespace mapping
    uid_t uid_map_inside;
    uid_t uid_map_outside;
    size_t uid_map_range;
    gid_t gid_map_inside;
    gid_t gid_map_outside;
    size_t gid_map_range;

    // Cgroup limits
    cgroup_limits_t cgroup_limits;
    bool enable_cgroup;
} cgroup_config_t;

/**
 * Result of container execution with cgroup.
 */
typedef struct {
    pid_t child_pid;
    int exit_status;
    bool exited_normally;
    int signal;
    void *stack_ptr;
    cgroup_context_t cgroup_ctx;
} cgroup_result_t;

/**
 * Execute process with optional cgroup resource limits.
 *
 * Supersedes uts_exec() — handles all Phase 4c functionality plus
 * cgroup resource control.
 *
 * @param config  Configuration including namespace, filesystem, and cgroup settings
 * @return        Result structure (includes cgroup context for cleanup)
 */
cgroup_result_t cgroup_exec(const cgroup_config_t *config);

/**
 * Cleanup resources allocated by cgroup_exec.
 * Removes cgroup directory and frees stack.
 *
 * @param result  Result from cgroup_exec
 */
void cgroup_cleanup(cgroup_result_t *result);

/**
 * Create and setup cgroup for container.
 * Called by PARENT before clone().
 *
 * @param ctx          Cgroup context (output — populated with path and name)
 * @param limits       Resource limits to apply
 * @param enable_debug Enable debug output
 * @return             0 on success, -1 on failure
 */
int setup_cgroup(cgroup_context_t *ctx, const cgroup_limits_t *limits,
                 bool enable_debug);

/**
 * Add PID to cgroup.
 * Called by PARENT after clone().
 *
 * @param ctx          Cgroup context
 * @param pid          PID to add
 * @param enable_debug Enable debug output
 * @return             0 on success, -1 on failure
 */
int add_pid_to_cgroup(const cgroup_context_t *ctx, pid_t pid,
                      bool enable_debug);

/**
 * Remove cgroup directory.
 * Called after container exits. Cgroup must be empty (no processes).
 *
 * @param ctx          Cgroup context
 * @param enable_debug Enable debug output
 */
void remove_cgroup(cgroup_context_t *ctx, bool enable_debug);

#endif // CGROUP_H