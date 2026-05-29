#ifndef CLEANUP_H
#define CLEANUP_H

#include <stdbool.h>

/**
 * Walk every Phase 7b runtime location for stale debris (state
 * directories whose pidfile points at a dead PID, orphan cgroup
 * directories, orphan veth_h_* interfaces, stale iptables MASQUERADE
 * rules pointing at gone containers, stale overlay directories).
 * Remove anything whose owning container is no longer alive.
 *
 * Phase 7b step 13 implements the body.  Until then the function is
 * an ENOSYS stub so cmd_cleanup's wiring in cli.c can build without
 * pulling step-13 work forward.
 *
 * @param enable_debug  Verbose progress logging to stderr
 * @param dry_run       If true, report findings but make no changes
 * @return              Count of items removed (or that would be
 *                      removed in dry-run mode), or -1 on error
 */
int cleanup_stale_resources(bool enable_debug, bool dry_run);

#endif // CLEANUP_H
