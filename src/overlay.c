#define _GNU_SOURCE
#include "overlay.h"
#include "mount.h"    // setup_rootfs(), mount_proc()
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <ftw.h>
#include <dirent.h>    // opendir/readdir for fd table audit 

#define STACK_SIZE (1024 * 1024)

// Phase 3 correction: extern char **environ is no longer needed here.
// The child no longer falls back to the host environment.
// build_container_env() in main.c constructs the environment, and it's
// passed through config->envp → child_args.envp → execve().

typedef struct {
    const char *program;
    char *const *argv;
    char *const *envp;
    bool enable_debug;
    const char *rootfs_path;  // Either merged_path (overlay) or raw rootfs
} child_args_t;

/**
 * Generate a 12-character hex container ID.
 */
static void generate_container_id(char *id){
    
    // This will generate randomness in the id
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    unsigned long hash = ts.tv_sec ^ ts.tv_nsec ^ getpid();
    snprintf(id, 13, "0x12lx", hash & 0xFFFFFFFFFFFF);
}

/**
 * Callback for nftw() to remove directory tree.
 */
static int remove_cb(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb;
    (void)typeflag;
    (void)ftwbuf;

    int rv = remove(path);
    if (rv) {
        perror(path);
    }
    return rv;
}

/**
 * Recursively remove a directory tree.
 */
static int remove_directory(const char *path) {
    return nftw(path, remove_cb, 64, FTW_DEPTH | FTW_PHYS);
}

/**
 * Initialize overlay paths in context.
 *
 * Generates a container ID, resolves absolute paths, and populates
 * the upper/work/merged path fields in ctx.
 *
 * @param ctx            Overlay context to populate
 * @param rootfs_path    Path to base image (lowerdir)
 * @param container_dir  Parent directory for overlay data (NULL = "./containers")
 * @param enable_debug   Print resolved paths
 * @return               0 on success, -1 on failure
 */
static int init_overlay_paths(overlay_context_t *ctx, const char *rootfs_path,
                              const char *container_dir, bool enable_debug) {
    // Generate container ID
    generate_container_id(ctx->container_id);

    // Resolve rootfs to absolute path
    if (!realpath(rootfs_path, ctx->lower_path)) {
        perror("realpath(rootfs)");
        return -1;
    }

    // Default container directory
    if (!container_dir) {
        container_dir = "./containers";
    }

    // Ensure container parent directory exists
    if (mkdir(container_dir, 0755) < 0 && errno != EEXIST) {
        perror("mkdir(container_dir)");
        return -1;
    }

    // Resolve container_dir to absolute path for overlay mount options
    char abs_container_dir[PATH_MAX];
    if (!realpath(container_dir, abs_container_dir)) {
        perror("realpath(container_dir)");
        return -1;
    }

    // Build paths for this container's overlay directories
    snprintf(ctx->container_base, PATH_MAX, "%s/%s",
             abs_container_dir, ctx->container_id);

    snprintf(ctx->upper_path, PATH_MAX, "%s/upper", ctx->container_base);
    snprintf(ctx->work_path, PATH_MAX, "%s/work", ctx->container_base);
    snprintf(ctx->merged_path, PATH_MAX, "%s/merged", ctx->container_base);

    if (enable_debug) {
        printf("[overlay] Container ID: %s\n", ctx->container_id);
        printf("[overlay] Lower: %s\n", ctx->lower_path);
        printf("[overlay] Upper: %s\n", ctx->upper_path);
        printf("[overlay] Work:  %s\n", ctx->work_path);
        printf("[overlay] Merged: %s\n", ctx->merged_path);
    }

    return 0;
}

/**
 * Create the overlay directory structure.
 *
 * Creates container_base/, upper/, work/, and merged/ directories.
 * Cleans up partial creation on failure.
 *
 * @param ctx  Overlay context with paths already initialized
 * @return     0 on success, -1 on failure
 */
static int create_overlay_dirs(overlay_context_t *ctx) {
    if (mkdir(ctx->container_base, 0755) < 0) {
        perror("mkdir(container_base)");
        return -1;
    }

    if (mkdir(ctx->upper_path, 0755) < 0) {
        perror("mkdir(upper)");
        goto cleanup_base;
    }

    if (mkdir(ctx->work_path, 0755) < 0) {
        perror("mkdir(work)");
        goto cleanup_upper;
    }

    if (mkdir(ctx->merged_path, 0755) < 0) {
        perror("mkdir(merged)");
        goto cleanup_work;
    }

    return 0;

cleanup_work:
    rmdir(ctx->work_path);
cleanup_upper:
    rmdir(ctx->upper_path);
cleanup_base:
    rmdir(ctx->container_base);
    return -1;
}

/**
 * Mount the overlay filesystem.
 *
 * Builds the mount options string and calls mount(2) with
 * MS_NODEV | MS_NOSUID (§3.6).
 *
 * @param ctx           Overlay context with paths initialized and dirs created
 * @param enable_debug  Print mount options
 * @return              0 on success, -1 on failure
 */
static int mount_overlay(overlay_context_t *ctx, bool enable_debug) {
    // Build mount options string
    char mount_opts[PATH_MAX * 4];
    snprintf(mount_opts, sizeof(mount_opts),
             "lowerdir=%s,upperdir=%s,workdir=%s",
             ctx->lower_path, ctx->upper_path, ctx->work_path);

    if (enable_debug) {
        printf("[overlay] Mount options: %s\n", mount_opts);
    }

    // Mount overlay — MS_NODEV | MS_NOSUID blocks device nodes and
    // setuid escalation inside the container (see §3.6)
    if (mount("overlay", ctx->merged_path, "overlay",
              MS_NODEV | MS_NOSUID, mount_opts) < 0) {
        perror("mount(overlay)");
        return -1;
    }

    ctx->is_mounted = true;

    if (enable_debug) {
        printf("[overlay] Overlay mounted at %s\n", ctx->merged_path);
    }

    return 0;
}

/**
 * Setup overlay filesystem.
 *
 * Public wrapper that initializes paths, creates directories, and mounts
 * overlayfs. On failure, partially created state is cleaned up by each
 * sub-function independently.
 */
int setup_overlay(overlay_context_t *ctx, const char *rootfs_path,
                  const char *container_dir, bool enable_debug) {
    if (!ctx || !rootfs_path) {
        fprintf(stderr, "setup_overlay: invalid arguments\n");
        return -1;
    }

    if (init_overlay_paths(ctx, rootfs_path, container_dir, enable_debug) < 0) {
        return -1;
    }

    if (create_overlay_dirs(ctx) < 0) {
        return -1;
    }

    if (mount_overlay(ctx, enable_debug) < 0) {
        rmdir(ctx->merged_path);
        rmdir(ctx->work_path);
        rmdir(ctx->upper_path);
        rmdir(ctx->container_base);
        return -1;
    }

    return 0;
}