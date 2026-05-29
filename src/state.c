// Note: _GNU_SOURCE is provided by the Makefile.
#include "state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>

#define STATE_ROOT_SYSTEM "/run/minicontainer"

void generate_container_id(char id_out[CONTAINER_ID_LEN + 1]) {
    unsigned char bytes[6];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        /* Fallback: time-based, less unique but never fails. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        unsigned long mix = (unsigned long)ts.tv_sec ^ ts.tv_nsec;
        snprintf(id_out, CONTAINER_ID_LEN + 1, "%012lx", mix & 0xFFFFFFFFFFFFul);
        return;
    }
    if (read(fd, bytes, 6) != 6) {
        close(fd);
        /* Same fallback. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        unsigned long mix = (unsigned long)ts.tv_sec ^ ts.tv_nsec;
        snprintf(id_out, CONTAINER_ID_LEN + 1, "%012lx", mix & 0xFFFFFFFFFFFFul);
        return;
    }
    close(fd);
    for (int i = 0; i < 6; i++) {
        snprintf(id_out + 2*i, 3, "%02x", bytes[i]);
    }
    id_out[CONTAINER_ID_LEN] = '\0';
}

const char *state_dir_root(void) {
    static char buf[PATH_MAX];

    if (geteuid() == 0) {
        return STATE_ROOT_SYSTEM;
    }

    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0]) {
        snprintf(buf, sizeof(buf), "%s/minicontainer", xdg);
        return buf;
    }

    snprintf(buf, sizeof(buf), "/run/user/%u/minicontainer", getuid());
    return buf;
}

void state_dir_path(const char *id, char *out, size_t size) {
    snprintf(out, size, "%s/%s", state_dir_root(), id);
}

int mkdir_p(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);   /* ignore EEXIST */
            *p = '/';
        }
    }
    return mkdir(tmp, mode);
}

int state_save(const container_state_t *s) {
    char dir[PATH_MAX], state_path[PATH_MAX], pidfile[PATH_MAX];

    state_dir_path(s->id, dir, sizeof(dir));
    mkdir_p(dir, 0755);

    /* Compose absolute paths inside dir.  Truncation here would mean
     * state_dir_root() + id exceeded PATH_MAX, which the runtime can't
     * recover from — bail and let the caller treat as save failure. */
    int n;
    n = snprintf(state_path, sizeof(state_path), "%s/state.json", dir);
    if (n < 0 || (size_t)n >= sizeof(state_path)) {
        fprintf(stderr, "state_save: state_path truncated for id %s\n", s->id);
        return -1;
    }
    n = snprintf(pidfile, sizeof(pidfile), "%s/pidfile", dir);
    if (n < 0 || (size_t)n >= sizeof(pidfile)) {
        fprintf(stderr, "state_save: pidfile path truncated for id %s\n", s->id);
        return -1;
    }

    FILE *f = fopen(state_path, "w");
    if (!f) {
        perror("fopen(state.json)");
        return -1;
    }

    fprintf(f,
        "{\n"
        "  \"id\":          \"%s\",\n"
        "  \"pid\":         %d,\n"
        "  \"started_at\":  \"%s\",\n"
        "  \"command\":     \"%s\",\n"
        "  \"argv\":        \"%s\",\n"
        "  \"rootfs\":      \"%s\",\n"
        "  \"hostname\":    \"%s\",\n"
        "  \"cgroup_path\": \"%s\",\n"
        "  \"namespaces\": {\n"
        "    \"pid\":     %s,\n"
        "    \"mount\":   %s,\n"
        "    \"uts\":     %s,\n"
        "    \"user\":    %s,\n"
        "    \"ipc\":     %s,\n"
        "    \"network\": %s\n"
        "  },\n",
        s->id, s->pid, s->started_at,
        s->command, s->argv_str,
        s->rootfs, s->hostname, s->cgroup_path,
        s->enable_pid_namespace   ? "true" : "false",
        s->enable_mount_namespace ? "true" : "false",
        s->enable_uts_namespace   ? "true" : "false",
        s->enable_user_namespace  ? "true" : "false",
        s->enable_ipc_namespace   ? "true" : "false",
        s->enable_network         ? "true" : "false");

    if (s->has_veth) {
        fprintf(f,
            "  \"veth\": {\n"
            "    \"host\":         \"%s\",\n"
            "    \"container\":    \"%s\",\n"
            "    \"host_ip\":      \"%s\",\n"
            "    \"container_ip\": \"%s\",\n"
            "    \"netmask\":      \"%s\"\n"
            "  }\n",
            s->veth_host, s->veth_container,
            s->veth_host_ip, s->veth_container_ip, s->veth_netmask);
    } else {
        fprintf(f, "  \"veth\": null\n");
    }
    fprintf(f, "}\n");
    fclose(f);

    FILE *pf = fopen(pidfile, "w");
    if (pf) {
        fprintf(pf, "%d\n", s->pid);
        fclose(pf);
    }

    return 0;
}

