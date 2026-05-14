#ifndef CORE_H
#define CORE_H

// NOTE: _GNU_SOURCE is provided by the Makefile via -D_GNU_SOURCE

#include <sched.h>
#include <stdbool.h>
#include <sys/types.h>
#include "cgroup.h"   // cgroup_limits_t, cgroup_context_t
#include "overlay.h"  // overlay_context_t
#include "net.h"      // veth_config_t, net_context_t

/**
 * Unified container configuration. Supersedes every prior *_config_t
 * (mount_config_t, overlay_config_t, uts_config_t, cgroup_config_t,
 * net_config_t). All flags are advisory — if you set
 * enable_uts_namespace=true but hostname=NULL, no harm done; the UTS
 * namespace gets created but the hostname is left unchanged.
 */
typedef struct {
    // Process
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
    bool enable_network;

    // Filesystem
    const char *rootfs_path;
    bool enable_overlay;
    const char *container_dir;

    // UTS
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

    // Network (veth)
    veth_config_t veth;
} container_config_t;

/**
 * Aggregate runtime context populated during container_exec, consumed
 * by container_cleanup. Each sub-context has its own state flags so
 * cleanup is idempotent.
 */
typedef struct {
    overlay_context_t overlay_ctx;
    cgroup_context_t  cgroup_ctx;
    net_context_t     net_ctx;
    void             *stack_ptr;
} container_context_t;

/**
 * Result of container_exec. Contains exit status plus the runtime
 * context needed for cleanup.
 */
typedef struct {
    pid_t child_pid;
    int   exit_status;
    bool  exited_normally;
    int   signal;
    container_context_t ctx;
} container_result_t;

/**
 * Execute a process inside a container described by config. This is
 * the single entry point for spawning a containerized process —
 * supersedes all prior *_exec() functions.
 *
 * The behavior is driven entirely by config flags. To get Phase-5
 * behavior (cgroups but no network): set enable_cgroup=true and
 * enable_network=false. To get Phase-6 behavior (cgroups + network):
 * set both true. Etc.
 *
 * @param config  Container configuration
 * @return        Result with child status and runtime context
 */
container_result_t container_exec(const container_config_t *config);

/**
 * Tear down everything container_exec set up. Idempotent — calling on
 * a zero-initialized result is a no-op.
 *
 * Cleans up (in reverse setup order):
 *   - Host veth + iptables NAT rule (cleanup_net)
 *   - Cgroup directory (remove_cgroup)
 *   - Clone stack (free)
 *
 * Overlay teardown is performed inside container_exec (immediately
 * after waitpid) because the overlay is per-execution and not useful
 * to the caller after the child exits.
 *
 * @param result  Result from container_exec
 */
void container_cleanup(container_result_t *result);

#endif // CORE_H