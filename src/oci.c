/*
 * oci.c — OCI Runtime Specification config.json parser
 *
 * Hand-rolled recursive-descent JSON parser plus consumer logic that
 * walks the parsed tree and populates an oci_config_t. No external
 * JSON library (jansson/cJSON/json-c) — hand-rolled for zero runtime
 * dependencies and educational signal.
 *
 * Scope: parses ~22 of the ~120 OCI fields; silently ignores the rest
 * (counted in ignored_count, logged in debug mode). The parsed-vs-
 * ignored field split is documented on oci_config_t in oci.h.
 */

#include "oci.h"
#include "core.h"          /* for the container_config_t cast in oci_to_container_config */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* ===== Parser State ===== */

typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
    int         depth;
    char        err[256];
} json_state_t;

#define JSON_MAX_DEPTH 16

static void set_err(json_state_t *st, const char *msg)
{
    if (st->err[0] != '\0') return;  /* keep first error */
    snprintf(st->err, sizeof(st->err), "JSON parse error at offset %zu: %s",
             st->pos, msg);
}

static void skip_ws(json_state_t *st)
{
    while (st->pos < st->len && isspace((unsigned char)st->src[st->pos])) {
        st->pos++;
    }
}

static bool match_char(json_state_t *st, char c)
{
    skip_ws(st);
    if (st->pos < st->len && st->src[st->pos] == c) {
        st->pos++;
        return true;
    }
    return false;
}

static bool expect_char(json_state_t *st, char c)
{
    if (!match_char(st, c)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected '%c'", c);
        set_err(st, msg);
        return false;
    }
    return true;
}

/* ===== String parser ===== */

/* Parse a JSON string into the caller's buffer. Truncates if too long
 * (without setting an error — OCI strings shouldn't exceed PATH_MAX
 * but if they do, truncation is gentler than failure). */
static bool parse_string(json_state_t *st, char *out, size_t out_size)
{
    skip_ws(st);
    if (!expect_char(st, '"')) return false;

    size_t out_i = 0;
    while (st->pos < st->len) {
        char c = st->src[st->pos++];
        if (c == '"') {
            if (out && out_size > 0) out[out_i < out_size ? out_i : out_size - 1] = '\0';
            return true;
        }
        if (c == '\\') {
            if (st->pos >= st->len) {
                set_err(st, "unterminated escape");
                return false;
            }
            char esc = st->src[st->pos++];
            char real;
            switch (esc) {
                case '"':  real = '"';  break;
                case '\\': real = '\\'; break;
                case '/':  real = '/';  break;
                case 'n':  real = '\n'; break;
                case 't':  real = '\t'; break;
                case 'r':  real = '\r'; break;
                case 'b':  real = '\b'; break;
                case 'f':  real = '\f'; break;
                case 'u':
                    /* Unicode escapes \uXXXX — we don't support full
                     * UTF-16 decoding; emit '?' for any non-ASCII.
                     * Real OCI bundles don't use these in paths. */
                    if (st->pos + 4 > st->len) {
                        set_err(st, "truncated \\u escape");
                        return false;
                    }
                    st->pos += 4;
                    real = '?';
                    break;
                default:
                    set_err(st, "unknown escape");
                    return false;
            }
            if (out && out_i + 1 < out_size) out[out_i++] = real;
            continue;
        }
        if (out && out_i + 1 < out_size) out[out_i++] = c;
    }
    set_err(st, "unterminated string");
    return false;
}

/* Skip a string without copying — used when we need to consume a value
 * we don't care about. */
static bool skip_string(json_state_t *st)
{
    return parse_string(st, NULL, 0);
}

/* ===== Number / bool / null parsers ===== */

static bool parse_int64(json_state_t *st, int64_t *out)
{
    skip_ws(st);
    char *endp;
    long long v = strtoll(st->src + st->pos, &endp, 10);
    if (endp == st->src + st->pos) {
        set_err(st, "expected number");
        return false;
    }
    st->pos = endp - st->src;
    if (out) *out = (int64_t)v;
    return true;
}

static bool parse_uint64(json_state_t *st, uint64_t *out)
{
    skip_ws(st);
    char *endp;
    unsigned long long v = strtoull(st->src + st->pos, &endp, 10);
    if (endp == st->src + st->pos) {
        set_err(st, "expected number");
        return false;
    }
    st->pos = endp - st->src;
    if (out) *out = (uint64_t)v;
    return true;
}

