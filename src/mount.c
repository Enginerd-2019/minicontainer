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

/*
 * Mount /sys as read-only inside the container's mount namespace.
 * Used by child_func when enable_hardening is true.
 *
 * Mounting /sys at all requires we're inside a new mount namespace
 * (which we are — clone() with CLONE_NEWNS).
 */
int mount_sys_ro(bool enable_debug)
{
    /* The build_rootfs.sh minimal rootfs ships NO /sys directory at
     * all (its mkdir list is bin/lib/lib64/proc/tmp/etc), so without
     * this the mount below fails ENOENT before the EBUSY logic ever
     * runs and the child aborts.  Same idempotent-mkdir move
     * mount_devpts makes for /dev/pts. */
    if (mkdir("/sys", 0755) < 0 && errno != EEXIST) {
        perror("mkdir(/sys)");
        return -1;
    }

    if (mount("sysfs", "/sys", "sysfs",
              MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC,
              NULL) < 0) {
        if (errno == EBUSY) {
            /* Already mounted (rootfs may have included it). Remount RO. */
            if (mount(NULL, "/sys", NULL,
                      MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID |
                      MS_NODEV | MS_NOEXEC,
                      NULL) < 0) {
                perror("mount(/sys, remount-ro)");
                return -1;
            }
        } else {
            perror("mount(/sys, RO)");
            return -1;
        }
    }

    if (enable_debug) {
        fprintf(stderr, "[mount] /sys mounted read-only\n");
    }
    return 0;
}

/* ===== Phase 8c — OCI mount / masked-path / readonly-path helpers ===== */

/* Split a comma-joined OCI mount-option string into VFS mount flags
 * (MS_*) and a residual filesystem-specific data string.  The OCI
 * `options` array conflates the two: "nosuid"/"nodev"/"noexec"/"ro"/...
 * are VFS flags the kernel wants as the mountflags argument, while
 * "mode=755"/"size=65536k"/... are tmpfs data.  Passing a VFS-flag
 * token through mount(2)'s data argument makes the call fail EINVAL
 * (the filesystem doesn't understand it) — real umoci bundles mount
 * tmpfs at /dev with exactly such mixed options, so the two must be
 * separated the way mount(8) / util-linux does. */
static unsigned long parse_mount_options(const char *opts,
                                         char *data_out, size_t data_size)
{
    unsigned long flags = 0;
    if (data_out && data_size) data_out[0] = '\0';
    if (!opts || !*opts) return 0;

    char buf[256];
    strncpy(buf, opts, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    size_t dlen = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        if      (strcmp(tok, "ro") == 0)          flags |= MS_RDONLY;
        else if (strcmp(tok, "rw") == 0)          flags &= ~(unsigned long)MS_RDONLY;
        else if (strcmp(tok, "nosuid") == 0)      flags |= MS_NOSUID;
        else if (strcmp(tok, "suid") == 0)        flags &= ~(unsigned long)MS_NOSUID;
        else if (strcmp(tok, "nodev") == 0)       flags |= MS_NODEV;
        else if (strcmp(tok, "dev") == 0)         flags &= ~(unsigned long)MS_NODEV;
        else if (strcmp(tok, "noexec") == 0)      flags |= MS_NOEXEC;
        else if (strcmp(tok, "exec") == 0)        flags &= ~(unsigned long)MS_NOEXEC;
        else if (strcmp(tok, "noatime") == 0)     flags |= MS_NOATIME;
        else if (strcmp(tok, "nodiratime") == 0)  flags |= MS_NODIRATIME;
        else if (strcmp(tok, "relatime") == 0)    flags |= MS_RELATIME;
        else if (strcmp(tok, "strictatime") == 0) flags &= ~(unsigned long)(MS_RELATIME | MS_NOATIME);
        else if (strcmp(tok, "sync") == 0)        flags |= MS_SYNCHRONOUS;
        else if (strcmp(tok, "dirsync") == 0)     flags |= MS_DIRSYNC;
        else {
            /* fs-specific data: mode=, size=, uid=, gid=, nr_inodes=, ... */
            size_t tlen = strlen(tok);
            if (data_out && dlen + tlen + (dlen ? 1u : 0u) < data_size) {
                if (dlen) data_out[dlen++] = ',';
                memcpy(data_out + dlen, tok, tlen);
                dlen += tlen;
                data_out[dlen] = '\0';
            }
        }
    }
    return flags;
}

