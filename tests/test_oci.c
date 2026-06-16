// Phase 8c: tests/test_oci.c
// Exercises oci.c's two public functions:
//   - oci_parse_config()        — the hand-rolled recursive-descent
//                                  JSON parser (fixture-driven: writes
//                                  JSON to /tmp, parses, verifies).
//   - oci_to_container_config() — the translator that maps a parsed
//                                  oci_config_t onto container_config_t
//                                  (bind-mount split, rootfs invariants,
//                                  flat uid/gid map fields, cgroup
//                                  limits, masked/readonly paths).
//
// No root required — pure parse/translate logic, no namespaces, no
// mounts.  realpath() is exercised against /tmp (always present), so
// the bind-mount-split test stays hermetic.

#include "oci.h"
#include "core.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL: %s — %s\n", __func__, msg);          \
            return 1;                                                   \
        }                                                               \
    } while (0)

/* Write `json` to a fresh /tmp file; copy the path into `out_path`
 * (size PATH-ish).  Returns 0 on success, -1 on failure. */
static int write_fixture(const char *json, char *out_path, size_t out_size) {
    char tmp[] = "/tmp/test-oci-XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    size_t len = strlen(json);
    ssize_t n = write(fd, json, len);
    close(fd);
    if (n != (ssize_t)len) {
        unlink(tmp);
        return -1;
    }
    snprintf(out_path, out_size, "%s", tmp);
    return 0;
}

/* ===== Parser tests ===== */

static int test_parse_minimal_config(void) {
    const char *json =
        "{\"ociVersion\":\"1.0.2\","
        "\"process\":{\"args\":[\"/bin/sh\"]},"
        "\"root\":{\"path\":\"rootfs\"}}";

    char path[256];
    CHECK(write_fixture(json, path, sizeof(path)) == 0, "fixture write failed");

    oci_config_t cfg;
    char err[256] = {0};
    int rc = oci_parse_config(path, &cfg, err, sizeof(err));
    unlink(path);

    CHECK(rc == 0, err);
    CHECK(strcmp(cfg.oci_version, "1.0.2") == 0, "oci_version mismatch");
    CHECK(cfg.proc_argc == 1, "proc_argc != 1");
    CHECK(strcmp(cfg.proc_args[0], "/bin/sh") == 0, "args[0] mismatch");
    CHECK(strcmp(cfg.root_path, "rootfs") == 0, "root_path mismatch");
    printf("PASS: test_parse_minimal_config\n");
    return 0;
}

