#include "overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <ftw.h>

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
 * Copies in the canonical container ID (state.h is the sole generator
 * — see decisions.md "Canonical container-ID source"), resolves
 * absolute paths, and populates the upper/work/merged path fields in
 * ctx.
 *
 * @param ctx            Overlay context to populate
 * @param rootfs_path    Path to base image (lowerdir)
 * @param container_dir  Parent directory for overlay data (NULL = "./containers")
 * @param container_id   Canonical 12-hex container ID; must be non-NULL and non-empty
 * @param enable_debug   Print resolved paths
 * @return               0 on success, -1 on failure
 */
static int init_overlay_paths(overlay_context_t *ctx, const char *rootfs_path,
                              const char *container_dir,
                              const char *container_id, bool enable_debug) {
    if (!container_id || container_id[0] == '\0') {
        fprintf(stderr, "init_overlay_paths: container_id is required\n");
        return -1;
    }
    // Copy in the canonical container ID.  State.h is the sole generator.
    strncpy(ctx->container_id, container_id, sizeof(ctx->container_id) - 1);
    ctx->container_id[sizeof(ctx->container_id) - 1] = '\0';

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
    // Each snprintf return value is checked against PATH_MAX to detect
    // truncation — a silently truncated path would cause mkdir/mount to
    // operate on the wrong location.
    if (snprintf(ctx->container_base, PATH_MAX, "%s/%s",
                 abs_container_dir, ctx->container_id) >= PATH_MAX) {
        fprintf(stderr, "init_overlay_paths: container_base path truncated\n");
        return -1;
    }

    if (snprintf(ctx->upper_path, PATH_MAX, "%s/upper", ctx->container_base) >= PATH_MAX) {
        fprintf(stderr, "init_overlay_paths: upper_path truncated\n");
        return -1;
    }

    if (snprintf(ctx->work_path, PATH_MAX, "%s/work", ctx->container_base) >= PATH_MAX) {
        fprintf(stderr, "init_overlay_paths: work_path truncated\n");
        return -1;
    }

    if (snprintf(ctx->merged_path, PATH_MAX, "%s/merged", ctx->container_base) >= PATH_MAX) {
        fprintf(stderr, "init_overlay_paths: merged_path truncated\n");
        return -1;
    }

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
                  const char *container_dir, const char *container_id,
                  bool enable_debug) {
    if (!ctx || !rootfs_path) {
        fprintf(stderr, "setup_overlay: invalid arguments\n");
        return -1;
    }
    if (!container_id || container_id[0] == '\0') {
        fprintf(stderr, "setup_overlay: container_id is required\n");
        return -1;
    }

    if (init_overlay_paths(ctx, rootfs_path, container_dir,
                           container_id, enable_debug) < 0) {
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

/**
 * Teardown overlay filesystem.
 * Unmounts overlay and removes all container directories.
 */
int teardown_overlay(overlay_context_t *ctx, bool enable_debug){
    
    if(!ctx){
        return -1;
    }

    int ret = 0;

    // Unmount the overlay
    if(ctx->is_mounted){
        if(enable_debug){
            printf("[overlay] Unmounting: %s\n", ctx->merged_path);
        }

        if(umount2(ctx->merged_path, MNT_DETACH) < 0){
            perror("umount2(overlay)");
            ret = -1;
        }else{
            ctx->is_mounted = false;
        }
    }

    // Remove upper directory tree (contains container's writes)
    if(ctx->upper_path[0]){
        if(enable_debug){
            printf("[overlay] Removing upper layer: %s\n", ctx->upper_path);
        }
        remove_directory(ctx->upper_path);
    }

    // Remove work directory tree (kernel bookkeeping)
    if(ctx->work_path[0]){
        if(enable_debug){
            printf("[overlay] Removing work dir: %s\n", ctx->work_path);
        }
        remove_directory(ctx->work_path);
    }

    // Remove merged directory (mount point)
    if(ctx->merged_path[0]){
        if(enable_debug){
            printf("[overlay] Removing merged dir: %s\n", ctx->merged_path);
        }
        rmdir(ctx->merged_path);
    }

    // Remove container base directory
    if(ctx->container_base[0]){
        if(enable_debug){
            printf("[overlay] Removing container base: %s\n", ctx->container_base);
        }
        rmdir(ctx->container_base);
    }

    if (enable_debug) {
        printf("[overlay] Cleanup complete\n");
    }

    return ret;
}

