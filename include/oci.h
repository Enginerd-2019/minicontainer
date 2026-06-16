#ifndef OCI_H
#define OCI_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>   /* size_t — oci.h is included standalone by oci.c */
#include <limits.h>

#define OCI_MAX_ARGS         64
#define OCI_MAX_ENV          64
#define OCI_MAX_MOUNTS       32
#define OCI_MAX_NAMESPACES   8
#define OCI_MAX_MASKED       32
#define OCI_MAX_READONLY     32
#define OCI_MAX_PATH         PATH_MAX

/* A single mount entry from config.json's mounts[] array. */
typedef struct {
    char destination[OCI_MAX_PATH];
    char source[OCI_MAX_PATH];
    char type[64];
    char options[256];
} oci_mount_t;

/* Parsed representation of an OCI config.json.
 * Fields are populated by oci_parse_config; consumed by
 * oci_to_container_config. Fields not present in the JSON are
 * left zero-initialized. */
typedef struct {
    char     oci_version[16];
    char     hostname[256];

    char     proc_args[OCI_MAX_ARGS][OCI_MAX_PATH];
    int      proc_argc;
    char     proc_env[OCI_MAX_ENV][OCI_MAX_PATH];
    int      proc_env_count;
    char     proc_cwd[OCI_MAX_PATH];
    uint32_t proc_uid;
    uint32_t proc_gid;
    bool     proc_terminal;

    char     root_path[OCI_MAX_PATH];
    bool     root_readonly;

    oci_mount_t mounts[OCI_MAX_MOUNTS];
    int         mount_count;

    bool ns_pid, ns_mount, ns_uts, ns_user, ns_ipc, ns_network, ns_cgroup;

    bool     has_uid_mapping;
    uint32_t uid_container, uid_host;
    uint32_t uid_size;
    bool     has_gid_mapping;
    uint32_t gid_container, gid_host;
    uint32_t gid_size;

    bool     has_memory_limit;
    uint64_t memory_limit_bytes;
    bool     has_cpu_quota;
    int64_t  cpu_quota;
    int64_t  cpu_period;
    bool     has_pids_limit;
    int64_t  pids_limit;

    char masked_paths[OCI_MAX_MASKED][OCI_MAX_PATH];
    int  masked_count;
    char readonly_paths[OCI_MAX_READONLY][OCI_MAX_PATH];
    int  readonly_count;

    int  ignored_count;  /* Number of fields silently ignored. */
} oci_config_t;

/* No core.h include and no forward declaration here — we want oci.h
 * independent of core.h for testability, and container_config_t
 * typedefs an ANONYMOUS struct in core.h, so there is no
 * `struct container_config` tag to forward-declare anyway. The
 * translator below takes a void* and casts internally; oci.c includes
 * core.h for the cast, and core.h includes oci.h for oci_mount_t.
 * The header dependency is strictly one-directional: core.h -> oci.h. */

/*
 * Parse an OCI config.json file at the given path.
 *
 * Reads the file into memory, runs a recursive-descent JSON parser,
 * populates *out_cfg.
 *
 * config_path  Absolute or relative path to config.json.
 * out_cfg      Caller-allocated; will be zero-initialized internally.
 *
 * Returns 0 on success. On failure returns -1 and sets errno; if
 * out_err is non-NULL, writes a human-readable error message to it
 * (up to err_size bytes including null terminator).
 */
int oci_parse_config(const char *config_path,
                     oci_config_t *out_cfg,
                     char *out_err, size_t err_size);

/*
 * Translate a parsed oci_config_t into a container_config_t.
 *
 * Maps OCI fields to minicontainer fields. Fields the OCI config
 * doesn't specify are LEFT UNCHANGED in *cfg — callers can pre-populate
 * cfg with defaults (or with CLI overrides) before calling.
 *
 * bundle_dir  Path to the bundle directory; root.path is resolved
 *             relative to this.
 * oci_cfg     Source — typically populated by oci_parse_config.
 * cfg         Destination — pointer to caller's container_config_t.
 *             Type-erased to void* to break the oci.h <-> core.h header
 *             coupling.
 *
 * Returns 0 on success, -1 on translation error (e.g., args array
 * empty, root.path missing).
 */
int oci_to_container_config(const char *bundle_dir,
                            const oci_config_t *oci_cfg,
                            void *cfg);

#endif /* OCI_H */