static int test_parse_full_config(void) {
    /* A representative bundle: process (args/env/cwd/terminal/user),
     * root (path/readonly), hostname, mounts (proc + a bind), the six
     * namespaces, uid/gidMappings, resources, masked/readonlyPaths. */
    const char *json =
        "{"
        "  \"ociVersion\": \"1.0.2\","
        "  \"hostname\": \"fullhost\","
        "  \"process\": {"
        "    \"terminal\": true,"
        "    \"user\": { \"uid\": 1000, \"gid\": 1000 },"
        "    \"args\": [\"/bin/sh\", \"-c\", \"echo hi\"],"
        "    \"env\": [\"PATH=/bin\", \"TERM=xterm\"],"
        "    \"cwd\": \"/work\""
        "  },"
        "  \"root\": { \"path\": \"rootfs\", \"readonly\": true },"
        "  \"mounts\": ["
        "    { \"destination\": \"/proc\", \"type\": \"proc\", \"source\": \"proc\" },"
        "    { \"destination\": \"/data\", \"type\": \"bind\", \"source\": \"/tmp\","
        "      \"options\": [\"rbind\", \"ro\"] }"
        "  ],"
        "  \"linux\": {"
        "    \"namespaces\": ["
        "      {\"type\": \"pid\"}, {\"type\": \"mount\"}, {\"type\": \"uts\"},"
        "      {\"type\": \"ipc\"}, {\"type\": \"user\"}, {\"type\": \"network\"}"
        "    ],"
        "    \"uidMappings\": [ {\"containerID\": 0, \"hostID\": 1000, \"size\": 1} ],"
        "    \"gidMappings\": [ {\"containerID\": 0, \"hostID\": 1000, \"size\": 1} ],"
        "    \"resources\": {"
        "      \"memory\": { \"limit\": 104857600 },"
        "      \"cpu\": { \"quota\": 50000, \"period\": 100000 },"
        "      \"pids\": { \"limit\": 128 }"
        "    },"
        "    \"maskedPaths\": [\"/proc/kcore\", \"/proc/keys\"],"
        "    \"readonlyPaths\": [\"/proc/sys\", \"/proc/bus\"]"
        "  }"
        "}";

    char path[256];
    CHECK(write_fixture(json, path, sizeof(path)) == 0, "fixture write failed");

    oci_config_t cfg;
    char err[256] = {0};
    int rc = oci_parse_config(path, &cfg, err, sizeof(err));
    unlink(path);
    CHECK(rc == 0, err);

    /* process */
    CHECK(cfg.proc_terminal == true, "terminal not parsed");
    CHECK(cfg.proc_uid == 1000, "uid mismatch");
    CHECK(cfg.proc_gid == 1000, "gid mismatch");
    CHECK(cfg.proc_argc == 3, "argc != 3");
    CHECK(strcmp(cfg.proc_args[2], "echo hi") == 0, "args[2] mismatch");
    CHECK(cfg.proc_env_count == 2, "env_count != 2");
    CHECK(strcmp(cfg.proc_env[0], "PATH=/bin") == 0, "env[0] mismatch");
    CHECK(strcmp(cfg.proc_cwd, "/work") == 0, "cwd mismatch");

    /* root */
    CHECK(strcmp(cfg.root_path, "rootfs") == 0, "root_path mismatch");
    CHECK(cfg.root_readonly == true, "root_readonly not parsed");

    /* hostname */
    CHECK(strcmp(cfg.hostname, "fullhost") == 0, "hostname mismatch");

    /* mounts */
    CHECK(cfg.mount_count == 2, "mount_count != 2");
    CHECK(strcmp(cfg.mounts[0].type, "proc") == 0, "mount[0] type mismatch");
    CHECK(strcmp(cfg.mounts[1].type, "bind") == 0, "mount[1] type mismatch");
    CHECK(strcmp(cfg.mounts[1].destination, "/data") == 0, "mount[1] dest mismatch");
    CHECK(strcmp(cfg.mounts[1].source, "/tmp") == 0, "mount[1] source mismatch");
    CHECK(strcmp(cfg.mounts[1].options, "rbind,ro") == 0, "mount[1] options join mismatch");

    /* namespaces */
    CHECK(cfg.ns_pid && cfg.ns_mount && cfg.ns_uts &&
          cfg.ns_ipc && cfg.ns_user && cfg.ns_network,
          "not all six namespaces parsed");

    /* mappings — first entry only */
    CHECK(cfg.has_uid_mapping, "uid mapping not captured");
    CHECK(cfg.uid_container == 0 && cfg.uid_host == 1000 && cfg.uid_size == 1,
          "uid mapping values wrong");
    CHECK(cfg.has_gid_mapping, "gid mapping not captured");
    CHECK(cfg.gid_host == 1000, "gid host wrong");

    /* resources */
    CHECK(cfg.has_memory_limit && cfg.memory_limit_bytes == 104857600ULL,
          "memory limit wrong");
    CHECK(cfg.has_cpu_quota && cfg.cpu_quota == 50000 && cfg.cpu_period == 100000,
          "cpu quota/period wrong");
    CHECK(cfg.has_pids_limit && cfg.pids_limit == 128, "pids limit wrong");

    /* masked / readonly */
    CHECK(cfg.masked_count == 2, "masked_count != 2");
    CHECK(strcmp(cfg.masked_paths[0], "/proc/kcore") == 0, "masked[0] mismatch");
    CHECK(cfg.readonly_count == 2, "readonly_count != 2");
    CHECK(strcmp(cfg.readonly_paths[1], "/proc/bus") == 0, "readonly[1] mismatch");

    printf("PASS: test_parse_full_config\n");
    return 0;
}

static int test_unsupported_oci_version(void) {
    const char *json = "{\"ociVersion\":\"2.0.0\","
                       "\"process\":{\"args\":[\"/bin/sh\"]},"
                       "\"root\":{\"path\":\"rootfs\"}}";
    char path[256];
    CHECK(write_fixture(json, path, sizeof(path)) == 0, "fixture write failed");

    oci_config_t cfg;
    char err[256] = {0};
    errno = 0;
    int rc = oci_parse_config(path, &cfg, err, sizeof(err));
    unlink(path);

    CHECK(rc == -1, "should reject ociVersion 2.0.0");
    CHECK(errno == ENOTSUP, "errno should be ENOTSUP");
    CHECK(err[0] != '\0', "error message should be set");
    printf("PASS: test_unsupported_oci_version\n");
    return 0;
}