/* Hand-rolled JSON parser. We don't link jansson/cJSON — the schema
 * is fixed (state_save writes it), so a line-by-line key/value scan
 * is sufficient. Three helpers do the heavy lifting:
 *   - find_key() locates "key" in the buffer
 *   - extract_string() reads the quoted value after a found key
 *   - extract_int() and extract_bool() handle the non-string types
 * Robust against missing optional fields. */

static const char *find_key(const char *buf, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    return strstr(buf, pat);
}

static int extract_string(const char *buf, const char *key,
                          char *out, size_t out_size) {
    const char *p = find_key(buf, key);
    if (!p) return -1;
    /* Skip past "key": to the opening quote of the value. */
    p = strchr(p, ':');
    if (!p) return -1;
    p = strchr(p, '"');
    if (!p) return -1;
    p++;  /* past the opening quote */
    const char *end = strchr(p, '"');
    if (!end) return -1;
    size_t len = (size_t)(end - p);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

static int extract_int(const char *buf, const char *key, long *out) {
    const char *p = find_key(buf, key);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    char *endp;
    long val = strtol(p, &endp, 10);
    if (endp == p) return -1;
    *out = val;
    return 0;
}

static int extract_bool(const char *buf, const char *key, bool *out) {
    const char *p = find_key(buf, key);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0)  { *out = true;  return 0; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return 0; }
    return -1;
}

int state_load(const char *id, container_state_t *out) {
    if (!id || !out) return -1;

    char dir[PATH_MAX], state_path[PATH_MAX];
    state_dir_path(id, dir, sizeof(dir));
    int path_n = snprintf(state_path, sizeof(state_path), "%s/state.json", dir);
    if (path_n < 0 || (size_t)path_n >= sizeof(state_path)) {
        fprintf(stderr, "state_load: state_path truncated for id %s\n", id);
        return -1;
    }

    FILE *f = fopen(state_path, "r");
    if (!f) return -1;

    /* Read whole file into a buffer. State files are <2 KB in practice;
     * 16 KB bounds the read with margin. */
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return -1;
    buf[n] = '\0';

    memset(out, 0, sizeof(*out));
    long lval;

    /* Required scalar fields. */
    if (extract_string(buf, "id",          out->id,           sizeof(out->id))         < 0) return -1;
    if (extract_int   (buf, "pid",         &lval)                                       < 0) return -1;
    out->pid = (pid_t)lval;
    if (extract_string(buf, "started_at",  out->started_at,   sizeof(out->started_at)) < 0) return -1;
    if (extract_string(buf, "command",     out->command,      sizeof(out->command))    < 0) return -1;

    /* Optional scalar fields — missing is OK, leave default. */
    extract_string(buf, "argv",        out->argv_str,    sizeof(out->argv_str));
    extract_string(buf, "rootfs",      out->rootfs,      sizeof(out->rootfs));
    extract_string(buf, "hostname",    out->hostname,    sizeof(out->hostname));
    extract_string(buf, "cgroup_path", out->cgroup_path, sizeof(out->cgroup_path));

    /* Namespaces block — each is "key": true/false inside "namespaces": {...}. */
    extract_bool(buf, "pid",     &out->enable_pid_namespace);
    extract_bool(buf, "mount",   &out->enable_mount_namespace);
    extract_bool(buf, "uts",     &out->enable_uts_namespace);
    extract_bool(buf, "user",    &out->enable_user_namespace);
    extract_bool(buf, "ipc",     &out->enable_ipc_namespace);
    extract_bool(buf, "network", &out->enable_network);

    /* Veth block — present only when network is active. */
    out->has_veth = false;
    if (find_key(buf, "host") && find_key(buf, "container_ip")) {
        extract_string(buf, "host",         out->veth_host,         sizeof(out->veth_host));
        extract_string(buf, "container",    out->veth_container,    sizeof(out->veth_container));
        extract_string(buf, "host_ip",      out->veth_host_ip,      sizeof(out->veth_host_ip));
        extract_string(buf, "container_ip", out->veth_container_ip, sizeof(out->veth_container_ip));
        extract_string(buf, "netmask",      out->veth_netmask,      sizeof(out->veth_netmask));
        out->has_veth = (out->veth_host[0] != '\0');
    }

    return 0;
}

