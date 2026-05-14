// Note: _GNU_SOURCE is provided by the Makefile via -D_GNU_SOURCE.
// Do NOT redefine it here (Error #8 from decisions.md).
#include "cgroup.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>

#define CGROUP_ROOT "/sys/fs/cgroup"
#define CGROUP_PREFIX "minicontainer"

/**
 * Generate unique cgroup name using timestamp + nanoseconds.
 */
static void generate_cgroup_name(char *name, size_t size) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(name, size, "%s_%ld_%ld", CGROUP_PREFIX, ts.tv_sec, ts.tv_nsec);
}

/**
 * Write string to a cgroup file.
 * Returns 0 on success, -1 on failure.
 */
static int write_cgroup_file(const char *path, const char *content,
                             bool enable_debug) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        if (enable_debug) {
            fprintf(stderr, "[cgroup] Failed to open %s: %s\n",
                    path, strerror(errno));
        }
        return -1;
    }

    ssize_t len = strlen(content);
    if (write(fd, content, len) != len) {
        if (enable_debug) {
            fprintf(stderr, "[cgroup] Failed to write to %s: %s\n",
                    path, strerror(errno));
        }
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

/**
 * Create and setup cgroup.
 *
 * Steps:
 * 1. Generate unique name
 * 2. Create directory in /sys/fs/cgroup/
 * 3. Enable controllers in parent (may fail if already enabled — that's fine)
 * 4. Write limits to memory.max, cpu.max, pids.max
 */
int setup_cgroup(cgroup_context_t *ctx, const cgroup_limits_t *limits,
                 bool enable_debug) {
    // Generate unique name
    generate_cgroup_name(ctx->cgroup_name, sizeof(ctx->cgroup_name));
    snprintf(ctx->cgroup_path, sizeof(ctx->cgroup_path),
             "%s/%s", CGROUP_ROOT, ctx->cgroup_name);

    if (enable_debug) {
        printf("[cgroup] Creating cgroup: %s\n", ctx->cgroup_path);
    }

    // Create cgroup directory
    if (mkdir(ctx->cgroup_path, 0755) < 0) {
        perror("mkdir(cgroup)");
        return -1;
    }
    ctx->created = true;

    // Enable controllers in root cgroup (may fail if already enabled)
    char subtree_path[512];
    snprintf(subtree_path, sizeof(subtree_path),
             "%s/cgroup.subtree_control", CGROUP_ROOT);
    write_cgroup_file(subtree_path, "+cpu +memory +pids", enable_debug);
    // Ignore errors — controllers might already be enabled

    // Set memory limit
    if (limits->memory_limit > 0) {
        char path[512];
        char content[64];

        snprintf(path, sizeof(path), "%s/memory.max", ctx->cgroup_path);
        snprintf(content, sizeof(content), "%zu", limits->memory_limit);

        if (write_cgroup_file(path, content, enable_debug) < 0) {
            fprintf(stderr, "[cgroup] Failed to set memory limit\n");
            return -1;
        }

        if (enable_debug) {
            printf("[cgroup] Memory limit: %zu bytes\n", limits->memory_limit);
        }
    }

    // Set CPU limit
    if (limits->cpu_quota > 0) {
        char path[512];
        char content[64];
        long period = limits->cpu_period > 0 ? limits->cpu_period : 100000;

        snprintf(path, sizeof(path), "%s/cpu.max", ctx->cgroup_path);
        snprintf(content, sizeof(content), "%ld %ld",
                 limits->cpu_quota, period);

        if (write_cgroup_file(path, content, enable_debug) < 0) {
            fprintf(stderr, "[cgroup] Failed to set CPU limit\n");
            return -1;
        }

        if (enable_debug) {
            printf("[cgroup] CPU limit: %ld/%ld µs\n",
                   limits->cpu_quota, period);
        }
    }

    // Set PID limit
    if (limits->pid_limit > 0) {
        char path[512];
        char content[64];

        snprintf(path, sizeof(path), "%s/pids.max", ctx->cgroup_path);
        snprintf(content, sizeof(content), "%zu", limits->pid_limit);

        if (write_cgroup_file(path, content, enable_debug) < 0) {
            fprintf(stderr, "[cgroup] Failed to set PID limit\n");
            return -1;
        }

        if (enable_debug) {
            printf("[cgroup] PID limit: %zu\n", limits->pid_limit);
        }
    }

    return 0;
}

/**
 * Add PID to cgroup.
 *
 * Writes the PID to cgroup.procs. All child processes automatically
 * inherit the cgroup membership.
 */
int add_pid_to_cgroup(const cgroup_context_t *ctx, pid_t pid,
                      bool enable_debug) {
    char path[512];
    char content[32];

    snprintf(path, sizeof(path), "%s/cgroup.procs", ctx->cgroup_path);
    snprintf(content, sizeof(content), "%d", pid);

    if (write_cgroup_file(path, content, enable_debug) < 0) {
        fprintf(stderr, "[cgroup] Failed to add PID %d to cgroup\n", pid);
        return -1;
    }

    if (enable_debug) {
        printf("[cgroup] Added PID %d to cgroup\n", pid);
    }

    return 0;
}

/**
 * Remove cgroup directory.
 *
 * Only succeeds when the cgroup is empty (no processes). This is why
 * we call it after waitpid() — the child has already exited.
 */
void remove_cgroup(cgroup_context_t *ctx, bool enable_debug) {
    if (!ctx->created) {
        return;
    }

    if (enable_debug) {
        printf("[cgroup] Removing cgroup: %s\n", ctx->cgroup_path);
    }

    if (rmdir(ctx->cgroup_path) < 0) {
        if (enable_debug) {
            fprintf(stderr, "[cgroup] Failed to remove cgroup: %s\n",
                    strerror(errno));
        }
    }

    ctx->created = false;
}