static int test_malformed_json(void) {
    /* Missing comma between ociVersion and process. */
    const char *json = "{\"ociVersion\":\"1.0.2\""
                       "\"process\":{\"args\":[\"/bin/sh\"]}}";
    char path[256];
    CHECK(write_fixture(json, path, sizeof(path)) == 0, "fixture write failed");

    oci_config_t cfg;
    char err[256] = {0};
    errno = 0;
    int rc = oci_parse_config(path, &cfg, err, sizeof(err));
    unlink(path);

    CHECK(rc == -1, "malformed JSON should fail");
    CHECK(err[0] != '\0', "error message should be set on parse failure");
    printf("PASS: test_malformed_json\n");
    return 0;
}

static int test_missing_file(void) {
    oci_config_t cfg;
    char err[256] = {0};
    int rc = oci_parse_config("/tmp/does-not-exist-oci-XXYYZZ", &cfg,
                              err, sizeof(err));
    CHECK(rc == -1, "missing file should fail");
    CHECK(err[0] != '\0', "error message should be set for missing file");
    printf("PASS: test_missing_file\n");
    return 0;
}

/* ===== Translator tests ===== */

static int test_translate_minimal(void) {
    const char *json =
        "{\"ociVersion\":\"1.0.2\","
        "\"process\":{\"args\":[\"/bin/sh\"]},"
        "\"root\":{\"path\":\"rootfs\"}}";
    char path[256];
    CHECK(write_fixture(json, path, sizeof(path)) == 0, "fixture write failed");

    oci_config_t oci;
    char err[256] = {0};
    int rc = oci_parse_config(path, &oci, err, sizeof(err));
    unlink(path);
    CHECK(rc == 0, err);

    container_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(oci_to_container_config("/tmp", &oci, &cfg) == 0, "translate failed");

    /* root.path "rootfs" resolved relative to bundle "/tmp". */
    CHECK(cfg.rootfs_path != NULL, "rootfs_path NULL");
    CHECK(strcmp(cfg.rootfs_path, "/tmp/rootfs") == 0, "rootfs_path not resolved");
    /* cwd defaults to "/" when process.cwd absent. */
    CHECK(strcmp(cfg.cwd, "/") == 0, "cwd default not '/'");
    /* rootfs ⇒ mount-ns + pid-ns invariant (re-derived from effective cfg). */
    CHECK(cfg.enable_mount_namespace, "mount-ns not auto-enabled by rootfs");
    CHECK(cfg.enable_pid_namespace, "pid-ns not auto-enabled by rootfs");
    /* bundle path stashed for oci-state.json. */
    CHECK(strcmp(cfg.bundle_path, "/tmp") == 0, "bundle_path not set");
    printf("PASS: test_translate_minimal\n");
    return 0;
}