static bool parse_uint32(json_state_t *st, uint32_t *out)
{
    uint64_t v;
    if (!parse_uint64(st, &v)) return false;
    if (out) *out = (uint32_t)v;
    return true;
}

static bool parse_bool(json_state_t *st, bool *out)
{
    skip_ws(st);
    if (st->pos + 4 <= st->len && strncmp(st->src + st->pos, "true", 4) == 0) {
        st->pos += 4;
        if (out) *out = true;
        return true;
    }
    if (st->pos + 5 <= st->len && strncmp(st->src + st->pos, "false", 5) == 0) {
        st->pos += 5;
        if (out) *out = false;
        return true;
    }
    set_err(st, "expected true/false");
    return false;
}

/* ===== Forward declarations for the recursive descent ===== */

static bool skip_value(json_state_t *st);

/* ===== Skip an arbitrary JSON value (used for fields we don't care about) ===== */

static bool skip_object(json_state_t *st)
{
    if (st->depth >= JSON_MAX_DEPTH) {
        set_err(st, "JSON too deeply nested");
        return false;
    }
    st->depth++;
    if (!expect_char(st, '{')) { st->depth--; return false; }
    skip_ws(st);
    if (match_char(st, '}')) { st->depth--; return true; }
    while (1) {
        if (!skip_string(st)) { st->depth--; return false; }
        if (!expect_char(st, ':')) { st->depth--; return false; }
        if (!skip_value(st)) { st->depth--; return false; }
        if (match_char(st, ',')) continue;
        if (match_char(st, '}')) { st->depth--; return true; }
        set_err(st, "expected ',' or '}'");
        st->depth--;
        return false;
    }
}

static bool skip_array(json_state_t *st)
{
    if (st->depth >= JSON_MAX_DEPTH) {
        set_err(st, "JSON too deeply nested");
        return false;
    }
    st->depth++;
    if (!expect_char(st, '[')) { st->depth--; return false; }
    skip_ws(st);
    if (match_char(st, ']')) { st->depth--; return true; }
    while (1) {
        if (!skip_value(st)) { st->depth--; return false; }
        if (match_char(st, ',')) continue;
        if (match_char(st, ']')) { st->depth--; return true; }
        set_err(st, "expected ',' or ']'");
        st->depth--;
        return false;
    }
}

static bool skip_value(json_state_t *st)
{
    skip_ws(st);
    if (st->pos >= st->len) {
        set_err(st, "unexpected end of input");
        return false;
    }
    char c = st->src[st->pos];
    if (c == '"') return skip_string(st);
    if (c == '{') return skip_object(st);
    if (c == '[') return skip_array(st);
    if (c == 't' || c == 'f') return parse_bool(st, NULL);
    if (c == 'n') {
        if (st->pos + 4 <= st->len && strncmp(st->src + st->pos, "null", 4) == 0) {
            st->pos += 4;
            return true;
        }
        set_err(st, "expected null");
        return false;
    }
    if (c == '-' || isdigit((unsigned char)c)) return parse_int64(st, NULL);
    set_err(st, "unexpected character");
    return false;
}

/* ===== Helper: parse an array of strings into a fixed-size buffer ===== */

static bool parse_string_array(json_state_t *st,
                                char (*out)[OCI_MAX_PATH], int max_count,
                                int *out_count)
{
    if (!expect_char(st, '[')) return false;
    skip_ws(st);
    *out_count = 0;
    if (match_char(st, ']')) return true;
    while (1) {
        if (*out_count >= max_count) {
            /* Silently drop extras — log in debug mode at higher layer */
            if (!skip_string(st)) return false;
        } else {
            if (!parse_string(st, out[*out_count], OCI_MAX_PATH)) return false;
            (*out_count)++;
        }
        if (match_char(st, ',')) continue;
        if (match_char(st, ']')) return true;
        set_err(st, "expected ',' or ']' in string array");
        return false;
    }
}

/* ===== OCI-specific object parsers ===== */

