#ifndef MOUNT_H
#define MOUNT_H

#include <sched.h>
#include <stdbool.h>
#include <sys/types.h>

/**
 * Configuration for mount namespace and filesystem isolation
 */
typedef struct {
    const char *program;
    char *const *argv;
    char *const *envp;
    bool enable_debug;

    // Namespace flags
    bool enable_pid_namespace;
    bool enable_mount_namespace;

    // Filesystem
    const char *rootfs_path;    // NULL = no rootfs change
} mount_config_t;

/**
 * Result of mount operation
 */
typedef struct {
    pid_t child_pid;
    int exit_status;
    bool exited_normally;
    int signal;
    void *stack_ptr;
} mount_result_t;

/**
 * Execute process with mount namespace isolation.
 *
 * @param config  Configuration including rootfs path
 * @return        Result structure
 */
mount_result_t mount_exec(const mount_config_t *config);

/**
 * Cleanup resources allocated by mount_exec.
 *
 * @param result  Result from mount_exec
 */
void mount_cleanup(mount_result_t *result);

/**
 * Setup rootfs using pivot_root.
 * Called by child process before execve.
 *
 * @param rootfs_path  Path to new root filesystem
 * @param enable_debug Enable debug output
 * @return             0 on success, -1 on failure
 */
int setup_rootfs(const char *rootfs_path, bool enable_debug);

/**
 * Mount /proc filesystem inside container.
 *
 * @param enable_debug Enable debug output
 * @return             0 on success, -1 on failure
 */
int mount_proc(bool enable_debug);

#endif // MOUNT_H