static int test_translate_full(void) {
    /* Bind source "/tmp" sits inside bundle "/tmp" → no outside-bundle
     * warning.  proc mount must land in oci_mounts, bind in mounts[]. */
    const char *json =
        "{"
        "  \"ociVersion\": \"1.0.2\","
        "  \"hostname\": \"xlate\","
        "  \"process\": {"
        "    \"terminal\": true,"
        "    \"args\": [\"/bin/sh\"],"
        "    \"cwd\": \"/srv\""
        "  },"
        "  \"root\": { \"path\": \"/abs/rootfs\", \"readonly\": true },"
        "  \"mounts\": ["
        "    { \"destination\": \"/proc\", \"type\": \"proc\", \"source\": \"proc\" },"
        "    { \"destination\": \"/data\", \"type\": \"bind\", \"source\": \"/tmp\","
        "      \"options\": [\"rbind\", \"ro\"] }"
        "  ],"
        "  \"linux\": {"
        "    \"namespaces\": [ {\"type\": \"uts\"}, {\"type\": \"ipc\"} ],"
        "    \"uidMappings\": [ {\"containerID\": 0, \"hostID\": 4321, \"size\": 1} ],"
        "    \"gidMappings\": [ {\"containerID\": 0, \"hostID\": 8765, \"size\": 1} ],"
        "    \"resources\": {"
        "      \"memory\": { \"limit\": 52428800 },"
        "      \"cpu\": { \"quota\": 25000, \"period\": 100000 },"
        "      \"pids\": { \"limit\": 64 }"
        "    },"
        "    \"maskedPaths\": [\"/proc/kcore\"],"
        "    \"readonlyPaths\": [\"/proc/sys\"]"
        "  }"
        "}";
    char path[256];
    CHECK(write_fixture(json, path, sizeof(path)) == 0, "fixture write failed");

    oci_config_t oci;
    char err[256] = {0};
    int rc = oci_parse_config(path, &oci, err, sizeof(err));
    unlink(path);
    CHECK(rc == 0, err);

    container_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(oci_to_container_config("/tmp", &oci, &cfg) == 0, "translate failed");

    /* Absolute root.path used verbatim (not joined to bundle). */
    CHECK(strcmp(cfg.rootfs_path, "/abs/rootfs") == 0, "abs rootfs not verbatim");
    CHECK(cfg.rootfs_readonly == true, "rootfs_readonly not propagated");

    /* terminal → enable_pty; cwd; hostname (aliases oci storage). */
    CHECK(cfg.enable_pty == true, "terminal not mapped to enable_pty");
    CHECK(strcmp(cfg.cwd, "/srv") == 0, "cwd mismatch");
    CHECK(cfg.hostname != NULL && strcmp(cfg.hostname, "xlate") == 0,
          "hostname not mapped");
    CHECK(cfg.enable_uts_namespace, "uts-ns not enabled");

    /* mount split: 1 bind in mounts[], 1 proc in oci_mounts[]. */
    CHECK(cfg.mount_count == 1, "bind mount not in mounts[]");
    CHECK(strcmp(cfg.mounts[0].container_path, "/data") == 0, "bind dest wrong");
    CHECK(cfg.mounts[0].readonly == true, "bind 'ro' option not detected");
    CHECK(strcmp(cfg.mounts[0].host_path, "/tmp") == 0, "bind host realpath wrong");
    CHECK(cfg.oci_mount_count == 1, "proc mount not in oci_mounts[]");
    CHECK(strcmp(cfg.oci_mounts[0].type, "proc") == 0, "oci_mounts[0] not proc");

    /* flat uid/gid map fields. */
    CHECK(cfg.uid_map_outside == 4321 && cfg.uid_map_range == 1, "uid map wrong");
    CHECK(cfg.gid_map_outside == 8765, "gid map wrong");

    /* cgroup limits (singular pid_limit). */
    CHECK(cfg.enable_cgroup, "cgroup not enabled by resources");
    CHECK(cfg.cgroup_limits.memory_limit == 52428800, "memory limit wrong");
    CHECK(cfg.cgroup_limits.cpu_quota == 25000, "cpu quota wrong");
    CHECK(cfg.cgroup_limits.cpu_period == 100000, "cpu period wrong");
    CHECK(cfg.cgroup_limits.pid_limit == 64, "pid_limit wrong");

    /* masked / readonly copied. */
    CHECK(cfg.masked_count == 1 && strcmp(cfg.masked_paths[0], "/proc/kcore") == 0,
          "masked path not copied");
    CHECK(cfg.readonly_count == 1 && strcmp(cfg.readonly_paths[0], "/proc/sys") == 0,
          "readonly path not copied");

    printf("PASS: test_translate_full\n");
    return 0;
}

static int test_translate_empty_args_rejected(void) {
    /* process.args empty → translation must fail (no program to run). */
    const char *json =
        "{\"ociVersion\":\"1.0.2\","
        "\"process\":{\"args\":[]},"
        "\"root\":{\"path\":\"rootfs\"}}";
    char path[256];
    CHECK(write_fixture(json, path, sizeof(path)) == 0, "fixture write failed");

    oci_config_t oci;
    char err[256] = {0};
    int rc = oci_parse_config(path, &oci, err, sizeof(err));
    unlink(path);
    CHECK(rc == 0, err);
    CHECK(oci.proc_argc == 0, "expected empty args");

    container_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(oci_to_container_config("/tmp", &oci, &cfg) == -1,
          "empty args should be rejected by translator");
    printf("PASS: test_translate_empty_args_rejected\n");
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_parse_minimal_config();
    rc |= test_parse_full_config();
    rc |= test_unsupported_oci_version();
    rc |= test_malformed_json();
    rc |= test_missing_file();
    rc |= test_translate_minimal();
    rc |= test_translate_full();
    rc |= test_translate_empty_args_rejected();
    if (rc == 0) {
        printf("\nAll OCI tests passed!\n");
    }
    return rc;
}
