#include "uts.h"
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>         // open(), O_WRONLY for /proc/<pid>/{uid_map,gid_map,setgroups}

/**
 * Setup hostname inside UTS namespace.
 */
int setup_uts(const char *hostname, bool enable_debug) {
    if (hostname) {
        if (sethostname(hostname, strlen(hostname)) < 0) {
            perror("sethostname");
            return -1;
        }

        if (enable_debug) {
            printf("[child] Set hostname: %s\n", hostname);
        }
    }

    return 0;
}

/**
 * Setup UID/GID mapping for user namespace.
 *
 * Called from the PARENT process after clone(). Writes to
 * /proc/<child_pid>/setgroups, uid_map, and gid_map.
 */
int setup_user_namespace_mapping(pid_t child_pid, const user_ns_mapping_t *config) {
    char path[256];
    char mapping[256];
    int fd;

    // Step 1: Disable setgroups (required before gid_map for unprivileged users)
    snprintf(path, sizeof(path), "/proc/%d/setgroups", child_pid);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("open(setgroups)");
        return -1;
    }
    if (write(fd, "deny", 4) < 0) {
        perror("write(setgroups)");
        close(fd);
        return -1;
    }
    close(fd);

    if (config->enable_debug) {
        printf("[parent] Disabled setgroups for PID %d\n", child_pid);
    }

    // Step 2: Write UID map
    snprintf(path, sizeof(path), "/proc/%d/uid_map", child_pid);
    snprintf(mapping, sizeof(mapping), "%u %u %zu",
             config->uid_map_inside, config->uid_map_outside,
             config->uid_map_range);

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("open(uid_map)");
        return -1;
    }
    if (write(fd, mapping, strlen(mapping)) < 0) {
        perror("write(uid_map)");
        close(fd);
        return -1;
    }
    close(fd);

    if (config->enable_debug) {
        printf("[parent] UID map: %s\n", mapping);
    }

    // Step 3: Write GID map
    snprintf(path, sizeof(path), "/proc/%d/gid_map", child_pid);
    snprintf(mapping, sizeof(mapping), "%u %u %zu",
             config->gid_map_inside, config->gid_map_outside,
             config->gid_map_range);

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("open(gid_map)");
        return -1;
    }
    if (write(fd, mapping, strlen(mapping)) < 0) {
        perror("write(gid_map)");
        close(fd);
        return -1;
    }
    close(fd);

    if (config->enable_debug) {
        printf("[parent] GID map: %s\n", mapping);
    }

    return 0;
}