static bool parse_process_user(json_state_t *st, oci_config_t *cfg)
{
    if (!expect_char(st, '{')) return false;
    while (!match_char(st, '}')) {
        char key[64] = {0};
        if (!parse_string(st, key, sizeof(key))) return false;
        if (!expect_char(st, ':')) return false;

        if (strcmp(key, "uid") == 0) {
            if (!parse_uint32(st, &cfg->proc_uid)) return false;
        } else if (strcmp(key, "gid") == 0) {
            if (!parse_uint32(st, &cfg->proc_gid)) return false;
        } else {
            /* additionalGids, umask, etc. — ignore */
            if (!skip_value(st)) return false;
            cfg->ignored_count++;
        }

        if (!match_char(st, ',')) {
            if (!expect_char(st, '}')) return false;
            return true;
        }
    }
    return true;
}

static bool parse_process(json_state_t *st, oci_config_t *cfg)
{
    if (!expect_char(st, '{')) return false;
    while (!match_char(st, '}')) {
        char key[64] = {0};
        if (!parse_string(st, key, sizeof(key))) return false;
        if (!expect_char(st, ':')) return false;

        if (strcmp(key, "args") == 0) {
            if (!parse_string_array(st, cfg->proc_args, OCI_MAX_ARGS,
                                     &cfg->proc_argc)) return false;
        } else if (strcmp(key, "env") == 0) {
            if (!parse_string_array(st, cfg->proc_env, OCI_MAX_ENV,
                                     &cfg->proc_env_count)) return false;
        } else if (strcmp(key, "cwd") == 0) {
            if (!parse_string(st, cfg->proc_cwd, sizeof(cfg->proc_cwd))) return false;
        } else if (strcmp(key, "user") == 0) {
            if (!parse_process_user(st, cfg)) return false;
        } else if (strcmp(key, "terminal") == 0) {
            if (!parse_bool(st, &cfg->proc_terminal)) return false;
        } else {
            /* capabilities, rlimits, noNewPrivileges, oomScoreAdj,
             * selinuxLabel, apparmorProfile, etc. — silently ignored */
            if (!skip_value(st)) return false;
            cfg->ignored_count++;
        }

        if (!match_char(st, ',')) {
            if (!expect_char(st, '}')) return false;
            return true;
        }
    }
    return true;
}

static bool parse_root(json_state_t *st, oci_config_t *cfg)
{
    if (!expect_char(st, '{')) return false;
    while (!match_char(st, '}')) {
        char key[64] = {0};
        if (!parse_string(st, key, sizeof(key))) return false;
        if (!expect_char(st, ':')) return false;

        if (strcmp(key, "path") == 0) {
            if (!parse_string(st, cfg->root_path, sizeof(cfg->root_path))) return false;
        } else if (strcmp(key, "readonly") == 0) {
            if (!parse_bool(st, &cfg->root_readonly)) return false;
        } else {
            if (!skip_value(st)) return false;
            cfg->ignored_count++;
        }

        if (!match_char(st, ',')) {
            if (!expect_char(st, '}')) return false;
            return true;
        }
    }
    return true;
}

static bool parse_mount_obj(json_state_t *st, oci_mount_t *m)
{
    if (!expect_char(st, '{')) return false;
    while (!match_char(st, '}')) {
        char key[64] = {0};
        if (!parse_string(st, key, sizeof(key))) return false;
        if (!expect_char(st, ':')) return false;

        if (strcmp(key, "destination") == 0) {
            if (!parse_string(st, m->destination, sizeof(m->destination))) return false;
        } else if (strcmp(key, "source") == 0) {
            if (!parse_string(st, m->source, sizeof(m->source))) return false;
        } else if (strcmp(key, "type") == 0) {
            if (!parse_string(st, m->type, sizeof(m->type))) return false;
        } else if (strcmp(key, "options") == 0) {
            /* options is an array of strings — join with commas into
             * the options field.  static, not stack-local: the array
             * is OCI_MAX_MOUNTS × OCI_MAX_PATH = 128 KB, and
             * parse_string_array's signature fixes the row size at
             * OCI_MAX_PATH.  Parsing is single-threaded and runs
             * parent-side (pre-clone), so one static scratch buffer
             * is safe and keeps it off the stack. */
            static char tmp_opts[OCI_MAX_MOUNTS][OCI_MAX_PATH];
            int  tmp_count = 0;
            if (!parse_string_array(st, tmp_opts, OCI_MAX_MOUNTS, &tmp_count)) return false;
            m->options[0] = '\0';
            for (int i = 0; i < tmp_count; i++) {
                if (i > 0) strncat(m->options, ",", sizeof(m->options) - strlen(m->options) - 1);
                strncat(m->options, tmp_opts[i],
                        sizeof(m->options) - strlen(m->options) - 1);
            }
        } else {
            if (!skip_value(st)) return false;
        }

        if (!match_char(st, ',')) {
            if (!expect_char(st, '}')) return false;
            return true;
        }
    }
    return true;
}

