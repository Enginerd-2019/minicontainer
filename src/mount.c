#include "mount.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
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
    const char *rootfs_path;
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

    if(mount("proc", "/proc", "proc", 0 , NULL) < 0){
        perror("mount(proc)");
        return -1;
    }

    if(enable_debug){
        printf("[child] Mounted /proc\n");
    }

    return 0;
}

/**
 * Child function for containerized process.
 */
static int child_func(void *arg){

    child_args_t *args = (child_args_t *)arg;

    if(args->enable_debug){
        printf("[child] PID: %d\n", getpid());
    }

    // Setup rootfs if requested
    if(args->rootfs_path){
        if(setup_rootfs(args->rootfs_path, args->enable_debug) < 0){
            fprintf(stderr, "[child] Failed to setup rootfs\n");
            return -1;
        }

        // Mount /proc after pivot_root
        if(mount_proc(args->enable_debug) < 0){
            fprintf(stderr, "[child] Failed to mount /proc\n");
            return -1;
        }
    }

    // Execute target program
    char *const *envp = args->envp ? args->envp : environ;
    execve(args->program, args->argv, envp);

    // execve only returns on error
    perror("execve");
    return 127;
}

mount_result_t mount_exec(const mount_config_t *config){
    mount_result_t result = {0};
    
    // Validate
    if(!config || !config->program || !config->argv){
        fprintf(stderr, "mount_exec: invalid config\n");
        result.child_pid = -1;
        return result;
    }

    if (config->enable_debug) {
        printf("[parent] Executing: %s", config->program);
        for (int i = 0; config->argv[i]; i++) {
            printf(" %s", config->argv[i]);
        }
        printf("\n");

        if (config->rootfs_path) {
            printf("[parent] Rootfs: %s\n", config->rootfs_path);
        }
    }

    // Allocate stack
    char *stack = malloc(STACK_SIZE);
    if(!stack){
        perror("malloc");
        result.child_pid = -1;
        return result;
    }
    
    result.stack_ptr = stack;

    // Prepare child args
    child_args_t child_args = {
        .program = config->program,
        .argv = config->argv,
        .envp = config->envp,
        .enable_debug = config->enable_debug,
        .rootfs_path = config->rootfs_path
    };

    // Build flags
    int flags = SIGCHLD;
    if (config->enable_pid_namespace) {
        flags |= CLONE_NEWPID;
    }
    if (config->enable_mount_namespace) {
        flags |= CLONE_NEWNS;
    }

    if (config->enable_debug && (flags & CLONE_NEWNS)) {
        printf("[parent] Creating mount namespace\n");
    }

    pid_t pid = clone(child_func, stack + STACK_SIZE, flags, &child_args);

    if(pid < 0){
        perror("clone");
        free(stack);
        result.child_pid = -1;
        result.stack_ptr = NULL;
        return result;
    }

    result.child_pid = pid;

    if(config->enable_debug){
        printf("[parent] Child PID: %d\n", pid);
    }

    // Wait for child
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        result.exit_status = -1;
        return result;
    }

    // Parse status
    if (WIFEXITED(status)) {
        result.exited_normally = true;
        result.exit_status = WEXITSTATUS(status);

        if (config->enable_debug) {
            printf("[parent] Child exited: %d\n", result.exit_status);
        }
    } else if (WIFSIGNALED(status)) {
        result.exited_normally = false;
        result.signal = WTERMSIG(status);
        result.exit_status = 128 + result.signal;

        if (config->enable_debug) {
            printf("[parent] Child killed by signal: %d\n", result.signal);
        }
    }

    return result;
}

void mount_cleanup(mount_result_t *result) {
    if (result && result->stack_ptr) {
        free(result->stack_ptr);
        result->stack_ptr = NULL;
    }
}