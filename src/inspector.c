/*
 * inspector.c — container runtime introspection
 *
 * Uses libprocfs (procfs.h + cgroups.h) for all /proc and cgroup
 * reading. inspector.c knows how to:
 *  - Look up a container's PID via Phase 7b state files
 *  - Resolve the container's cgroup path
 *  - Bundle libprocfs results into container_inspect_info_t
 *  - Pretty-print for the inspect / stats / top / netstat subcommands
 */

#include "inspector.h"
#include "state.h"               /* Phase 7b: state_load, state_dir_path */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

/*
 * Find the container's PID. Uses Phase 7b's state_load() — the same
 * function cmd_stop / cmd_exec rely on. Returns -1 if not found.
 */
static pid_t lookup_container_pid(const char *container_id)
{
    container_state_t st = {0};
    if (state_load(container_id, &st) < 0) {
        return -1;
    }
    return st.pid;
}

/*
 * Resolve the container's cgroup absolute path.
 *
 * Two strategies (in order):
 *   1) Read state.json's cgroup_path field (populated by Phase 7b's
 *      cmd_run / cmd_start when enable_cgroup is true).
 *   2) Fall back to libprocfs's get_pid_cgroup(pid) reading
 *      /proc/<pid>/cgroup directly.
 *
 * Returns 0 on success (path written to buf), -1 if both fail.
 */
static int resolve_cgroup_path(const char *container_id, pid_t pid,
                               char *buf, size_t buflen)
{
    container_state_t st = {0};
    if (state_load(container_id, &st) == 0 && st.cgroup_path[0] != '\0') {
        size_t n = strlen(st.cgroup_path);
        if (n + 1 > buflen) {
            return -1;
        }
        memcpy(buf, st.cgroup_path, n + 1);
        return 0;
    }

    /* Fallback to libprocfs */
    return get_pid_cgroup(pid, buf, buflen);
}

int inspect_container(const char *container_id,
                      container_inspect_info_t *info)
{
    if (container_id == NULL || info == NULL) {
        return -1;
    }

    memset(info, 0, sizeof(*info));
    strncpy(info->container_id, container_id,
            sizeof(info->container_id) - 1);

    pid_t pid = lookup_container_pid(container_id);
    if (pid < 0) {
        fprintf(stderr, "Container %s not found\n", container_id);
        return -1;
    }
    info->pid = pid;

    /* /proc/<pid>/status — fatal if it fails (container is gone) */
    if (read_proc_status(pid, &info->proc_info) < 0) {
        fprintf(stderr, "Failed to read /proc/%d/status\n", (int)pid);
        return -1;
    }

    /* The following three are best-effort. count=0 on failure. */
    if (enumerate_fds(pid, &info->fds, &info->fd_count) < 0) {
        info->fds = NULL;
        info->fd_count = 0;
    }
    if (enumerate_threads(pid, &info->threads, &info->thread_count) < 0) {
        info->threads = NULL;
        info->thread_count = 0;
    }
    if (find_process_sockets(pid, &info->sockets, &info->socket_count) < 0) {
        info->sockets = NULL;
        info->socket_count = 0;
    }

    /* Cgroup stats — best-effort; missing controllers leave fields zero. */
    char cgroup_path[PATH_MAX];
    if (resolve_cgroup_path(container_id, pid,
                            cgroup_path, sizeof(cgroup_path)) == 0) {
        read_cgroup_stats(cgroup_path, &info->cgroup_stats);
    }

    return 0;
}

void inspect_info_free(container_inspect_info_t *info)
{
    if (info == NULL) return;
    fd_entries_free(info->fds);
    thread_info_free(info->threads);
    socket_list_free(info->sockets);
    memset(info, 0, sizeof(*info));
}

/* ===== CLI front-ends ===== */

int show_container_inspect(const char *container_id)
{
    container_inspect_info_t info;
    if (inspect_container(container_id, &info) < 0) {
        return -1;
    }

    printf("Container:  %s\n", info.container_id);
    printf("PID:        %d\n", (int)info.pid);
    printf("Process:    %s\n", info.proc_info.name);
    printf("State:      %s\n", state_to_string(info.proc_info.state));
    printf("UID/GID:    %d/%d (effective: %d/%d)\n",
           (int)info.proc_info.uid_real, (int)info.proc_info.gid_real,
           (int)info.proc_info.uid_effective, (int)info.proc_info.gid_effective);
    printf("Threads:    %d\n", info.thread_count);
    printf("FDs:        %d\n", info.fd_count);
    printf("Sockets:    %d\n", info.socket_count);
    printf("\n");
    printf("Memory (process /proc/status):\n");
    printf("  VmRSS:    %lu KB\n", info.proc_info.vm_rss_kb);
    printf("  VmSize:   %lu KB\n", info.proc_info.vm_size_kb);
    printf("\n");
    printf("Memory (cgroup):\n");
    printf("  Current:  %lu bytes (%.2f MB)\n",
           (unsigned long)info.cgroup_stats.memory_current_bytes,
           info.cgroup_stats.memory_current_bytes / 1048576.0);
    printf("  Peak:     %lu bytes (%.2f MB)\n",
           (unsigned long)info.cgroup_stats.memory_peak_bytes,
           info.cgroup_stats.memory_peak_bytes / 1048576.0);
    if (info.cgroup_stats.memory_max_bytes != UINT64_MAX &&
        info.cgroup_stats.memory_max_bytes > 0) {
        printf("  Limit:    %lu bytes (%.2f MB)\n",
               (unsigned long)info.cgroup_stats.memory_max_bytes,
               info.cgroup_stats.memory_max_bytes / 1048576.0);
    } else {
        printf("  Limit:    (unlimited)\n");
    }
    printf("\n");
    printf("CPU (cgroup, cumulative):\n");
    printf("  Total:    %.3f sec\n", info.cgroup_stats.cpu_usage_usec  / 1e6);
    printf("  User:     %.3f sec\n", info.cgroup_stats.cpu_user_usec   / 1e6);
    printf("  System:   %.3f sec\n", info.cgroup_stats.cpu_system_usec / 1e6);
    printf("\n");
    printf("PIDs (cgroup):\n");
    printf("  Current:  %d\n", info.cgroup_stats.pids_current);
    if (info.cgroup_stats.pids_max >= 0) {
        printf("  Limit:    %d\n", info.cgroup_stats.pids_max);
    } else {
        printf("  Limit:    (unlimited)\n");
    }

    inspect_info_free(&info);
    return 0;
}

