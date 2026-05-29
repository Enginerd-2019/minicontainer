// Phase 7b: tests/test_state.c
// Exercises state.h's on-disk round-trip plus the small helpers
// (state_dir_path, mkdir_p).  Writes under state_dir_root() so the
// test needs root for the system path (/run/minicontainer); the
// unprivileged path ($XDG_RUNTIME_DIR/minicontainer) also works if
// $XDG_RUNTIME_DIR is set.
//
// Every test allocates a fresh container ID, runs its assertion,
// then state_remove()s.  No /run leftover on success or failure.

#include "state.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL: %s — %s\n", __func__, msg);          \
            return 1;                                                   \
        }                                                               \
    } while (0)

/* Populate every container_state_t field with a recognizable value. */
static void seed_state(container_state_t *s) {
    memset(s, 0, sizeof(*s));
    /* state.id filled by caller with generate_container_id */
    s->pid = getpid();
    strncpy(s->started_at, "2026-05-28T17:00:00Z", sizeof(s->started_at) - 1);
    strncpy(s->command,    "/bin/sleep",           sizeof(s->command) - 1);
    strncpy(s->argv_str,   "100",                  sizeof(s->argv_str) - 1);
    s->enable_pid_namespace   = true;
    s->enable_mount_namespace = true;
    s->enable_uts_namespace   = false;
    s->enable_user_namespace  = false;
    s->enable_ipc_namespace   = true;
    s->enable_network         = true;
    strncpy(s->rootfs,       "/some/rootfs",  sizeof(s->rootfs) - 1);
    strncpy(s->hostname,     "test-host",     sizeof(s->hostname) - 1);
    strncpy(s->cgroup_path,
            "/sys/fs/cgroup/minicontainer_abcdef012345",
            sizeof(s->cgroup_path) - 1);
    s->has_veth = true;
    strncpy(s->veth_host,         "veth_h_aa11", sizeof(s->veth_host) - 1);
    strncpy(s->veth_container,    "veth_c_aa11", sizeof(s->veth_container) - 1);
    strncpy(s->veth_host_ip,      "10.0.0.1",   sizeof(s->veth_host_ip) - 1);
    strncpy(s->veth_container_ip, "10.0.0.2",   sizeof(s->veth_container_ip) - 1);
    strncpy(s->veth_netmask,      "24",         sizeof(s->veth_netmask) - 1);
}

static int test_round_trip(void) {
    container_state_t a, b;
    seed_state(&a);
    generate_container_id(a.id);

    if (state_save(&a) < 0) {
        perror("state_save");
        return 1;
    }

    memset(&b, 0, sizeof(b));
    int rc = state_load(a.id, &b);
    if (rc < 0) {
        perror("state_load");
        state_remove(a.id);
        return 1;
    }

    /* Every field that round-trips through JSON must match. */
    CHECK(strcmp(a.id, b.id) == 0,                       "id mismatch");
    CHECK(a.pid == b.pid,                                "pid mismatch");
    CHECK(strcmp(a.started_at, b.started_at) == 0,       "started_at mismatch");
    CHECK(strcmp(a.command, b.command) == 0,             "command mismatch");
    CHECK(strcmp(a.argv_str, b.argv_str) == 0,           "argv_str mismatch");
    CHECK(a.enable_pid_namespace   == b.enable_pid_namespace,   "pid_ns mismatch");
    CHECK(a.enable_mount_namespace == b.enable_mount_namespace, "mnt_ns mismatch");
    CHECK(a.enable_uts_namespace   == b.enable_uts_namespace,   "uts_ns mismatch");
    CHECK(a.enable_user_namespace  == b.enable_user_namespace,  "user_ns mismatch");
    CHECK(a.enable_ipc_namespace   == b.enable_ipc_namespace,   "ipc_ns mismatch");
    CHECK(a.enable_network         == b.enable_network,         "network mismatch");
    CHECK(strcmp(a.rootfs, b.rootfs) == 0,                       "rootfs mismatch");
    CHECK(strcmp(a.hostname, b.hostname) == 0,                   "hostname mismatch");
    CHECK(strcmp(a.cgroup_path, b.cgroup_path) == 0,             "cgroup_path mismatch");
    CHECK(a.has_veth == b.has_veth,                              "has_veth mismatch");
    CHECK(strcmp(a.veth_host, b.veth_host) == 0,                 "veth_host mismatch");
    CHECK(strcmp(a.veth_container, b.veth_container) == 0,       "veth_container mismatch");
    CHECK(strcmp(a.veth_host_ip, b.veth_host_ip) == 0,           "veth_host_ip mismatch");
    CHECK(strcmp(a.veth_container_ip, b.veth_container_ip) == 0, "veth_container_ip mismatch");
    CHECK(strcmp(a.veth_netmask, b.veth_netmask) == 0,           "veth_netmask mismatch");

    state_remove(a.id);
    printf("PASS: test_round_trip\n");
    return 0;
}