/* Apply a single OCI mount entry. Dispatches by mount.type. */
int apply_oci_mount(const oci_mount_t *m, bool enable_debug)
{
    if (m == NULL || m->destination[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    /* /proc is handled by mount_proc — skip here. The OCI bundle
     * typically requests /proc as type "proc"; we honor it by
     * doing nothing (mount_proc runs separately and does the work). */
    if (strcmp(m->type, "proc") == 0 && strcmp(m->destination, "/proc") == 0) {
        if (enable_debug) {
            fprintf(stderr,
                "[oci-mount] Deferring /proc to mount_proc()\n");
        }
        return 0;
    }

    /* /dev/pts is handled by mount_devpts (Phase 7b §7.2.2 — runs
     * unconditionally when a rootfs is set, with the newinstance +
     * ptmxmode options the PTY path depends on).  Defer like /proc;
     * umoci bundles routinely include a devpts entry. */
    if (strcmp(m->type, "devpts") == 0) {
        if (enable_debug) {
            fprintf(stderr,
                "[oci-mount] Deferring /dev/pts to mount_devpts()\n");
        }
        return 0;
    }

    /* /sys handled by mount_sys_ro when --secure. If --secure was not
     * passed and the OCI requests /sys, mount it RW (default sysfs). */
    if (strcmp(m->type, "sysfs") == 0 && strcmp(m->destination, "/sys") == 0) {
        if (mount("sysfs", "/sys", "sysfs", 0, NULL) < 0) {
            if (errno != EBUSY) {
                perror("mount(sysfs)");
                return -1;
            }
        }
        if (enable_debug) {
            fprintf(stderr, "[oci-mount] /sys mounted (sysfs)\n");
        }
        return 0;
    }

    if (strcmp(m->type, "tmpfs") == 0) {
        /* Real bundles mount tmpfs at /dev, /dev/shm, etc. — the
         * mountpoint may not exist in the extracted rootfs, and
         * mount(2) fails ENOENT without it.  Same EEXIST-tolerant
         * mkdir bind_mount_apply uses for its targets. */
        if (mkdir(m->destination, 0755) < 0 && errno != EEXIST) {
            perror("mkdir(tmpfs destination)");
            return -1;
        }
        /* Separate VFS flags (nosuid/nodev/...) from tmpfs data
         * (mode=/size=/...) — passing the raw options string as data
         * fails EINVAL on the flag tokens. */
        char data[256];
        unsigned long mflags = parse_mount_options(m->options, data, sizeof(data));
        if (mount("tmpfs", m->destination, "tmpfs", mflags,
                  data[0] ? data : NULL) < 0) {
            perror("mount(tmpfs)");
            return -1;
        }
        if (enable_debug) {
            fprintf(stderr, "[oci-mount] tmpfs at %s (flags=0x%lx data=%s)\n",
                    m->destination, mflags, data);
        }
        return 0;
    }

    if (strcmp(m->type, "bind") == 0 || m->type[0] == '\0') {
        /* Bind mounts have a HOST source, which is unreachable here:
         * apply_oci_mount runs AFTER setup_rootfs's pivot_root, and the
         * old root is detached by then (decisions.md Error #22). So
         * bind-type OCI mounts are NOT applied here — they are converted
         * to bind_mount_t entries at parse time, appended to
         * config->mounts[], and applied by setup_rootfs BEFORE the pivot
         * (the same path --volume binds use). Reaching this branch means
         * a bind entry slipped past that conversion; treat it as a bug
         * rather than doing an inline mount whose source won't resolve. */
        fprintf(stderr,
            "[oci-mount] bind %s -> %s reached apply_oci_mount; bind "
            "mounts must be hoisted to the pre-pivot path (Error #22)\n",
            m->source, m->destination);
        errno = EINVAL;
        return -1;
    }

    /* Other types (cgroup, devpts, mqueue, ...) — silently skipped for now.
     * A production runtime handles each. Educational simplification. */
    if (enable_debug) {
        fprintf(stderr, "[oci-mount] Unsupported type '%s' — skipped\n",
                m->type);
    }
    return 0;
}

/* Mask a path by bind-mounting /dev/null over it. The /dev/null device
 * must exist inside the container — typically does after setup_rootfs
 * pivots in a real rootfs. */
int apply_masked_path(const char *path, bool enable_debug)
{
    if (!path) return 0;

    /* If the target doesn't exist, OCI says skip silently. */
    if (access(path, F_OK) < 0) {
        if (enable_debug) {
            fprintf(stderr, "[masked] %s missing, skipping\n", path);
        }
        return 0;
    }

    /* If it's a directory, bind-mount an empty tmpfs over it instead
     * of /dev/null (since /dev/null is a char device, can't replace a
     * dir). */
    struct stat sb;
    if (stat(path, &sb) < 0) {
        perror("stat(masked path)");
        return -1;
    }

    if (S_ISDIR(sb.st_mode)) {
        if (mount("tmpfs", path, "tmpfs", MS_RDONLY, NULL) < 0) {
            perror("mount(tmpfs over masked dir)");
            return -1;
        }
    } else {
        if (mount("/dev/null", path, NULL, MS_BIND, NULL) < 0) {
            perror("mount(/dev/null bind over masked path)");
            return -1;
        }
    }

    if (enable_debug) {
        fprintf(stderr, "[masked] %s\n", path);
    }
    return 0;
}

/* Remount a path as read-only via MS_BIND | MS_REMOUNT | MS_RDONLY.
 * Same kernel quirk as Phase 7b's MS_RDONLY bind mounts. */
int apply_readonly_path(const char *path, bool enable_debug)
{
    if (!path) return 0;

    if (access(path, F_OK) < 0) {
        if (enable_debug) {
            fprintf(stderr, "[readonly] %s missing, skipping\n", path);
        }
        return 0;
    }

    /* First mount(MS_BIND) to make it a self-bind, then remount RO. */
    if (mount(path, path, NULL, MS_BIND, NULL) < 0) {
        perror("mount(self-bind for readonly path)");
        return -1;
    }
    if (mount(NULL, path, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0) {
        perror("mount(remount-RO)");
        return -1;
    }

    if (enable_debug) {
        fprintf(stderr, "[readonly] %s\n", path);
    }
    return 0;
}