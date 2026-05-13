#ifndef NET_H
#define NET_H

// NOTE: _GNU_SOURCE is provided by the Makefile via -D_GNU_SOURCE.
// Do NOT redefine it here (Error #8 from decisions.md).

#include <sched.h>
#include <stdbool.h>
#include <sys/types.h>
#include <arpa/inet.h>   // INET_ADDRSTRLEN
#include <net/if.h>      // IFNAMSIZ
#include "cgroup.h"      // cgroup_config_t-style fields, cgroup_context_t

/**
 * Veth-specific configuration. Embedded inside net_config_t.
 * Defaults (set by main.c when --net is enabled):
 *   host_ip="10.0.0.1", container_ip="10.0.0.2", netmask="24", enable_nat=true
 */
typedef struct {
    char host_ip[INET_ADDRSTRLEN];
    char container_ip[INET_ADDRSTRLEN];
    char netmask[8];                  // CIDR suffix, e.g., "24"
    bool enable_nat;                  // iptables MASQUERADE for internet access
} veth_config_t;

/**
 * Network runtime context. veth names are populated BEFORE clone() by
 * generate_veth_names() so the child gets a stable snapshot. Other
 * fields populated by setup_net() (parent side, after clone) and
 * consumed by cleanup_net().
 */
typedef struct {
    char veth_host[IFNAMSIZ];                  // e.g., "veth_h_a1b2"
    char veth_container[IFNAMSIZ];             // e.g., "veth_c_a1b2"
    bool veth_created;                         // For idempotent cleanup
    bool nat_added;                            // For idempotent cleanup
    char nat_source_cidr[INET_ADDRSTRLEN + 8]; // Stored so cleanup can
                                               // delete the iptables rule
                                               // without the original config
} net_context_t;

/**
 * Configuration for container with network namespace support.
 * Supersedes cgroup_config_t — includes all Phase 5 fields plus network.
 */
typedef struct {
    // Base configuration (unchanged from Phase 5)
    const char *program;
    char *const *argv;
    char *const *envp;
    bool enable_debug;

    // Namespace flags (enable_network is new)
    bool enable_pid_namespace;
    bool enable_mount_namespace;
    bool enable_uts_namespace;
    bool enable_user_namespace;
    bool enable_ipc_namespace;
    bool enable_network;              // New in Phase 6

    // Filesystem (unchanged)
    const char *rootfs_path;
    bool enable_overlay;
    const char *container_dir;

    // UTS settings (unchanged)
    const char *hostname;

    // User namespace mapping (unchanged from Phase 4b)
    uid_t uid_map_inside;
    uid_t uid_map_outside;
    size_t uid_map_range;
    gid_t gid_map_inside;
    gid_t gid_map_outside;
    size_t gid_map_range;

    // Cgroup limits (unchanged from Phase 5)
    cgroup_limits_t cgroup_limits;
    bool enable_cgroup;

    // Network (new in Phase 6)
    veth_config_t veth;
} net_config_t;

/**
 * Result of container execution with network.
 * Includes both cgroup and net contexts for cleanup.
 */
typedef struct {
    pid_t child_pid;
    int exit_status;
    bool exited_normally;
    int signal;
    void *stack_ptr;
    cgroup_context_t cgroup_ctx;
    net_context_t net_ctx;
} net_result_t;

/**
 * Execute process with optional network namespace isolation.
 *
 * Supersedes cgroup_exec() — handles all Phase 5 functionality plus
 * veth-pair creation, network configuration, and cleanup.
 *
 * @param config  Configuration including namespace, filesystem, cgroup, network
 * @return        Result structure (cgroup + net contexts for cleanup)
 */
net_result_t net_exec(const net_config_t *config);

/**
 * Cleanup resources allocated by net_exec.
 * Deletes host veth (kernel removes container veth with the netns),
 * removes iptables NAT rule if added, removes cgroup, frees stack.
 *
 * @param result  Result from net_exec
 */
void net_cleanup(net_result_t *result);

/**
 * Generate unique veth pair names. Called BEFORE clone() so the names are
 * baked into child_args (which the child reads after the sync pipe).
 * See §3.4.1 for the address-space rationale.
 *
 * Populates ctx->veth_host and ctx->veth_container; does not touch other
 * fields.
 *
 * @param ctx  Network context to populate
 */
void generate_veth_names(net_context_t *ctx);

/**
 * Setup veth pair (host side). Called by PARENT after clone(), before
 * signaling the child via sync pipe.
 *
 * Steps: create veth pair with names from ctx, move container end into
 * child's netns, configure host IP, bring host end up, (optionally)
 * enable NAT and record source CIDR in ctx->nat_source_cidr.
 *
 * @param ctx           Network context (in: veth names; out: created/nat flags)
 * @param veth          Veth configuration (IPs, netmask, NAT flag)
 * @param child_pid     PID of clone'd child (for `ip link set ... netns <pid>`)
 * @param enable_debug  Enable [network] debug output
 * @return              0 on success, -1 on failure
 */
int setup_net(net_context_t *ctx, const veth_config_t *veth,
              pid_t child_pid, bool enable_debug);

/**
 * Configure network inside the container. Called by CHILD, after the
 * sync pipe unblocks (parent has moved veth_c into our netns).
 *
 * Steps: ip link set lo up, ip addr add to veth_c, ip link set veth_c up,
 * ip route add default via host_ip.
 *
 * @param ctx           Network context (read: veth_container name)
 * @param veth          Veth configuration (IPs, netmask)
 * @param enable_debug  Enable [child] debug output
 * @return              0 on success, -1 on failure
 */
int configure_container_net(const net_context_t *ctx,
                            const veth_config_t *veth,
                            bool enable_debug);

/**
 * Cleanup veth pair and NAT rules. Called after waitpid(). Self-contained:
 * uses ctx->nat_source_cidr (stored by setup_net) so it does not need the
 * original veth_config_t.
 *
 * @param ctx           Network context
 * @param enable_debug  Enable [network] debug output
 */
void cleanup_net(net_context_t *ctx, bool enable_debug);

#endif // NET_H