static bool parse_mounts_array(json_state_t *st, oci_config_t *cfg)
{
    if (!expect_char(st, '[')) return false;
    if (match_char(st, ']')) return true;
    while (1) {
        if (cfg->mount_count >= OCI_MAX_MOUNTS) {
            oci_mount_t throwaway = {0};
            if (!parse_mount_obj(st, &throwaway)) return false;
            cfg->ignored_count++;
        } else {
            if (!parse_mount_obj(st, &cfg->mounts[cfg->mount_count])) return false;
            cfg->mount_count++;
        }
        if (match_char(st, ',')) continue;
        if (match_char(st, ']')) return true;
        set_err(st, "expected ',' or ']' in mounts array");
        return false;
    }
}

static bool parse_namespaces_array(json_state_t *st, oci_config_t *cfg)
{
    if (!expect_char(st, '[')) return false;
    if (match_char(st, ']')) return true;
    while (1) {
        /* Each entry is {"type": "<name>", optional "path": "..."} */
        if (!expect_char(st, '{')) return false;
        char ns_type[32] = {0};
        while (!match_char(st, '}')) {
            char key[64] = {0};
            if (!parse_string(st, key, sizeof(key))) return false;
            if (!expect_char(st, ':')) return false;
            if (strcmp(key, "type") == 0) {
                if (!parse_string(st, ns_type, sizeof(ns_type))) return false;
            } else {
                /* path (joining existing ns) — out of scope */
                if (!skip_value(st)) return false;
                cfg->ignored_count++;
            }
            if (!match_char(st, ',')) {
                if (!expect_char(st, '}')) return false;
                break;
            }
        }

        if      (strcmp(ns_type, "pid")     == 0) cfg->ns_pid     = true;
        else if (strcmp(ns_type, "mount")   == 0) cfg->ns_mount   = true;
        else if (strcmp(ns_type, "uts")     == 0) cfg->ns_uts     = true;
        else if (strcmp(ns_type, "user")    == 0) cfg->ns_user    = true;
        else if (strcmp(ns_type, "ipc")     == 0) cfg->ns_ipc     = true;
        else if (strcmp(ns_type, "network") == 0) cfg->ns_network = true;
        else if (strcmp(ns_type, "cgroup")  == 0) cfg->ns_cgroup  = true;
        /* unknown namespaces silently ignored */

        if (match_char(st, ',')) continue;
        if (match_char(st, ']')) return true;
        set_err(st, "expected ',' or ']' in namespaces array");
        return false;
    }
}

static bool parse_uid_or_gid_mappings(json_state_t *st, bool is_gid, oci_config_t *cfg)
{
    if (!expect_char(st, '[')) return false;
    if (match_char(st, ']')) return true;

    /* We only capture the FIRST mapping. Subsequent entries are skipped. */
    bool captured = false;
    while (1) {
        uint32_t container_id = 0, host_id = 0, size = 1;
        if (!expect_char(st, '{')) return false;
        while (!match_char(st, '}')) {
            char key[64] = {0};
            if (!parse_string(st, key, sizeof(key))) return false;
            if (!expect_char(st, ':')) return false;
            if (strcmp(key, "containerID") == 0) {
                if (!parse_uint32(st, &container_id)) return false;
            } else if (strcmp(key, "hostID") == 0) {
                if (!parse_uint32(st, &host_id)) return false;
            } else if (strcmp(key, "size") == 0) {
                if (!parse_uint32(st, &size)) return false;
            } else {
                if (!skip_value(st)) return false;
                cfg->ignored_count++;
            }
            if (!match_char(st, ',')) {
                if (!expect_char(st, '}')) return false;
                break;
            }
        }

        if (!captured) {
            if (is_gid) {
                cfg->has_gid_mapping = true;
                cfg->gid_container   = container_id;
                cfg->gid_host        = host_id;
                cfg->gid_size        = size;
            } else {
                cfg->has_uid_mapping = true;
                cfg->uid_container   = container_id;
                cfg->uid_host        = host_id;
                cfg->uid_size        = size;
            }
            captured = true;
        } else {
            cfg->ignored_count++;
        }

        if (match_char(st, ',')) continue;
        if (match_char(st, ']')) return true;
        set_err(st, "expected ',' or ']' in mappings");
        return false;
    }
}

