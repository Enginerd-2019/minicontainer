#ifndef NET_H
#define NET_H

// NOTE: _GNU_SOURCE is provided by the Makefile via -D_GNU_SOURCE.
// Do NOT redefine it here (Error #8 from decisions.md).
#include <sched.h>
#include <stdbool.h>
#include <sys/types.h>
#include <arpa/inet.h>   // INET_ADDRSTRLEN
#include <net/if.h>      // IFNAMSIZ

/**
 * Veth-specific configuration. Embedded inside container_config_t
 * as the `veth` field.
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
 * Locate the `ip` binary in one of three canonical locations:
 *   /sbin/ip, /usr/sbin/ip, /bin/ip (in that order).
 *
 * Phase 7a: promoted from `static` in net.c to a public function so
 * core.c's early-failure check (before clone) can call it without
 * having to duplicate the path-search logic.
 *
 * Returns a pointer to a static string (do not free) or NULL if no
 * `ip` binary is found.
 */
const char *find_ip_binary(void);

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
