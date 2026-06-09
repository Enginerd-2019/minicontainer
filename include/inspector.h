#ifndef CONTAINER_INSPECTOR_H
#define CONTAINER_INSPECTOR_H

#include <stdbool.h>
#include <sys/types.h>

/* libprocfs unified API */
#include "procfs.h"
#include "cgroups.h"

/*
 * Container introspection bundle. Combines libprocfs /proc data with
 * cgroup stats for a complete runtime snapshot.
 *
 * Heap-allocated members (fds, threads, sockets) must be released via
 * inspect_info_free(). It is safe to call inspect_info_free() on a
 * zeroed or partially-populated struct.
 */
typedef struct {
    char container_id[13];           /* 12 hex + NUL, matches Phase 7b ID format */
    pid_t pid;

    /* /proc/<pid> data via libprocfs */
    proc_info_t    proc_info;
    fd_entry_t    *fds;
    int            fd_count;
    thread_info_t *threads;
    int            thread_count;
    socket_info_t *sockets;
    int            socket_count;

    /* /sys/fs/cgroup/<path> data via libprocfs */
    cgroup_stats_t cgroup_stats;
} container_inspect_info_t;

/*
 * Populate info for a running container.
 *
 * container_id  12-hex-char ID from Phase 7b state files
 * info          Output struct
 *
 * read_proc_status() failure is fatal (returns -1; container is gone).
 * All other libprocfs failures (FD enumeration, sockets, etc.) are
 * non-fatal — corresponding count is set to 0 and the rest of the
 * inspection continues.
 *
 * Returns 0 on success, -1 on failure.
 */
int inspect_container(const char *container_id,
                      container_inspect_info_t *info);

/* Free heap members; safe on zeroed/partial structs. */
void inspect_info_free(container_inspect_info_t *info);

/*
 * CLI front-ends (called from cli.c's cmd_inspect / cmd_stats / etc.).
 * Each prints to stdout, returns 0 on success, -1 on container-not-found.
 */
int show_container_inspect(const char *container_id);
int show_container_stats(const char *container_id, bool continuous);
int show_container_top(const char *container_id);
int show_container_netstat(const char *container_id, bool verbose);

#endif /* CONTAINER_INSPECTOR_H */
