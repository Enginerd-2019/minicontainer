#ifndef CORE_H
#define CORE_H

// NOTE: _GNU_SOURCE is provided by the Makefile via -D_GNU_SOURCE

#include <sched.h>
#include <stdbool.h>
#include <sys/types.h>
#include "cgroup.h"   // cgroup_limits_t, cgroup_context_t
#include "overlay.h"  // overlay_context_t
#include "net.h"      // veth_config_t, net_context_t
#include "state.h"
#include "mount.h"

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

    // Canonical container identity (Phase 7b).  Generated once per
    // `cmd_run` by state.h's generate_container_id() and threaded
    // into every downstream module (overlay, cgroup, state file, veth
    // naming).  An empty string is rejected by setup_overlay and is
    // a bug — the CLI is responsible for populating this before
    // calling container_start / container_exec.  See §3.5
    // "Canonical container-ID source" and decisions.md.
    char container_id[CONTAINER_ID_LEN + 1];

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

    // Bind mounts (Phase 7b)
    bind_mount_t mounts[MAX_MOUNTS];
    int mount_count;

    // Interactive PTY (Phase 7b). Single boolean — the slave path is
    // allocated inside the child after mount_devpts, and the master fd
    // is returned to the caller via container_result_t.pty_master_fd.
    bool enable_pty;

    // Detached mode (Phase 7b)
    bool detach;
    int  stdout_fd;            // -1 = inherit; cmd_start writes to log file
    int  stderr_fd;            // -1 = inherit; cmd_start writes to log file
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
    int pty_master_fd;
} container_result_t;

/**
 * Phase 7b: parent-side container setup.  Runs the original Phase 7a
 * pipeline through the clone(2) call and the post-clone wiring
 * (close-read-end of sync pipe, user-ns map, setup_net, write-sync,
 * add_pid_to_cgroup), then returns.  Does NOT wait for the child.
 *
 * The split exists so cmd_run can write the state file BETWEEN
 * launch and reap (`cmd_run`: start → state_save → wait), and so
 * cmd_start can launch a detached container by calling start and
 * exiting without ever calling wait.
 *
 * On success, `result.child_pid > 0` and `result.ctx` is populated
 * with whatever was allocated (overlay, cgroup, net, stack).  On
 * failure, `result.child_pid < 0`; caller should still call
 * container_cleanup(&result) to release any partial allocations.
 *
 * If `config->enable_pty`, the parent sets up a SOCK_STREAM
 * socketpair before clone and receives the master fd via SCM_RIGHTS
 * after clone.  On success the master fd is stored on
 * `result.pty_master_fd` (-1 otherwise).  Caller owns the fd.
 *
 * @param config  Container configuration (Phase 7a + 7b fields)
 * @return        Result with child_pid set and ctx populated
 */
container_result_t container_start(const container_config_t *config);

/**
 * Phase 7b: parent-side waitpid + overlay teardown.  Companion to
 * container_start.  Mutates `result` in place:
 *   - result->exit_status   = WEXITSTATUS(status) on normal exit
 *   - result->exited_normally = true on normal exit, false on signal
 *   - result->signal        = WTERMSIG(status) when killed by signal
 * Overlay teardown runs here (immediately after waitpid) because the
 * overlay's per-execution lifetime ends with the child.
 *
 * Caller is responsible for calling container_cleanup() afterward
 * (or for not calling it — cmd_start deliberately skips both wait
 * and cleanup so the detached container survives the parent's exit).
 *
 * @param result        Result from container_start (mutated in place)
 * @param enable_debug  Verbose teardown logging
 */
void container_wait(container_result_t *result, bool enable_debug);

/**
 * Phase 7a's monolithic entry point.  After 7b's split, this is a
 * thin wrapper: container_start() followed by container_wait().
 * Existing Phase 7a tests and the transitional main.c keep working
 * unchanged.  cmd_run does NOT call this — it needs the state-file
 * write between the two halves and so calls them directly.
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