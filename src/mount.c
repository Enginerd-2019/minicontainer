#define _GNU_SOURCE
#include "mount.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <limits.h>

#define STACK_SIZE (1024 * 1024)

extern char **environ;

typedef struct{
    const char *program;
    char *const *argv;
    char *const *envp;
    bool enable_debug;
    const char *rootfd_path;
} child_args_t;

/**
 * Setup rootfs using pivot_root.
 *
 * Must be called from child_func() after clone(CLONE_NEWNS). On failure the
 * child exits, and the kernel tears down the mount namespace — no explicit
 * error-path cleanup is needed here.
 */
int setup_rootfs(const char *rootfs_path, bool enable_debug){

    if(!rootfs_path){
        return 0;
    }

    if(enable_debug){
        printf("[child] Setting up rootfs: %s\n", rootfs_path);
    }

    char abs_path[PATH_MAX];
    if(!realpath(rootfs_path, abs_path)){
        perror("realpath");
        return -1;
    }

    // Bind mount to itself so pivot_root sees it as a mount point
    if(mount(abs_path, abs_path, NULL, MS_BIND | MS_REC, NULL) < 0){
        perror("mount(MS_BIND)");
        return -1;
    }

    // Propagation flags must be a separate mount(2) call per the man page;
    // MS_PRIVATE prevents mount events leaking to the parent namespace
    if(mount("", abs_path, NULL, MS_PRIVATE | MS_REC, NULL) < 0){
        perror("mount(MS_PRIVATE)");
        return -1;
    }

    if (enable_debug) {
        printf("[child] Bind mounted %s\n", abs_path);
    }

    if(chdir(abs_path) < 0){
        perror("chdir(new_root)");
        return -1;
    }

    const char *put_old = "old_root";
    if(mkdir(put_old, 0700) < 0 && errno != EEXIST){
        perror("mkdir(old_root)");
        return -1;
    }

    if(pivot_root(".", put_old) < 0){
        perror("pivot_root");
        return -1;
    }

    if (enable_debug) {
        printf("[child] pivot_root successful\n");
    }

    if (chdir("/") < 0) {
        perror("chdir(/)");
        return -1;
    }

    // MNT_DETACH: lazy unmount so in-flight references drain gracefully
    if(umount2("/old_root", MNT_DETACH) < 0){
        perror("umount2(old_root)");
        return -1;
    }

    if(enable_debug){
        printf("[child] Unmounted old root\n");
    }

    // Best-effort cleanup; isolation is already complete at this point
    rmdir("/old_root");

    return 0;
}