static bool parse_resources(json_state_t *st, oci_config_t *cfg)
{
    if (!expect_char(st, '{')) return false;
    while (!match_char(st, '}')) {
        char key[64] = {0};
        if (!parse_string(st, key, sizeof(key))) return false;
        if (!expect_char(st, ':')) return false;

        if (strcmp(key, "memory") == 0) {
            if (!expect_char(st, '{')) return false;
            while (!match_char(st, '}')) {
                char k2[64] = {0};
                if (!parse_string(st, k2, sizeof(k2))) return false;
                if (!expect_char(st, ':')) return false;
                if (strcmp(k2, "limit") == 0) {
                    if (!parse_uint64(st, &cfg->memory_limit_bytes)) return false;
                    cfg->has_memory_limit = true;
                } else {
                    if (!skip_value(st)) return false;
                    cfg->ignored_count++;
                }
                if (!match_char(st, ',')) {
                    if (!expect_char(st, '}')) return false;
                    break;
                }
            }
        } else if (strcmp(key, "cpu") == 0) {
            if (!expect_char(st, '{')) return false;
            cfg->cpu_period = 100000; /* default OCI period */
            while (!match_char(st, '}')) {
                char k2[64] = {0};
                if (!parse_string(st, k2, sizeof(k2))) return false;
                if (!expect_char(st, ':')) return false;
                if (strcmp(k2, "quota") == 0) {
                    if (!parse_int64(st, &cfg->cpu_quota)) return false;
                    cfg->has_cpu_quota = true;
                } else if (strcmp(k2, "period") == 0) {
                    if (!parse_int64(st, &cfg->cpu_period)) return false;
                } else {
                    if (!skip_value(st)) return false;
                    cfg->ignored_count++;
                }
                if (!match_char(st, ',')) {
                    if (!expect_char(st, '}')) return false;
                    break;
                }
            }
        } else if (strcmp(key, "pids") == 0) {
            if (!expect_char(st, '{')) return false;
            while (!match_char(st, '}')) {
                char k2[64] = {0};
                if (!parse_string(st, k2, sizeof(k2))) return false;
                if (!expect_char(st, ':')) return false;
                if (strcmp(k2, "limit") == 0) {
                    if (!parse_int64(st, &cfg->pids_limit)) return false;
                    cfg->has_pids_limit = true;
                } else {
                    if (!skip_value(st)) return false;
                    cfg->ignored_count++;
                }
                if (!match_char(st, ',')) {
                    if (!expect_char(st, '}')) return false;
                    break;
                }
            }
        } else {
            /* devices, blockIO, hugepageLimits, network — ignored */
            if (!skip_value(st)) return false;
            cfg->ignored_count++;
        }

        if (!match_char(st, ',')) {
            if (!expect_char(st, '}')) return false;
            return true;
        }
    }
    return true;
}

static bool parse_linux(json_state_t *st, oci_config_t *cfg)
{
    if (!expect_char(st, '{')) return false;
    while (!match_char(st, '}')) {
        char key[64] = {0};
        if (!parse_string(st, key, sizeof(key))) return false;
        if (!expect_char(st, ':')) return false;

        if (strcmp(key, "namespaces") == 0) {
            if (!parse_namespaces_array(st, cfg)) return false;
        } else if (strcmp(key, "uidMappings") == 0) {
            if (!parse_uid_or_gid_mappings(st, false, cfg)) return false;
        } else if (strcmp(key, "gidMappings") == 0) {
            if (!parse_uid_or_gid_mappings(st, true, cfg)) return false;
        } else if (strcmp(key, "resources") == 0) {
            if (!parse_resources(st, cfg)) return false;
        } else if (strcmp(key, "maskedPaths") == 0) {
            if (!parse_string_array(st, cfg->masked_paths, OCI_MAX_MASKED,
                                     &cfg->masked_count)) return false;
        } else if (strcmp(key, "readonlyPaths") == 0) {
            if (!parse_string_array(st, cfg->readonly_paths, OCI_MAX_READONLY,
                                     &cfg->readonly_count)) return false;
        } else {
            /* seccomp, devices, cgroupsPath, rootfsPropagation,
             * intelRdt, etc. — silently ignored */
            if (!skip_value(st)) return false;
            cfg->ignored_count++;
        }

        if (!match_char(st, ',')) {
            if (!expect_char(st, '}')) return false;
            return true;
        }
    }
    return true;
}

