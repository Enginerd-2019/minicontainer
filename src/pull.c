/*
 * pull.c — Minimal tarball downloader.
 *
 * NOT a registry client: a hardcoded whitelist of known rootfs URLs
 * (alpine, ubuntu) fetched via curl/wget + tar. A deliberate toy —
 * for arbitrary images use `skopeo` to fetch and `umoci unpack`.
 */

#include "pull.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>   /* PATH_MAX — pull.c uses it but the other headers don't pull it in */

typedef struct {
    const char *tag;
    const char *url;
    const char *bundle_dir;
    const char *archive_ext;
} pull_entry_t;

static const pull_entry_t pull_whitelist[] = {
    {
        .tag         = "alpine",
        .url         = "https://dl-cdn.alpinelinux.org/alpine/v3.18/releases/x86_64/alpine-minirootfs-3.18.4-x86_64.tar.gz",
        .bundle_dir  = "./alpine-bundle",
        .archive_ext = ".tar.gz",
    },
    {
        .tag         = "ubuntu",
        .url         = "https://cloud-images.ubuntu.com/jammy/current/jammy-server-cloudimg-amd64-root.tar.xz",
        .bundle_dir  = "./ubuntu-bundle",
        .archive_ext = ".tar.xz",
    },
    { NULL, NULL, NULL, NULL }
};

static const pull_entry_t *find_entry(const char *tag)
{
    for (int i = 0; pull_whitelist[i].tag != NULL; i++) {
        if (strcmp(pull_whitelist[i].tag, tag) == 0) {
            return &pull_whitelist[i];
        }
    }
    return NULL;
}

/* Fork+exec a command, wait for completion. Returns 0 on success,
 * -1 on fork/exec failure or non-zero exit. */
static int run_command(const char *argv[])
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], (char *const *)argv);
        perror("execvp");
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "command %s failed (status %d)\n", argv[0], status);
        return -1;
    }
    return 0;
}

/* Recursive mkdir -p (mode 0755). Returns 0 on success / already-exists,
 * -1 on real error. */
static int mkdir_p_local(const char *path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
                perror("mkdir");
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
        perror("mkdir");
        return -1;
    }
    return 0;
}

/* Write a minimal config.json into <bundle_dir>/config.json that points
 * at rootfs/ and runs /bin/sh by default. The user can edit this. */
static int write_default_config_json(const char *bundle_dir)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/config.json", bundle_dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        perror("fopen(config.json)");
        return -1;
    }
    fprintf(f,
        "{\n"
        "    \"ociVersion\": \"1.0.2\",\n"
        "    \"process\": {\n"
        "        \"terminal\": true,\n"
        "        \"user\": { \"uid\": 0, \"gid\": 0 },\n"
        "        \"args\": [\"/bin/sh\"],\n"
        "        \"env\": [\n"
        "            \"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\",\n"
        "            \"TERM=xterm\"\n"
        "        ],\n"
        "        \"cwd\": \"/\"\n"
        "    },\n"
        "    \"root\": { \"path\": \"rootfs\", \"readonly\": false },\n"
        "    \"hostname\": \"minicontainer\",\n"
        "    \"mounts\": [\n"
        "        { \"destination\": \"/proc\", \"type\": \"proc\", \"source\": \"proc\" }\n"
        "    ],\n"
        "    \"linux\": {\n"
        "        \"namespaces\": [\n"
        "            { \"type\": \"pid\" },\n"
        "            { \"type\": \"mount\" },\n"
        "            { \"type\": \"uts\" }\n"
        "        ]\n"
        "    }\n"
        "}\n");
    fclose(f);
    return 0;
}

int cmd_pull(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: minicontainer pull <tag>\n"
            "Supported tags:\n");
        for (int i = 0; pull_whitelist[i].tag != NULL; i++) {
            fprintf(stderr, "  %s — %s\n",
                    pull_whitelist[i].tag, pull_whitelist[i].url);
        }
        fprintf(stderr,
            "\nNote: minicontainer pull is NOT a registry client. For\n"
            "arbitrary images, use `skopeo` to fetch + `umoci unpack`.\n");
        return 1;
    }

    const pull_entry_t *e = find_entry(argv[1]);
    if (!e) {
        fprintf(stderr,
            "Unknown tag: %s\n"
            "Supported tags: ", argv[1]);
        for (int i = 0; pull_whitelist[i].tag != NULL; i++) {
            fprintf(stderr, "%s%s", pull_whitelist[i].tag,
                    pull_whitelist[i+1].tag ? ", " : "\n");
        }
        return 1;
    }

    /* Create bundle directory tree. */
    char rootfs_dir[PATH_MAX];
    snprintf(rootfs_dir, sizeof(rootfs_dir), "%s/rootfs", e->bundle_dir);
    if (mkdir_p_local(rootfs_dir) < 0) return 1;

    /* Tempfile path inside the bundle (avoids cross-FS rename issues). */
    char tmp_archive[PATH_MAX];
    snprintf(tmp_archive, sizeof(tmp_archive), "%s/rootfs%s",
             e->bundle_dir, e->archive_ext);

    /* Download. Prefer curl; fall back to wget. */
    const char *download_argv_curl[] = {
        "curl", "-fsSL", "-o", tmp_archive, e->url, NULL
    };
    const char *download_argv_wget[] = {
        "wget", "-q", "-O", tmp_archive, e->url, NULL
    };
    int rc = run_command(download_argv_curl);
    if (rc != 0) {
        fprintf(stderr, "curl failed, trying wget...\n");
        rc = run_command(download_argv_wget);
    }
    if (rc != 0) {
        fprintf(stderr, "Failed to download %s\n", e->url);
        return 1;
    }

    /* Extract. tar handles both .gz and .xz via auto-detect. */
    const char *extract_argv[] = {
        "tar", "-xf", tmp_archive, "-C", rootfs_dir, NULL
    };
    if (run_command(extract_argv) != 0) {
        fprintf(stderr, "Failed to extract %s\n", tmp_archive);
        unlink(tmp_archive);
        return 1;
    }

    /* Cleanup tempfile. */
    unlink(tmp_archive);

    /* Write default config.json. */
    if (write_default_config_json(e->bundle_dir) < 0) {
        fprintf(stderr, "Failed to write default config.json\n");
        return 1;
    }

    fprintf(stderr,
        "Pulled %s.\n"
        "Bundle at: %s\n"
        "Run with: sudo ./minicontainer run --bundle %s\n",
        argv[1], e->bundle_dir, e->bundle_dir);
    return 0;
}
