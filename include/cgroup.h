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