int state_list(container_info_t *infos, int max_infos) {
    const char *root = state_dir_root();
    DIR *dir = opendir(root);
    if (!dir) {
        if (errno == ENOENT) return 0;   /* No state yet — that's fine. */
        perror("opendir(state_root)");
        return -1;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_infos) {
        if (entry->d_name[0] == '.') continue;
        /* Container IDs are 12 hex chars. */
        if (strlen(entry->d_name) != CONTAINER_ID_LEN) continue;

        container_state_t s = {0};
        if (state_load(entry->d_name, &s) < 0) continue;

        container_info_t *info = &infos[count++];
        strncpy(info->id, s.id, CONTAINER_ID_LEN + 1);
        info->pid = s.pid;
        strncpy(info->started_at, s.started_at, sizeof(info->started_at));
        /* container_info_t.command is intentionally smaller than
         * container_state_t.command (64 vs PATH_MAX) — list output
         * is column-constrained.  Truncate safely. */
        strncpy(info->command, s.command, sizeof(info->command) - 1);
        info->command[sizeof(info->command) - 1] = '\0';
        info->alive = (kill(s.pid, 0) == 0);

        /* Best-effort cgroup stats. memory.current is a single number
         * in bytes; cpu.stat has multiple lines but we read usage_usec.
         * Both are optional — leave 0 on any read failure (including
         * the common case where enable_cgroup was false). */
        info->memory_current = 0;
        info->cpu_pct = 0;
        if (s.cgroup_path[0]) {
            char p[PATH_MAX];
            int pn = snprintf(p, sizeof(p), "%s/memory.current", s.cgroup_path);
            if (pn < 0 || (size_t)pn >= sizeof(p)) continue;
            FILE *mf = fopen(p, "r");
            if (mf) {
                unsigned long long bytes = 0;
                if (fscanf(mf, "%llu", &bytes) == 1) {
                    info->memory_current = (size_t)bytes;
                }
                fclose(mf);
            }
            /* CPU percent is non-trivial: cpu.stat gives total usage,
             * but "percent" requires two samples and a delta. For 7b
             * we report the cumulative usec divided by uptime — a
             * crude lifetime average rather than instantaneous percent.
             * A real implementation would sample twice with a sleep
             * in between. */
            pn = snprintf(p, sizeof(p), "%s/cpu.stat", s.cgroup_path);
            if (pn < 0 || (size_t)pn >= sizeof(p)) continue;
            FILE *cf = fopen(p, "r");
            if (cf) {
                char line[128];
                unsigned long long usage_usec = 0;
                while (fgets(line, sizeof(line), cf)) {
                    if (sscanf(line, "usage_usec %llu", &usage_usec) == 1)
                        break;
                }
                fclose(cf);
                /* Compute lifetime average: usage_usec / wall_usec_since_start. */
                struct timespec now_ts;
                clock_gettime(CLOCK_REALTIME, &now_ts);
                /* Parse started_at (ISO 8601 UTC) back to time_t. */
                struct tm start_tm = {0};
                if (strptime(s.started_at, "%Y-%m-%dT%H:%M:%SZ", &start_tm)) {
                    time_t start_t = timegm(&start_tm);
                    long elapsed_sec = (long)now_ts.tv_sec - (long)start_t;
                    if (elapsed_sec > 0) {
                        unsigned long long elapsed_usec =
                            (unsigned long long)elapsed_sec * 1000000ULL;
                        info->cpu_pct = (int)((usage_usec * 100) / elapsed_usec);
                    }
                }
            }
        }
    }
    closedir(dir);
    return count;
}

int state_remove(const char *id) {
    char dir[PATH_MAX], path[PATH_MAX];
    state_dir_path(id, dir, sizeof(dir));

    /* Best-effort cleanup.  Each snprintf return is checked against the
     * buffer size; on truncation (only possible with a pathologically
     * long state_dir_root()+id, never reached in practice) we skip the
     * unlink/rmdir rather than act on a truncated path.  Silent skip,
     * not warning — state_remove runs on every container exit and the
     * truncation path is unreachable. */
    int n;
    n = snprintf(path, sizeof(path), "%s/state.json", dir);
    if (n > 0 && (size_t)n < sizeof(path)) unlink(path);
    n = snprintf(path, sizeof(path), "%s/pidfile", dir);
    if (n > 0 && (size_t)n < sizeof(path)) unlink(path);
    n = snprintf(path, sizeof(path), "%s/logs/stdout.log", dir);
    if (n > 0 && (size_t)n < sizeof(path)) unlink(path);
    n = snprintf(path, sizeof(path), "%s/logs/stderr.log", dir);
    if (n > 0 && (size_t)n < sizeof(path)) unlink(path);
    n = snprintf(path, sizeof(path), "%s/logs", dir);
    if (n > 0 && (size_t)n < sizeof(path)) rmdir(path);
    rmdir(dir);
    return 0;
}