/* ===== Top-level parser ===== */

static bool parse_root_object(json_state_t *st, oci_config_t *cfg)
{
    if (!expect_char(st, '{')) return false;
    while (!match_char(st, '}')) {
        char key[64] = {0};
        if (!parse_string(st, key, sizeof(key))) return false;
        if (!expect_char(st, ':')) return false;

        if (strcmp(key, "ociVersion") == 0) {
            if (!parse_string(st, cfg->oci_version, sizeof(cfg->oci_version))) return false;
        } else if (strcmp(key, "hostname") == 0) {
            if (!parse_string(st, cfg->hostname, sizeof(cfg->hostname))) return false;
        } else if (strcmp(key, "process") == 0) {
            if (!parse_process(st, cfg)) return false;
        } else if (strcmp(key, "root") == 0) {
            if (!parse_root(st, cfg)) return false;
        } else if (strcmp(key, "mounts") == 0) {
            if (!parse_mounts_array(st, cfg)) return false;
        } else if (strcmp(key, "linux") == 0) {
            if (!parse_linux(st, cfg)) return false;
        } else {
            /* hooks, annotations, solaris, windows, vm — silently ignored */
            if (!skip_value(st)) return false;
            cfg->ignored_count++;
        }

        if (!match_char(st, ',')) {
            if (!expect_char(st, '}')) return false;
            return true;
        }
    }
    return true;
}

/* ===== Public API ===== */

int oci_parse_config(const char *config_path,
                     oci_config_t *out_cfg,
                     char *out_err, size_t err_size)
{
    if (!config_path || !out_cfg) {
        errno = EINVAL;
        return -1;
    }

    /* Read file into memory. */
    int fd = open(config_path, O_RDONLY);
    if (fd < 0) {
        if (out_err) snprintf(out_err, err_size, "open(%s): %s",
                              config_path, strerror(errno));
        return -1;
    }
    struct stat sb;
    if (fstat(fd, &sb) < 0) {
        close(fd);
        if (out_err) snprintf(out_err, err_size, "fstat(%s): %s",
                              config_path, strerror(errno));
        return -1;
    }
    if (sb.st_size > 1024 * 1024) {
        close(fd);
        if (out_err) snprintf(out_err, err_size,
                              "config.json larger than 1 MB (likely malformed)");
        errno = EFBIG;
        return -1;
    }
    char *buf = malloc(sb.st_size + 1);
    if (!buf) {
        close(fd);
        if (out_err) snprintf(out_err, err_size, "malloc: %s", strerror(errno));
        return -1;
    }
    ssize_t n = read(fd, buf, sb.st_size);
    close(fd);
    if (n != sb.st_size) {
        free(buf);
        if (out_err) snprintf(out_err, err_size,
                              "short read on config.json (%zd vs %lld)",
                              n, (long long)sb.st_size);
        errno = EIO;
        return -1;
    }
    buf[sb.st_size] = '\0';

    /* Initialize parser state and output config. */
    memset(out_cfg, 0, sizeof(*out_cfg));
    json_state_t st = { .src = buf, .len = (size_t)sb.st_size, .pos = 0, .depth = 0 };
    bool ok = parse_root_object(&st, out_cfg);

    free(buf);

    if (!ok) {
        if (out_err) snprintf(out_err, err_size, "%s", st.err);
        errno = EINVAL;
        return -1;
    }

    /* Validate ociVersion. */
    if (out_cfg->oci_version[0] != '1' || out_cfg->oci_version[1] != '.') {
        if (out_err) snprintf(out_err, err_size,
                              "unsupported ociVersion: %s (expected 1.x.y)",
                              out_cfg->oci_version);
        errno = ENOTSUP;
        return -1;
    }

    return 0;
}

/* ===== Translation to container_config_t ===== */

