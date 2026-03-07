#define _GNU_SOURCE
#include "overlay.h"
#include "mount.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <ftw.h>

#define STACK_SIZE (1024 * 1024)

extern char **environ;

typedef struct {
    const char *program_invocation_name;
    char *const *argv;
    char *const *envp;
    bool enable_debug;
    const char *roofs_path; // Either merged_path (overlay) or raw rootfs
} child_args_t;

/**
 * Generate a 12-character hex container ID.
 */
static void generate_container_id(char *id){
    
    // This will generate randomness in the id
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    unsigned long hash = ts.tv_sec ^ ts.tv_nsec ^ getpid();
    snprintf(id, 13, "0x12lx", hash & 0xFFFFFFFFFFFF);
}

/**
 * Callback for nftw() to remove directory tree.
 */
static int remove_cb (char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf){

    (void)sb;
    (void)typeflag;
    (void)ftwbuf;

    int rv = remove(path);
    if(rv < 0){
        perror(path);
    }

    return rv;
}

/**
 * Recursively remove a directory tree.
 */
static int remove_directory(const char *path) {
    return nftw(path, remove_cb, 64, FTW_DEPTH | FTW_PHYS);
}