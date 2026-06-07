// Phase 7b: tests/test_bind.c
// Exercises bind_mount_apply() from mount.c.
//
// Each test runs in a fresh mount namespace via unshare(CLONE_NEWNS),
// so the bind mounts the test creates never leak to the host even if
// the test crashes before its umount.  The mount-namespace propagation
// is set to MS_PRIVATE on '/' first — otherwise, on systemd-managed
// hosts the parent's shared propagation would push our mounts back
// onto the host and break the isolation.
//
// Requires root for CAP_SYS_ADMIN (unshare + mount).  Tests skip
// cleanly if invoked unprivileged.

#include "mount.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL: %s — %s\n", __func__, msg);          \
            return 1;                                                   \
        }                                                               \
    } while (0)

/* Each test runs in a child process so its mount namespace is fully
 * disposable.  Parent waits and propagates the child's exit code. */
typedef int (*test_fn)(void);

static int run_in_isolated_mount_ns(test_fn fn) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        if (unshare(CLONE_NEWNS) < 0) {
            perror("unshare(CLONE_NEWNS)");
            _exit(1);
        }
        /* Make every existing mount slave so our subsequent mounts
         * (and any umounts) don't propagate to the host. */
        if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
            perror("mount(/, MS_REC|MS_PRIVATE)");
            _exit(1);
        }
        _exit(fn());
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); return 1; }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* Helper: write `data` to `path`.  Truncates if exists. */
static int write_file(const char *path, const char *data) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    ssize_t n = write(fd, data, strlen(data));
    close(fd);
    return n == (ssize_t)strlen(data) ? 0 : -1;
}

/* Helper: read up to size-1 bytes from `path` into `buf`.  Returns 0
 * on success and NUL-terminates buf. */
static int read_file(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return 0;
}

/* ---------- test 1: directory bind ---------- */

static int run_test_bind_directory(void) {
    /* Setup: /tmp/.bind_src/marker.txt = "hello-bind" */
    mkdir("/tmp/.bind_src", 0755);
    mkdir("/tmp/.bind_tgt", 0755);
    CHECK(write_file("/tmp/.bind_src/marker.txt", "hello-bind") == 0,
          "write source marker failed");

    bind_mount_t m = {0};
    strncpy(m.host_path,      "/tmp/.bind_src", sizeof(m.host_path) - 1);
    strncpy(m.container_path, "/tmp/.bind_tgt", sizeof(m.container_path) - 1);
    m.readonly = false;

    CHECK(bind_mount_apply(&m, NULL, false) == 0, "bind_mount_apply failed");

    /* Read the marker through the target.  Should be the source content. */
    char buf[64];
    CHECK(read_file("/tmp/.bind_tgt/marker.txt", buf, sizeof(buf)) == 0,
          "read through bind target failed");
    CHECK(strcmp(buf, "hello-bind") == 0, "bind mount content mismatch");

    /* Mount lives in this process's mount namespace; umount lazily so
     * the directory cleanup below doesn't fight a busy mount.  Same
     * applies in every test below. */
    umount2("/tmp/.bind_tgt", MNT_DETACH);
    unlink("/tmp/.bind_src/marker.txt");
    rmdir("/tmp/.bind_src");
    rmdir("/tmp/.bind_tgt");
    printf("PASS: test_bind_directory\n");
    return 0;
}

/* ---------- test 2: read-only bind ---------- */

static int run_test_bind_readonly(void) {
    mkdir("/tmp/.bind_src_ro", 0755);
    mkdir("/tmp/.bind_tgt_ro", 0755);
    CHECK(write_file("/tmp/.bind_src_ro/data.txt", "original") == 0,
          "write source failed");

    bind_mount_t m = {0};
    strncpy(m.host_path,      "/tmp/.bind_src_ro", sizeof(m.host_path) - 1);
    strncpy(m.container_path, "/tmp/.bind_tgt_ro", sizeof(m.container_path) - 1);
    m.readonly = true;

    CHECK(bind_mount_apply(&m, NULL, false) == 0, "bind_mount_apply (ro) failed");

    /* Reads still work. */
    char buf[64];
    CHECK(read_file("/tmp/.bind_tgt_ro/data.txt", buf, sizeof(buf)) == 0,
          "read on ro-bind failed");
    CHECK(strcmp(buf, "original") == 0, "ro-bind content mismatch");

    /* Writes must fail with EROFS.  Open with O_WRONLY against the
     * bind-mounted file. */
    int fd = open("/tmp/.bind_tgt_ro/data.txt", O_WRONLY | O_TRUNC);
    if (fd >= 0) {
        close(fd);
        umount2("/tmp/.bind_tgt_ro", MNT_DETACH);
        unlink("/tmp/.bind_src_ro/data.txt");
        rmdir("/tmp/.bind_src_ro");
        rmdir("/tmp/.bind_tgt_ro");
        fprintf(stderr, "FAIL: ro-bind allowed open(O_WRONLY)\n");
        return 1;
    }
    CHECK(errno == EROFS, "open(O_WRONLY) on ro-bind returned wrong errno");

    umount2("/tmp/.bind_tgt_ro", MNT_DETACH);
    unlink("/tmp/.bind_src_ro/data.txt");
    rmdir("/tmp/.bind_src_ro");
    rmdir("/tmp/.bind_tgt_ro");
    printf("PASS: test_bind_readonly\n");
    return 0;
}

/* ---------- test 3: file bind (not directory) ---------- */

static int run_test_bind_file(void) {
    /* bind_mount_apply detects regular files via S_ISDIR check and
     * touches the target with open(O_CREAT) instead of mkdir. */
    CHECK(write_file("/tmp/.bind_src_file.txt", "file-bind-content") == 0,
          "write source file failed");

    /* Target's parent must exist; touch a placeholder there. */
    CHECK(write_file("/tmp/.bind_tgt_file.txt", "to-be-overlaid") == 0,
          "write target placeholder failed");

    bind_mount_t m = {0};
    strncpy(m.host_path,      "/tmp/.bind_src_file.txt", sizeof(m.host_path) - 1);
    strncpy(m.container_path, "/tmp/.bind_tgt_file.txt", sizeof(m.container_path) - 1);
    m.readonly = false;

    CHECK(bind_mount_apply(&m, NULL, false) == 0, "bind_mount_apply on file failed");

    char buf[64];
    CHECK(read_file("/tmp/.bind_tgt_file.txt", buf, sizeof(buf)) == 0,
          "read through file-bind failed");
    CHECK(strcmp(buf, "file-bind-content") == 0,
          "file-bind content mismatch (placeholder visible?)");

    umount2("/tmp/.bind_tgt_file.txt", MNT_DETACH);
    unlink("/tmp/.bind_src_file.txt");
    unlink("/tmp/.bind_tgt_file.txt");
    printf("PASS: test_bind_file\n");
    return 0;
}

int main(void) {
    if (geteuid() != 0) {
        printf("SKIP: test_bind requires root (CAP_SYS_ADMIN for "
               "unshare + mount)\n");
        return 0;
    }

    int rc = 0;
    rc |= run_in_isolated_mount_ns(run_test_bind_directory);
    rc |= run_in_isolated_mount_ns(run_test_bind_readonly);
    rc |= run_in_isolated_mount_ns(run_test_bind_file);
    if (rc == 0) {
        printf("\nAll bind-mount tests passed!\n");
    }
    return rc;
}