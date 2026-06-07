#include "mount.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <fcntl.h>
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
int setup_rootfs(const char *rootfs_path, const bind_mount_t *mounts,
                 int mount_count, bool enable_debug){

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

    /* Apply user bind mounts onto the new root BEFORE pivot_root.  The
     * host source paths are still reachable in this mount namespace
     * here; after the pivot + old-root detach below they are not.  Each
     * target is composed under abs_path so the bind lands inside the
     * future container root and the pivot carries it along.  (Error #22) */
    for (int i = 0; i < mount_count; i++) {
        if (bind_mount_apply(&mounts[i], abs_path, enable_debug) < 0) {
            fprintf(stderr, "[child] Failed to apply bind mount %s -> %s\n",
                    mounts[i].host_path, mounts[i].container_path);
            return -1;
        }
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

int bind_mount_apply(const bind_mount_t *m, const char *root_prefix,
                     bool enable_debug) {
    if (!m || !m->host_path[0] || !m->container_path[0]) {
        fprintf(stderr, "[bind] invalid bind mount spec\n");
        return -1;
    }

    /* Compose the on-disk target.  With a root_prefix (the runtime
     * path: bind applied before pivot_root) the target must resolve
     * under the new root rather than against the still-current host
     * root.  Without one (the unit tests) container_path is used
     * verbatim.  (decisions.md Error #22) */
    char target[PATH_MAX];
    int n;
    if (root_prefix && root_prefix[0]) {
        n = snprintf(target, sizeof(target), "%s%s",
                     root_prefix, m->container_path);
    } else {
        n = snprintf(target, sizeof(target), "%s", m->container_path);
    }
    if (n < 0 || (size_t)n >= sizeof(target)) {
        fprintf(stderr, "[bind] target path too long for %s\n",
                m->container_path);
        return -1;
    }

    /* Determine whether the host source is a directory or a file.
     * For files, we touch the target so mount(2) has a target to
     * overlay onto. For directories, we mkdir -p. */
    struct stat st;
    if (stat(m->host_path, &st) < 0) {
        fprintf(stderr, "[bind] host_path %s: %s\n",
                m->host_path, strerror(errno));
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        /* mkdir -p the target. */
        mkdir(target, 0755);  /* ignore EEXIST */
    } else {
        /* Ensure parent dir exists, then touch the file. */
        int fd = open(target, O_WRONLY | O_CREAT, 0644);
        if (fd >= 0) close(fd);
    }

    if (enable_debug) {
        printf("[bind] %s -> %s%s\n",
               m->host_path, target,
               m->readonly ? " (ro)" : "");
    }

    if (mount(m->host_path, target, NULL,
              MS_BIND, NULL) < 0) {
        fprintf(stderr, "[bind] mount(MS_BIND) %s -> %s: %s\n",
                m->host_path, target, strerror(errno));
        return -1;
    }

    /* Read-only: kernel ignores MS_RDONLY on the initial MS_BIND.
     * Need a separate remount to actually enforce. */
    if (m->readonly) {
        if (mount(NULL, target, NULL,
                  MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0) {
            fprintf(stderr,
                "[bind] remount-ro %s: %s\n",
                target, strerror(errno));
            return -1;
        }
    }

    return 0;
}

/**
 * Mount a private devpts instance on /dev/pts and redirect /dev/ptmx
 * to the new instance's master multiplexer.
 *
 * Why two steps: posix_openpt(2) opens "/dev/ptmx". Inside the
 * rootfs that's either a stale character-device inode shipped with
 * the rootfs (disconnected from the devpts driver) or non-existent.
 * Mounting devpts with newinstance creates a private instance whose
 * own master multiplexer lives at /dev/pts/ptmx. The second step is
 * a symlink from /dev/ptmx to /dev/pts/ptmx so posix_openpt finds
 * the right device. See Documentation/filesystems/devpts.txt.
 */
int mount_devpts(bool enable_debug, bool user_namespace_active) {
    /* Ensure /dev and /dev/pts both exist.  The build_rootfs.sh
     * minimal rootfs omits /dev entirely; Ubuntu and Alpine ship it
     * empty.  Both mkdir calls are idempotent — EEXIST is fine. */
    if (mkdir("/dev", 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "[devpts] mkdir /dev: %s\n", strerror(errno));
        return -1;
    }
    if (mkdir("/dev/pts", 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "[devpts] mkdir /dev/pts: %s\n", strerror(errno));
        return -1;
    }

    /* The four-character options string is the magic.  newinstance
     * gives this mount its own private master/slave numbering.
     * ptmxmode=0666 lets non-root inside the container open the
     * master.  mode=0620 + gid=5 matches the Debian/Ubuntu host
     * convention for /dev/pts/<N> ownership (gid 5 = "tty"). */
    const char *opts = "newinstance,ptmxmode=0666,mode=0620,gid=5";
    if (mount("devpts", "/dev/pts", "devpts", 0, opts) < 0) {
        if (user_namespace_active) {
            fprintf(stderr,
                "[devpts] Warning: devpts mount denied "
                "(user namespace restriction) — pty allocation "
                "inside the container will fail\n");
            return 0;  /* graceful degradation, same as mount_proc */
        }
        fprintf(stderr, "[devpts] mount devpts on /dev/pts: %s\n",
                strerror(errno));
        return -1;
    }

    /* /dev/ptmx → /dev/pts/ptmx so glibc's posix_openpt(2) finds the
     * newinstance master.  Two ways to wire this: bind-mount (used by
     * Docker for legacy ABI compat) or symlink (used by every modern
     * distro on the host).  Symlink is simpler and doesn't care about
     * the target inode's pre-existing type — we just unlink whatever
     * stale /dev/ptmx the rootfs shipped with (if any) and create a
     * fresh symlink. */
    if (unlink("/dev/ptmx") < 0 && errno != ENOENT) {
        fprintf(stderr,
            "[devpts] unlink stale /dev/ptmx: %s\n", strerror(errno));
        /* Non-fatal — symlink() below will fail with EEXIST if there's
         * still something there, and that's the loud error worth
         * reporting. */
    }
    if (symlink("/dev/pts/ptmx", "/dev/ptmx") < 0) {
        fprintf(stderr,
            "[devpts] symlink /dev/pts/ptmx -> /dev/ptmx: %s\n",
            strerror(errno));
        return -1;
    }

    if (enable_debug) {
        printf("[devpts] mounted newinstance on /dev/pts, "
               "/dev/ptmx -> /dev/pts/ptmx\n");
    }
    return 0;
}