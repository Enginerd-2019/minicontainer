#ifndef STATE_H
#define STATE_H

#include <arpa/inet.h>
#include <limits.h>
#include <stdbool.h>
#include <sys/types.h>

#define CONTAINER_ID_LEN 12   // 12 hex chars
#define MAX_CONTAINERS 256    // List ceiling

typedef struct {
    char id[CONTAINER_ID_LEN + 1];
    pid_t pid;
    char started_at[32];
    char command[PATH_MAX];
    char argv_str[1024];

    bool enable_pid_namespace;
    bool enable_mount_namespace;
    bool enable_uts_namespace;
    bool enable_user_namespace;
    bool enable_ipc_namespace;
    bool enable_network;

    char rootfs[PATH_MAX];
    char hostname[64];
    char cgroup_path[PATH_MAX];

    bool has_veth;
    char veth_host[16];
    char veth_container[16];
    char veth_host_ip[INET_ADDRSTRLEN];
    char veth_container_ip[INET_ADDRSTRLEN];
    char veth_netmask[8];
} container_state_t;

typedef struct {
    char id[CONTAINER_ID_LEN + 1];
    pid_t pid;
    char command[64];
    char started_at[32];
    bool alive;
    size_t memory_current;
    int cpu_pct;
} container_info_t;

/**
 * Generate a fresh 12-hex-character container ID using /dev/urandom.
 * @param id_out  Buffer of at least CONTAINER_ID_LEN+1 bytes.
 */
void generate_container_id(char id_out[CONTAINER_ID_LEN + 1]);

/**
 * Return the runtime state directory.
 * For root: "/run/minicontainer".
 * For non-root: "$XDG_RUNTIME_DIR/minicontainer" or
 *   "/run/user/<uid>/minicontainer" as fallback.
 */
const char *state_dir_root(void);

/**
 * Path to a specific container's state directory:
 *   <state_dir_root()>/<id>/
 * @param id     Container ID
 * @param out    Buffer for the path
 * @param size   Buffer size
 */
void state_dir_path(const char *id, char *out, size_t size);

/**
 * Write state.json + pidfile for a container.
 * @return  0 on success, -1 on failure
 */
int state_save(const container_state_t *state);

/**
 * Load state.json for a container by ID.
 * @return  0 on success, -1 if not found or malformed
 */
int state_load(const char *id, container_state_t *out);

/**
 * Walk state_dir_root() and populate `infos[]` with up to
 * max_infos entries. Each info's `alive` is set via kill(pid, 0).
 * @return  Number of entries written, or -1 on error
 */
int state_list(container_info_t *infos, int max_infos);

/**
 * Remove a container's state directory.
 */
int state_remove(const char *id);

/**
 * `mkdir -p` semantics on a single path.  Walks the path, creates any
 * missing parent directories with the same mode, idempotent on
 * EEXIST.  Exposed publicly (Phase 7b step 7) so cli.c's cmd_start
 * can create the per-container `logs/` subdirectory before opening
 * stdout/stderr log files.
 *
 * @return  0 on success, -1 with errno set on failure
 */
int mkdir_p(const char *path, mode_t mode);

#endif // STATE_H