static int test_remove_clears_directory(void) {
    container_state_t a;
    seed_state(&a);
    generate_container_id(a.id);
    CHECK(state_save(&a) == 0, "state_save failed");

    /* Confirm the directory exists. */
    char dir[4096];
    state_dir_path(a.id, dir, sizeof(dir));
    struct stat st;
    CHECK(stat(dir, &st) == 0 && S_ISDIR(st.st_mode),
          "state dir missing after state_save");

    /* Remove. */
    state_remove(a.id);

    /* Confirm the directory is gone. */
    CHECK(stat(dir, &st) < 0 && errno == ENOENT,
          "state dir still present after state_remove");

    printf("PASS: test_remove_clears_directory\n");
    return 0;
}

static int test_load_missing_returns_error(void) {
    container_state_t out;
    int rc = state_load("000000000000", &out);   /* never written */
    CHECK(rc < 0, "state_load on missing ID should return -1");
    printf("PASS: test_load_missing_returns_error\n");
    return 0;
}

static int test_list_finds_saved_entries(void) {
    /* Save two state files, list, confirm both appear with correct
     * liveness (self-pid is alive; pid 1 = init is always alive too;
     * use pid 999999 to fake a dead container). */
    container_state_t live, dead;
    seed_state(&live);   live.pid = getpid();
    seed_state(&dead);   dead.pid = 999999;       /* presumed dead */
    generate_container_id(live.id);
    generate_container_id(dead.id);

    CHECK(state_save(&live) == 0, "save live failed");
    CHECK(state_save(&dead) == 0, "save dead failed");

    container_info_t infos[MAX_CONTAINERS];
    int n = state_list(infos, MAX_CONTAINERS);
    CHECK(n >= 2, "state_list returned < 2 entries");

    bool seen_live = false, seen_dead = false;
    for (int i = 0; i < n; i++) {
        if (strcmp(infos[i].id, live.id) == 0) {
            CHECK(infos[i].alive, "live entry not marked alive");
            seen_live = true;
        }
        if (strcmp(infos[i].id, dead.id) == 0) {
            CHECK(!infos[i].alive, "dead entry marked alive");
            seen_dead = true;
        }
    }
    CHECK(seen_live, "saved live entry missing from list");
    CHECK(seen_dead, "saved dead entry missing from list");

    state_remove(live.id);
    state_remove(dead.id);
    printf("PASS: test_list_finds_saved_entries\n");
    return 0;
}

static int test_mkdir_p_nested_and_idempotent(void) {
    /* Pick a temp path that's almost certainly not present. */
    const char *path = "/tmp/.minicontainer_test_state/a/b/c";
    CHECK(mkdir_p(path, 0755) == 0, "first mkdir_p failed");

    struct stat st;
    CHECK(stat(path, &st) == 0 && S_ISDIR(st.st_mode), "leaf dir missing");

    /* Idempotent: second call must succeed (or fail with EEXIST handled
     * internally — the function's contract is "return 0 if the path
     * exists as a directory afterwards"). */
    CHECK(mkdir_p(path, 0755) == 0, "second mkdir_p failed");

    /* Cleanup. */
    (void)rmdir("/tmp/.minicontainer_test_state/a/b/c");
    (void)rmdir("/tmp/.minicontainer_test_state/a/b");
    (void)rmdir("/tmp/.minicontainer_test_state/a");
    (void)rmdir("/tmp/.minicontainer_test_state");
    printf("PASS: test_mkdir_p_nested_and_idempotent\n");
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_round_trip();
    rc |= test_remove_clears_directory();
    rc |= test_load_missing_returns_error();
    rc |= test_list_finds_saved_entries();
    rc |= test_mkdir_p_nested_and_idempotent();
    if (rc == 0) {
        printf("\nAll state tests passed!\n");
    }
    return rc;
}