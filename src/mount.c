#include "mount.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <limits.h>

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

    // Systemd sets / to shared propagation; pivot_root requires the
    // current root to not be shared.  Make everything private first.
    if(mount("", "/", NULL, MS_PRIVATE | MS_REC, NULL) < 0){
        perror("mount(MS_PRIVATE /)");
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

    if(syscall(SYS_pivot_root, ".", put_old) < 0){
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

/**
 * Mount /proc filesystem.
 */
int mount_proc(bool enable_debug){
    struct stat st;
    if(stat("/proc", &st) < 0 || !S_ISDIR(st.st_mode)){
        if(enable_debug){
            printf("[child] /proc doesn't exist or is not a directory, skipping mount\n");
        }
        return 0;
    }

    // MS_NOSUID | MS_NODEV | MS_NOEXEC: required for user namespace
    // (kernel rejects proc mounts less restrictive than the existing one)
    // and good hardening defaults regardless.
    if(mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0){
        perror("mount(proc)");
        return -1;
    }

    if(enable_debug){
        printf("[child] Mounted /proc\n");
    }

    return 0;
}