int show_container_stats(const char *container_id, bool continuous)
{
    do {
        container_inspect_info_t info;
        if (inspect_container(container_id, &info) < 0) {
            return -1;
        }

        if (continuous) {
            printf("\033[2J\033[H");   /* ANSI clear + home cursor */
        }

        printf("Container: %s (PID %d)\n", info.container_id, (int)info.pid);
        printf("Process:   %s [%s]\n",
               info.proc_info.name,
               state_to_string(info.proc_info.state));
        printf("\n");
        printf("MEMORY                                CPU\n");
        printf("  Current:  %10lu KB              Total: %10.3f sec\n",
               (unsigned long)(info.cgroup_stats.memory_current_bytes / 1024),
               info.cgroup_stats.cpu_usage_usec  / 1e6);
        printf("  Peak:     %10lu KB              User:  %10.3f sec\n",
               (unsigned long)(info.cgroup_stats.memory_peak_bytes / 1024),
               info.cgroup_stats.cpu_user_usec   / 1e6);
        if (info.cgroup_stats.memory_max_bytes != UINT64_MAX &&
            info.cgroup_stats.memory_max_bytes > 0) {
            printf("  Limit:    %10lu KB              Sys:   %10.3f sec\n",
                   (unsigned long)(info.cgroup_stats.memory_max_bytes / 1024),
                   info.cgroup_stats.cpu_system_usec / 1e6);
        } else {
            printf("  Limit:           unlimited              Sys:   %10.3f sec\n",
                   info.cgroup_stats.cpu_system_usec / 1e6);
        }
        printf("\n");
        printf("PIDs:    %3d / %s        Threads: %d   FDs: %d   Sockets: %d\n",
               info.cgroup_stats.pids_current,
               info.cgroup_stats.pids_max < 0 ? "max" : "limited",
               info.thread_count, info.fd_count, info.socket_count);

        inspect_info_free(&info);

        if (continuous) {
            sleep(1);
        }
    } while (continuous);

    return 0;
}

int show_container_top(const char *container_id)
{
    pid_t pid = lookup_container_pid(container_id);
    if (pid < 0) {
        fprintf(stderr, "Container %s not found\n", container_id);
        return -1;
    }

    /*
     * Use nsenter to enter the container's PID and mount namespaces,
     * then run ps. Inside the namespaces, ps sees the container's
     * own PID space (PID 1 = container init).
     *
     * Requires nsenter on the host (util-linux package — present on
     * essentially every Linux system).
     */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "nsenter --target %d --pid --mount ps aux", (int)pid);
    return system(cmd);
}

int show_container_netstat(const char *container_id, bool verbose)
{
    container_inspect_info_t info;
    if (inspect_container(container_id, &info) < 0) {
        return -1;
    }

    printf("Container:           %s (PID %d)\n",
           info.container_id, (int)info.pid);
    printf("Network connections: %d\n", info.socket_count);

    if (verbose && info.socket_count > 0) {
        printf("\n");
        printf("Proto  Local Address          Remote Address         State\n");
        printf("-----  ---------------------  ---------------------  -----------\n");

        for (int i = 0; i < info.socket_count; i++) {
            char local[32], remote[32];
            format_ip_port(info.sockets[i].local_addr,
                           info.sockets[i].local_port,
                           local, sizeof(local));
            format_ip_port(info.sockets[i].remote_addr,
                           info.sockets[i].remote_port,
                           remote, sizeof(remote));

            printf("%-5s  %-21s  %-21s  %s\n",
                   info.sockets[i].is_tcp ? "TCP" : "UDP",
                   local, remote,
                   info.sockets[i].is_tcp
                       ? tcp_state_to_string(info.sockets[i].state)
                       : "-");
        }
    }

    inspect_info_free(&info);
    return 0;
}