int oci_to_container_config(const char *bundle_dir,
                            const oci_config_t *oci_cfg,
                            void *cfg_void)
{
    container_config_t *cfg = (container_config_t *)cfg_void;
    if (!bundle_dir || !oci_cfg || !cfg) {
        errno = EINVAL;
        return -1;
    }

    /* Bundle path itself — used by state.c to emit oci-state.json. */
    strncpy(cfg->bundle_path, bundle_dir, sizeof(cfg->bundle_path) - 1);

    /* process.args → program + argv (caller is expected to copy into
     * an argv array of char*; for now, just stash args[0] in program). */
    if (oci_cfg->proc_argc == 0) {
        fprintf(stderr, "OCI: process.args is empty\n");
        return -1;
    }
    /* The caller (cli.c) builds the actual argv from oci_cfg->proc_args;
     * we don't allocate here. cli.c does:
     *   for (int i = 0; i < oci_cfg->proc_argc; i++)
     *       custom_argv[i] = oci_cfg->proc_args[i];
     *   cfg.program = custom_argv[0];
     *   cfg.argv    = custom_argv;
     * using file-scope static storage so the pointers outlive the parse. */

    /* process.env → caller composes into a char ** for build_container_env. */

    /* process.cwd — default "/" if not set. */
    if (oci_cfg->proc_cwd[0] != '\0') {
        strncpy(cfg->cwd, oci_cfg->proc_cwd, sizeof(cfg->cwd) - 1);
    } else {
        strncpy(cfg->cwd, "/", sizeof(cfg->cwd) - 1);
    }

    /* process.terminal → enable_pty (Phase 7b name). */
    cfg->enable_pty = oci_cfg->proc_terminal;

    /* root.path — resolve relative to bundle_dir.
     *
     * cfg->rootfs_path and cfg->hostname are `const char *` POINTERS
     * in the shipped container_config_t (they normally alias
     * argv/optarg storage) — NOT char arrays.  You cannot strncpy into
     * them; the resolved path needs storage that outlives this
     * function and parse_run_flags, all the way into the cloned child.
     * Same static-storage pattern as cli.c's oci_argv: file-scope
     * lifetime, strictly single-use per process (the parse is
     * one-shot). */
    static char resolved_rootfs[PATH_MAX];
    if (oci_cfg->root_path[0] == '/') {
        snprintf(resolved_rootfs, sizeof(resolved_rootfs), "%s",
                 oci_cfg->root_path);
    } else {
        int rn = snprintf(resolved_rootfs, sizeof(resolved_rootfs),
                          "%s/%s", bundle_dir, oci_cfg->root_path);
        if (rn < 0 || (size_t)rn >= sizeof(resolved_rootfs)) {
            fprintf(stderr, "OCI: bundle rootfs path too long\n");
            return -1;
        }
    }
    cfg->rootfs_path = resolved_rootfs;
    cfg->rootfs_readonly = oci_cfg->root_readonly;

    /* hostname → UTS.  Point at the parsed config's own storage —
     * oci_cfg is cli.c's file-scope static oci_cfg_storage, so the
     * pointer stays valid through container_start and the clone. */
    if (oci_cfg->hostname[0] != '\0') {
        cfg->hostname = oci_cfg->hostname;
    }

    /* mounts → split by type (decisions.md Error #22):
     *   - "bind" (or untyped) entries have a HOST source that is
     *     unreachable after pivot_root, so they are converted to
     *     bind_mount_t and appended to cfg->mounts[] — the same
     *     pre-pivot path --volume uses inside setup_rootfs.
     *   - everything else (proc/tmpfs/sysfs/devpts/...) goes to
     *     cfg->oci_mounts[] for the post-pivot pass in child_func.
     *
     * SECURITY: bind sources come from config.json — the BUNDLE AUTHOR
     * chooses them, not the user (bundles are TRUSTED INPUT).  Resolve each
     * through realpath() (same as parse_volume does for --volume) and
     * warn loudly when a source resolves outside the bundle directory;
     * otherwise running an untrusted bundle as root silently
     * bind-mounts any host path it names into the container. */
    cfg->oci_mount_count = 0;
    for (int i = 0; i < oci_cfg->mount_count; i++) {
        const oci_mount_t *m = &oci_cfg->mounts[i];
        bool is_bind = (strcmp(m->type, "bind") == 0) ||
                       (m->type[0] == '\0');
        if (!is_bind) {
            cfg->oci_mounts[cfg->oci_mount_count++] = *m;
            continue;
        }

        if (cfg->mount_count >= MAX_MOUNTS) {
            fprintf(stderr, "OCI: too many bind mounts (max %d)\n",
                    MAX_MOUNTS);
            return -1;
        }
        bind_mount_t *b = &cfg->mounts[cfg->mount_count];
        if (!realpath(m->source, b->host_path)) {
            fprintf(stderr, "OCI: bind source %s: %s\n",
                    m->source, strerror(errno));
            return -1;
        }
        char bundle_real[PATH_MAX];
        if (realpath(bundle_dir, bundle_real) &&
            strncmp(b->host_path, bundle_real,
                    strlen(bundle_real)) != 0) {
            fprintf(stderr,
                "[oci] WARNING: bundle bind-mounts host path %s "
                "(outside the bundle) into the container\n",
                b->host_path);
        }
        strncpy(b->container_path, m->destination,
                sizeof(b->container_path) - 1);
        b->container_path[sizeof(b->container_path) - 1] = '\0';

        /* options are comma-joined; look for the exact "ro" token
         * (a bare strstr would false-match inside other options). */
        b->readonly = false;
        const char *opt = m->options;
        while (opt && *opt) {
            if (strncmp(opt, "ro", 2) == 0 &&
                (opt[2] == '\0' || opt[2] == ',')) {
                b->readonly = true;
                break;
            }
            opt = strchr(opt, ',');
            if (opt) opt++;
        }
        cfg->mount_count++;
    }

    /* namespaces → enable_* booleans, then enforce the project
     * invariants on the EFFECTIVE config.  These normally live in
     * parse_run_flags's tail, which only sees flag-derived locals, so
     * the bundle path must apply them itself:
     *   - a rootfs REQUIRES the mount namespace (Phase 2) — without
     *     CLONE_NEWNS, setup_rootfs's mount/pivot_root calls would
     *     run against the HOST mount table;
     *   - a rootfs auto-enables the PID namespace (Phase 4b). */
    cfg->enable_pid_namespace     = oci_cfg->ns_pid;
    cfg->enable_uts_namespace     = oci_cfg->ns_uts;
    cfg->enable_user_namespace    = oci_cfg->ns_user;
    cfg->enable_ipc_namespace     = oci_cfg->ns_ipc;
    cfg->enable_network           = oci_cfg->ns_network;
    if (cfg->rootfs_path != NULL) {
        cfg->enable_mount_namespace = true;   /* Phase 2 invariant */
        cfg->enable_pid_namespace   = true;   /* Phase 4b auto-enable */
    }

    /* uidMappings / gidMappings — first entry only.  The shipped
     * container_config_t has FLAT mapping fields (uid_map_inside etc.);
     * user_ns_mapping_t is assembled ad hoc inside container_start. */
    if (oci_cfg->has_uid_mapping) {
        cfg->uid_map_inside  = oci_cfg->uid_container;
        cfg->uid_map_outside = oci_cfg->uid_host;
        cfg->uid_map_range   = oci_cfg->uid_size;
    }
    if (oci_cfg->has_gid_mapping) {
        cfg->gid_map_inside  = oci_cfg->gid_container;
        cfg->gid_map_outside = oci_cfg->gid_host;
        cfg->gid_map_range   = oci_cfg->gid_size;
    }

    /* resources — Phase 5/7a nested struct: cfg->cgroup_limits.* */
    if (oci_cfg->has_memory_limit) {
        cfg->enable_cgroup                  = true;
        cfg->cgroup_limits.memory_limit     = (size_t)oci_cfg->memory_limit_bytes;
    }
    if (oci_cfg->has_cpu_quota) {
        cfg->enable_cgroup                  = true;
        /* OCI period/quota → Phase 5's cpu_quota/cpu_period (microseconds). */
        cfg->cgroup_limits.cpu_quota        = (long)oci_cfg->cpu_quota;
        cfg->cgroup_limits.cpu_period       = (long)oci_cfg->cpu_period;
    }
    if (oci_cfg->has_pids_limit) {
        cfg->enable_cgroup                  = true;
        /* Shipped Phase 5 field name is SINGULAR pid_limit. */
        cfg->cgroup_limits.pid_limit        = (long)oci_cfg->pids_limit;
    }

    /* maskedPaths / readonlyPaths */
    memcpy(cfg->masked_paths, oci_cfg->masked_paths,
           sizeof(oci_cfg->masked_paths[0]) * oci_cfg->masked_count);
    cfg->masked_count = oci_cfg->masked_count;
    memcpy(cfg->readonly_paths, oci_cfg->readonly_paths,
           sizeof(oci_cfg->readonly_paths[0]) * oci_cfg->readonly_count);
    cfg->readonly_count = oci_cfg->readonly_count;

    return 0;
}
