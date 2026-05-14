#ifndef UTS_H
#define UTS_H

#include <sched.h>
#include <stdbool.h>
#include <sys/types.h>
#include <limits.h>

typedef struct {
    uid_t uid_map_inside;
    uid_t uid_map_outside;
    size_t uid_map_range;
    gid_t gid_map_inside;
    gid_t gid_map_outside;
    size_t gid_map_range;
    bool  enable_debug;
} user_ns_mapping_t;

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
int setup_user_namespace_mapping(pid_t child_pid, const user_ns_mapping_t *config);

#endif