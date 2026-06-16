// Note: _GNU_SOURCE is provided by the Makefile.
#include "cli.h"
#include "core.h"
#include "env.h"
#include "state.h"
#include "cleanup.h"
#include "pty.h"
#include "inspector.h"   /* Phase 8a: show_container_* live reports */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>

subcommand_t parse_subcommand(const char *arg) {
    if (!arg) return SUBCMD_UNKNOWN;
    if (strcmp(arg, "run") == 0)     return SUBCMD_RUN;
    if (strcmp(arg, "start") == 0)   return SUBCMD_START;
    if (strcmp(arg, "stop") == 0)    return SUBCMD_STOP;
    if (strcmp(arg, "exec") == 0)    return SUBCMD_EXEC;
    if (strcmp(arg, "inspect") == 0) return SUBCMD_INSPECT;
    if (strcmp(arg, "list") == 0)    return SUBCMD_LIST;
    if (strcmp(arg, "cleanup") == 0) return SUBCMD_CLEANUP;
    if (strcmp(arg, "stats") == 0)   return SUBCMD_STATS;    /* NEW in 8a */
    if (strcmp(arg, "top") == 0)     return SUBCMD_TOP;      /* NEW in 8a */
    if (strcmp(arg, "netstat") == 0) return SUBCMD_NETSTAT;  /* NEW in 8a */
    return SUBCMD_UNKNOWN;
}

/* Parse "--volume HOST:CONTAINER[:ro]" into a bind_mount_t. */
static int parse_volume(const char *spec, bind_mount_t *out) {
    char copy[PATH_MAX * 2 + 8];
    strncpy(copy, spec, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char *colon = strchr(copy, ':');
    if (!colon) {
        fprintf(stderr, "Invalid --volume: missing ':' in %s\n", spec);
        return -1;
    }
    *colon = '\0';
    char *host = copy;
    char *container = colon + 1;

    char *colon2 = strchr(container, ':');
    bool ro = false;
    if (colon2) {
        *colon2 = '\0';
        if (strcmp(colon2 + 1, "ro") == 0) {
            ro = true;
        } else {
            fprintf(stderr, "Invalid --volume option: %s\n", colon2 + 1);
            return -1;
        }
    }

    /* Resolve host to absolute path. */
    if (!realpath(host, out->host_path)) {
        fprintf(stderr, "Invalid --volume host: %s: %s\n",
                host, strerror(errno));
        return -1;
    }
    strncpy(out->container_path, container,
            sizeof(out->container_path) - 1);
    out->readonly = ro;
    return 0;
}

/* Parse a memory limit string ("100M" / "1G" / "512K"). Carried
 * forward from Phase 5 main.c — unchanged. */
static size_t parse_memory_limit(const char *str) {
    char *endptr;
    double value = strtod(str, &endptr);
    if (*endptr == 'K' || *endptr == 'k') return (size_t)(value * 1024);
    if (*endptr == 'M' || *endptr == 'm') return (size_t)(value * 1024 * 1024);
    if (*endptr == 'G' || *endptr == 'g') return (size_t)(value * 1024 * 1024 * 1024);
    return (size_t)value;
}

static long parse_cpu_limit(const char *str) {
    double value = strtod(str, NULL);
    return (long)(value * 100000);
}

/* Shared by cmd_run and cmd_start. Returns 0 on success, non-zero
 * exit code on parse error. *out_cfg gets fully populated;
 * *out_custom_env is filled with --env values that the caller passes
 * to build_container_env. */
static int parse_run_flags(int argc, char *argv[],
                           container_config_t *out_cfg,
                           char **custom_env, int *env_count,
                           bool *out_detach) {
    /* Reset state — getopt_long is stateful across calls in the
     * same process. */
    optind = 1;

    bool enable_pid_namespace = false;
    bool enable_user_namespace = false;
    bool enable_ipc_namespace = false;
    char *rootfs_path = NULL;
    char *container_dir = NULL;
    char *hostname = NULL;
    bool enable_overlay = false;
    bool enable_debug = false;

    cgroup_limits_t limits = {0};
    bool enable_cgroup = false;

    bool enable_network = false;
    char *net_host_ip = NULL, *net_container_ip = NULL, *net_netmask = NULL;
    bool no_nat = false;

    bool enable_pty = false;
    bool detach = false;

    *env_count = 0;
    out_cfg->mount_count = 0;

    static struct option long_options[] = {
        {"debug",            no_argument,       NULL, 'd'},
        {"pid",              no_argument,       NULL, 'p'},
        {"rootfs",           required_argument, NULL, 'r'},
        {"overlay",          no_argument,       NULL, 'o'},
        {"container-dir",    required_argument, NULL, 'c'},
        {"hostname",         required_argument, NULL, 'H'},
        {"user",             no_argument,       NULL, 'u'},
        {"ipc",              no_argument,       NULL, 'I'},
        {"memory",           required_argument, NULL, 'm'},
        {"cpus",             required_argument, NULL, 'C'},
        {"pids",             required_argument, NULL, 'P'},
        {"net",              no_argument,       NULL, 'N'},
        {"net-host-ip",      required_argument, NULL,  1 },
        {"net-container-ip", required_argument, NULL,  2 },
        {"net-netmask",      required_argument, NULL,  3 },
        {"no-nat",           no_argument,       NULL,  4 },
        {"volume",           required_argument, NULL, 'v'},      /* Phase 7b */
        {"interactive",      no_argument,       NULL, 'i'},      /* Phase 7b */
        {"detach",           no_argument,       NULL, 'D'},      /* Phase 7b */
        {"secure",           no_argument,       NULL, 'S'},      /* Phase 8b */
        {"env",              required_argument, NULL, 'e'},
        {"help",             no_argument,       NULL, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "+dpr:oc:H:uIm:C:P:Nv:iDSe:h",
                              long_options, NULL)) != -1) {
        switch (opt) {
            case 'd': enable_debug = true; break;
            case 'p': enable_pid_namespace = true; break;
            case 'r': rootfs_path = optarg; break;
            case 'o': enable_overlay = true; break;
            case 'c': container_dir = optarg; break;
            case 'H': hostname = optarg; break;
            case 'u': enable_user_namespace = true; break;
            case 'I': enable_ipc_namespace = true; break;
            case 'm':
                limits.memory_limit = parse_memory_limit(optarg);
                enable_cgroup = true;
                break;
            case 'C':
                limits.cpu_quota = parse_cpu_limit(optarg);
                limits.cpu_period = 100000;
                enable_cgroup = true;
                break;
            case 'P':
                limits.pid_limit = atoi(optarg);
                enable_cgroup = true;
                break;
            case 'N': enable_network = true; break;
            case 1:   net_host_ip = optarg; break;
            case 2:   net_container_ip = optarg; break;
            case 3:   net_netmask = optarg; break;
            case 4:   no_nat = true; break;
            case 'v':
                if (out_cfg->mount_count >= MAX_MOUNTS) {
                    fprintf(stderr, "Too many --volume entries (max %d)\n",
                            MAX_MOUNTS);
                    return 1;
                }
                if (parse_volume(optarg,
                        &out_cfg->mounts[out_cfg->mount_count]) < 0) {
                    return 1;
                }
                out_cfg->mount_count++;
                break;
            case 'i': enable_pty = true; break;
            case 'D': detach = true; break;
            case 'S':
                out_cfg->enable_hardening = true;
                break;
            case 'e':
                if (*env_count >= MAX_ENV_ENTRIES - 1) {
                    fprintf(stderr, "Too many --env entries\n");
                    return 1;
                }
                custom_env[(*env_count)++] = optarg;
                break;
            case 'h': cli_usage("minicontainer"); return -1;  /* caller exits 0 */
            default:  return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: No command specified\n");
        return 1;
    }

    /* Phase 3 invariant carried forward. */
    if (enable_overlay && !rootfs_path) {
        fprintf(stderr, "Error: --overlay requires --rootfs\n");
        return 1;
    }

    /* Phase 6 invariant carried forward. */
    if ((net_host_ip || net_container_ip || net_netmask || no_nat)
        && !enable_network) {
        fprintf(stderr, "Error: --net-host-ip / --net-container-ip / "
                        "--net-netmask / --no-nat require --net\n");
        return 1;
    }

    /* Phase 7b invariant: --interactive + --detach is meaningless.
     * Detached container has no terminal to connect a PTY to. */
    if (enable_pty && detach) {
        fprintf(stderr, "Error: --interactive and --detach are mutually exclusive\n");
        return 1;
    }

    // Phase 8b invariant: --secure requires --rootfs.  Use the LOCAL
    // rootfs_path (like the --overlay check above): out_cfg->rootfs_path
    // is not assigned until the config-build block below, so checking it
    // here would always read NULL and reject every --secure run.
    if (out_cfg->enable_hardening && !rootfs_path) {
        fprintf(stderr,
            "Error: --secure requires --rootfs "
            "(hardening only makes sense with its own mount namespace)\n");
        return 1;
    }

    /* Build the config. */
    out_cfg->program = argv[optind];
    out_cfg->argv = &argv[optind];
    out_cfg->enable_debug = enable_debug;
    /* Phase 4b auto-enable: --rootfs implies --pid */
    out_cfg->enable_pid_namespace = enable_pid_namespace || (rootfs_path != NULL);
    out_cfg->enable_mount_namespace = (rootfs_path != NULL);
    out_cfg->enable_uts_namespace = (hostname != NULL);
    out_cfg->enable_user_namespace = enable_user_namespace;
    out_cfg->enable_ipc_namespace = enable_ipc_namespace;
    out_cfg->enable_network = enable_network;
    out_cfg->rootfs_path = rootfs_path;
    out_cfg->enable_overlay = enable_overlay;
    out_cfg->container_dir = container_dir;
    out_cfg->hostname = hostname;
    out_cfg->uid_map_inside = 0;
    out_cfg->uid_map_outside = getuid();
    out_cfg->uid_map_range = 1;
    out_cfg->gid_map_inside = 0;
    out_cfg->gid_map_outside = getgid();
    out_cfg->gid_map_range = 1;
    out_cfg->cgroup_limits = limits;
    out_cfg->enable_cgroup = enable_cgroup;
    out_cfg->veth.enable_nat = !no_nat;
    out_cfg->veth.host_ip[0] = '\0';
    out_cfg->veth.container_ip[0] = '\0';
    out_cfg->veth.netmask[0] = '\0';
    if (enable_network) {
        strncpy(out_cfg->veth.host_ip,
                net_host_ip ? net_host_ip : "10.0.0.1",
                sizeof(out_cfg->veth.host_ip) - 1);
        strncpy(out_cfg->veth.container_ip,
                net_container_ip ? net_container_ip : "10.0.0.2",
                sizeof(out_cfg->veth.container_ip) - 1);
        strncpy(out_cfg->veth.netmask,
                net_netmask ? net_netmask : "24",
                sizeof(out_cfg->veth.netmask) - 1);
    }

    out_cfg->enable_pty = enable_pty;
    out_cfg->stdout_fd = -1;
    out_cfg->stderr_fd = -1;
    out_cfg->detach = detach;

    *out_detach = detach;
    return 0;
}

/* Populate state from a fully-configured container_config_t. PID and
 * cgroup_path are filled in by the caller after container_start. */
static void state_from_config(const container_config_t *cfg,
                              container_state_t *state) {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(state->started_at, sizeof(state->started_at),
             "%Y-%m-%dT%H:%M:%SZ", &tm);
    strncpy(state->command, cfg->program, sizeof(state->command) - 1);

    /* argv_str: space-joined for display. Skip argv[0] since command
     * already shows it. */
    size_t off = 0;
    for (int i = 1; cfg->argv[i] && off < sizeof(state->argv_str) - 1; i++) {
        int n = snprintf(state->argv_str + off,
                         sizeof(state->argv_str) - off,
                         "%s%s", i == 1 ? "" : " ", cfg->argv[i]);
        if (n < 0) break;
        off += (size_t)n;
    }

    state->enable_pid_namespace   = cfg->enable_pid_namespace;
    state->enable_mount_namespace = cfg->enable_mount_namespace;
    state->enable_uts_namespace   = cfg->enable_uts_namespace;
    state->enable_user_namespace  = cfg->enable_user_namespace;
    state->enable_ipc_namespace   = cfg->enable_ipc_namespace;
    state->enable_network         = cfg->enable_network;

    if (cfg->rootfs_path) {
        strncpy(state->rootfs, cfg->rootfs_path,
                sizeof(state->rootfs) - 1);
    }
    if (cfg->hostname) {
        strncpy(state->hostname, cfg->hostname,
                sizeof(state->hostname) - 1);
    }
}

int cmd_run(int argc, char *argv[]) {
    container_config_t cfg = {0};
    char *custom_env[MAX_ENV_ENTRIES];
    memset(custom_env, 0, sizeof(custom_env));
    int env_count = 0;
    bool detach = false;

    int rc = parse_run_flags(argc, argv, &cfg, custom_env,
                             &env_count, &detach);
    if (rc < 0) return 0;  /* --help printed */
    if (rc > 0) return rc; /* parse error */

    /* --detach via cmd_run? Redirect to cmd_start so we get the
     * detached-mode log files etc. */
    if (detach) {
        return cmd_start(argc, argv);
    }

    /* Discoverability hint.  An interactive program run as PID 1 on the
     * host's controlling terminal (no --interactive => no PTY) emits
     * job-control warnings like "Cannot set tty process group" because
     * the terminal's foreground pgid is a host-side PID that doesn't
     * resolve inside the new PID namespace.  When stdin is a terminal
     * but no PTY was requested, point the user at -i.  Gated on stderr
     * also being a terminal so the note never lands in a redirected
     * stderr log, and on stdin being a tty so piped / scripted runs
     * (and headless CI) never see it. */
    if (!cfg.enable_pty && isatty(STDIN_FILENO) && isatty(STDERR_FILENO)) {
        fprintf(stderr,
                "note: stdin is a terminal but no PTY was allocated; "
                "pass --interactive (-i) for a controlling terminal "
                "(avoids 'Cannot set tty process group' warnings)\n");
    }

    /* PTY allocation now happens INSIDE the child after mount_devpts;
     * container_start handles the socketpair handoff and returns the
     * master fd on result.pty_master_fd.  cmd_run just sets the flag. */

    /* Container env. Phase 3 correction carried forward. */
    char **container_env = build_container_env(
        env_count > 0 ? custom_env : NULL, cfg.enable_debug);
    if (!container_env) {
        return 1;
    }
    cfg.envp = container_env;

    /* Allocate the container ID and partial state.  The ID is the
     * canonical container_id: it must be copied into cfg so overlay,
     * cgroup, and veth naming all agree on it.  (Without this,
     * setup_overlay aborts with "container_id is required" — Error #24.) */
    container_state_t state = {0};
    if (state_claim_id(state.id) < 0) {
        fprintf(stderr, "Failed to claim container ID: %s\n", strerror(errno));
        free(container_env);
        return 1;
    }
    strncpy(cfg.container_id, state.id, sizeof(cfg.container_id) - 1);
    cfg.container_id[sizeof(cfg.container_id) - 1] = '\0';
    state_from_config(&cfg, &state);

    /* Phase 7b split: container_start does parent-side setup +
     * clone + sync; container_wait does waitpid + teardown.
     * State file write happens between them so `list` can find
     * the container while it's running. */
    container_result_t r = container_start(&cfg);
    if (r.child_pid < 0) {
        fprintf(stderr, "Failed to start container\n");
        /* Release the state dir claimed by state_claim_id; it has no
         * state.json yet, so cleanup's state_load sweep would miss it. */
        state_remove(state.id);
        free(container_env);
        return 1;
    }

    state.pid = r.child_pid;
    if (cfg.enable_cgroup) {
        strncpy(state.cgroup_path, r.ctx.cgroup_ctx.cgroup_path,
                sizeof(state.cgroup_path) - 1);
    }
    if (cfg.enable_network) {
        state.has_veth = true;
        strncpy(state.veth_host,         r.ctx.net_ctx.veth_host,
                sizeof(state.veth_host) - 1);
        strncpy(state.veth_container,    r.ctx.net_ctx.veth_container,
                sizeof(state.veth_container) - 1);
        strncpy(state.veth_host_ip,      cfg.veth.host_ip,
                sizeof(state.veth_host_ip) - 1);
        strncpy(state.veth_container_ip, cfg.veth.container_ip,
                sizeof(state.veth_container_ip) - 1);
        strncpy(state.veth_netmask,      cfg.veth.netmask,
                sizeof(state.veth_netmask) - 1);
    }
    state_save(&state);

    /* Interactive mode: forward stdio between user's terminal and
     * the PTY master while the child runs. pty_forward blocks until
     * the master EOFs (child exited).  Master fd was received from
     * the child via SCM_RIGHTS during container_start. */
    if (cfg.enable_pty && r.pty_master_fd >= 0) {
        pty_forward(r.pty_master_fd);
        close(r.pty_master_fd);
        r.pty_master_fd = -1;
    }

    /* Wait for the child + teardown overlay. */
    container_wait(&r, cfg.enable_debug);

    /* Clean exit: remove the state file, then free everything. */
    state_remove(state.id);
    container_cleanup(&r);
    free(container_env);

    if (!r.exited_normally) {
        fprintf(stderr, "Container killed by signal %d\n", r.signal);
        return 128 + r.signal;
    }
    return r.exit_status;
}

int cmd_list(int argc, char *argv[]) {
    (void)argc; (void)argv;

    container_info_t infos[MAX_CONTAINERS];
    int n = state_list(infos, MAX_CONTAINERS);
    if (n < 0) {
        fprintf(stderr, "Failed to list containers\n");
        return 1;
    }

    printf("CONTAINER ID  PID     STARTED              COMMAND\n");
    for (int i = 0; i < n; i++) {
        if (!infos[i].alive) continue;
        printf("%-13s %-7d %-20s %s\n",
               infos[i].id, infos[i].pid,
               infos[i].started_at, infos[i].command);
    }
    return 0;
}

int cmd_stop(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: minicontainer stop <id>\n");
        return 1;
    }
    const char *id = argv[1];

    container_state_t s = {0};
    if (state_load(id, &s) < 0) {
        fprintf(stderr, "No such container: %s\n", id);
        return 1;
    }

    if (kill(s.pid, 0) != 0) {
        fprintf(stderr, "Container %s is not running (PID %d)\n",
                id, s.pid);
        state_remove(id);   /* opportunistic cleanup */
        return 1;
    }

    /* SIGTERM first, then SIGKILL after 10s. */
    if (kill(s.pid, SIGTERM) < 0) {
        perror("kill(SIGTERM)");
        return 1;
    }

    for (int i = 0; i < 100; i++) {  /* 100 * 100ms = 10s */
        if (kill(s.pid, 0) != 0) break;
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }

    if (kill(s.pid, 0) == 0) {
        fprintf(stderr, "Container %s did not stop on SIGTERM, sending SIGKILL\n", id);
        kill(s.pid, SIGKILL);
    }

    state_remove(id);
    printf("Stopped %s\n", id);
    return 0;
}

int cmd_start(int argc, char *argv[]) {
    container_config_t cfg = {0};
    char *custom_env[MAX_ENV_ENTRIES];
    memset(custom_env, 0, sizeof(custom_env));
    int env_count = 0;
    bool detach_was_set = false;

    int rc = parse_run_flags(argc, argv, &cfg, custom_env,
                             &env_count, &detach_was_set);
    if (rc < 0) return 0;
    if (rc > 0) return rc;

    /* --interactive doesn't make sense in detached mode (no terminal
     * to attach a PTY to). parse_run_flags already rejected this
     * combination, but be defensive. */
    if (cfg.enable_pty) {
        fprintf(stderr, "Error: --interactive is incompatible with start (detached)\n");
        return 1;
    }
    cfg.detach = true;

    /* Container env. */
    char **container_env = build_container_env(
        env_count > 0 ? custom_env : NULL, cfg.enable_debug);
    if (!container_env) return 1;
    cfg.envp = container_env;

    /* Allocate ID and prepare state.  Copy the canonical ID into cfg so
     * overlay/cgroup/veth naming agree (Error #24 — setup_overlay needs
     * cfg.container_id). */
    container_state_t state = {0};
    if (state_claim_id(state.id) < 0) {
        fprintf(stderr, "Failed to claim container ID: %s\n", strerror(errno));
        free(container_env);
        return 1;
    }
    strncpy(cfg.container_id, state.id, sizeof(cfg.container_id) - 1);
    cfg.container_id[sizeof(cfg.container_id) - 1] = '\0';
    state_from_config(&cfg, &state);

    /* Open log files for the detached child's stdout/stderr. We
     * mkdir the state dir first because the log paths are under
     * <state_dir>/logs/.  GCC -Wformat-truncation: explicit return-
     * value checks below treat truncation (only possible with a
     * pathologically long state_dir_root() + id) as a hard failure
     * rather than silently writing to a truncated path. */
    char state_dir[PATH_MAX], logs_dir[PATH_MAX];
    char stdout_path[PATH_MAX], stderr_path[PATH_MAX];
    state_dir_path(state.id, state_dir, sizeof(state_dir));
    int pn;
    pn = snprintf(logs_dir, sizeof(logs_dir), "%s/logs", state_dir);
    if (pn < 0 || (size_t)pn >= sizeof(logs_dir)) {
        fprintf(stderr, "cmd_start: logs_dir path truncated for id %s\n", state.id);
        state_remove(state.id);   /* release the claimed state dir */
        free(container_env);
        return 1;
    }

    if (mkdir_p(logs_dir, 0755) < 0 && errno != EEXIST) {
        perror("mkdir(logs)");
        state_remove(state.id);   /* release the claimed state dir */
        free(container_env);
        return 1;
    }

    pn = snprintf(stdout_path, sizeof(stdout_path), "%s/stdout.log", logs_dir);
    if (pn < 0 || (size_t)pn >= sizeof(stdout_path)) {
        fprintf(stderr, "cmd_start: stdout_path truncated for id %s\n", state.id);
        state_remove(state.id);   /* release the claimed state dir */
        free(container_env);
        return 1;
    }
    pn = snprintf(stderr_path, sizeof(stderr_path), "%s/stderr.log", logs_dir);
    if (pn < 0 || (size_t)pn >= sizeof(stderr_path)) {
        fprintf(stderr, "cmd_start: stderr_path truncated for id %s\n", state.id);
        state_remove(state.id);   /* release the claimed state dir */
        free(container_env);
        return 1;
    }

    cfg.stdout_fd = open(stdout_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    cfg.stderr_fd = open(stderr_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (cfg.stdout_fd < 0 || cfg.stderr_fd < 0) {
        perror("open(log)");
        if (cfg.stdout_fd >= 0) close(cfg.stdout_fd);
        if (cfg.stderr_fd >= 0) close(cfg.stderr_fd);
        state_remove(state.id);   /* release the claimed state dir */
        free(container_env);
        return 1;
    }

    /* Start the container. NOTE: container_start gives the child its
     * own copy of the fd table via clone, so it's safe to close our
     * copies of the log fds in the parent after start returns. */
    container_result_t r = container_start(&cfg);
    close(cfg.stdout_fd);
    close(cfg.stderr_fd);

    if (r.child_pid < 0) {
        fprintf(stderr, "Failed to start container\n");
        state_remove(state.id);   /* release the claimed state dir */
        free(container_env);
        return 1;
    }

    /* Fill the rest of state. */
    state.pid = r.child_pid;
    if (cfg.enable_cgroup) {
        strncpy(state.cgroup_path, r.ctx.cgroup_ctx.cgroup_path,
                sizeof(state.cgroup_path) - 1);
    }
    if (cfg.enable_network) {
        state.has_veth = true;
        strncpy(state.veth_host,         r.ctx.net_ctx.veth_host,
                sizeof(state.veth_host) - 1);
        strncpy(state.veth_container,    r.ctx.net_ctx.veth_container,
                sizeof(state.veth_container) - 1);
        strncpy(state.veth_host_ip,      cfg.veth.host_ip,
                sizeof(state.veth_host_ip) - 1);
        strncpy(state.veth_container_ip, cfg.veth.container_ip,
                sizeof(state.veth_container_ip) - 1);
        strncpy(state.veth_netmask,      cfg.veth.netmask,
                sizeof(state.veth_netmask) - 1);
    }
    state_save(&state);

    /* Print the container ID and exit. We deliberately do NOT call
     * container_wait or container_cleanup — the child is detached
     * and runs until it exits or `stop`/`cleanup` removes it. The
     * cgroup/veth/state-dir persist until `cleanup` runs. */
    printf("%s\n", state.id);

    free(container_env);
    return 0;
}

/* Argument struct for cmd_exec's cloned child. Kept at file scope
 * (NOT nested) so the project stays portable C — no GCC nested-
 * function extension. */
typedef struct {
    char **argv;
    bool enable_debug;
} exec_args_t;

static int exec_child(void *arg) {
    exec_args_t *a = (exec_args_t *)arg;
    /* No build_container_env here — exec runs *inside* the container's
     * existing env (which was set up by container_start when the
     * container originally launched). For simplicity we reuse the
     * parent's environ. A future enhancement could thread --env flags
     * through cmd_exec to override per-invocation. */
    extern char **environ;
    execve(a->argv[0], a->argv, environ);
    perror("execve");
    return 127;
}

int cmd_exec(int argc, char *argv[]) {
    /* Usage: minicontainer exec [--debug] <id> <command> [args...]
     * Joins the container's existing namespaces via setns and clones
     * a new child with no namespace flags (clone(SIGCHLD, ...)) so
     * the child inherits the joined namespaces. See §8.5. */
    optind = 1;
    bool enable_debug = false;

    static struct option opts[] = {
        {"debug", no_argument, NULL, 'd'},
        {0,0,0,0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "+d", opts, NULL)) != -1) {
        if (opt == 'd') enable_debug = true;
        else return 1;
    }

    if (optind >= argc - 1) {
        fprintf(stderr, "Usage: minicontainer exec [--debug] <id> <command> [args...]\n");
        return 1;
    }
    const char *id = argv[optind++];

    container_state_t s = {0};
    if (state_load(id, &s) < 0) {
        fprintf(stderr, "No such container: %s\n", id);
        return 1;
    }
    if (kill(s.pid, 0) != 0) {
        fprintf(stderr, "Container %s is not running (PID %d gone)\n", id, s.pid);
        return 1;
    }

    /* Open every relevant namespace fd. Order matters for the setns
     * sequence: user namespace first (if active) because joining a
     * user namespace adjusts what capabilities the calling process
     * has for subsequent setns calls. Mount namespace LAST so path
     * resolution for the other ns files (/proc/<pid>/ns/...) still
     * uses the host's mount table. */
    struct ns_entry {
        const char *name;
        int  flag;
    };
    static const struct ns_entry NS[] = {
        { "user", CLONE_NEWUSER },
        { "pid",  CLONE_NEWPID  },
        { "uts",  CLONE_NEWUTS  },
        { "ipc",  CLONE_NEWIPC  },
        { "net",  CLONE_NEWNET  },
        { "mnt",  CLONE_NEWNS   },   /* must be last */
    };
    int nsfds[6];
    int nfds = 0;
    for (unsigned i = 0; i < sizeof(NS)/sizeof(NS[0]); i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/proc/%d/ns/%s", s.pid, NS[i].name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            if (errno == ENOENT) continue;   /* ns not active for this container */
            fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
            for (int j = 0; j < nfds; j++) close(nsfds[j]);
            return 1;
        }
        /* Skip namespaces we're already in.  /proc/<pid>/ns/<type>
         * always exists for every type, so file-presence does NOT mean
         * the container created a distinct namespace — a container
         * started without --user shares our (init) user namespace, and
         * setns(CLONE_NEWUSER) into your own user ns returns EINVAL,
         * which would abort exec for the common case.  Compare the
         * target ns identity (st_dev/st_ino) against our own and skip
         * on a match — joining a namespace you're already in is a
         * no-op at best and EINVAL at worst.  (decisions.md Error #23) */
        struct stat ns_st, self_st;
        char self_path[PATH_MAX];
        snprintf(self_path, sizeof(self_path), "/proc/self/ns/%s", NS[i].name);
        if (fstat(fd, &ns_st) == 0 && stat(self_path, &self_st) == 0 &&
            ns_st.st_dev == self_st.st_dev && ns_st.st_ino == self_st.st_ino) {
            if (enable_debug)
                fprintf(stderr, "[exec] already in %s namespace, skipping\n",
                        NS[i].name);
            close(fd);
            continue;
        }
        if (setns(fd, NS[i].flag) < 0) {
            fprintf(stderr, "setns(%s): %s\n", NS[i].name, strerror(errno));
            close(fd);
            for (int j = 0; j < nfds; j++) close(nsfds[j]);
            return 1;
        }
        nsfds[nfds++] = fd;
        if (enable_debug) fprintf(stderr, "[exec] joined %s namespace\n", NS[i].name);
    }

    /* setns(CLONE_NEWPID) doesn't change our PID namespace — it
     * changes the namespace our children are born into. So we must
     * clone NOW to get a process inside the joined PID ns. Use
     * SIGCHLD only — no CLONE_NEW* flags (those would defeat setns). */
    static char child_stack[1024 * 1024];
    exec_args_t eargs = {
        .argv = &argv[optind],
        .enable_debug = enable_debug
    };

    pid_t pid = clone(exec_child,
                      child_stack + sizeof(child_stack),
                      SIGCHLD,
                      &eargs);
    if (pid < 0) {
        perror("clone");
        for (int j = 0; j < nfds; j++) close(nsfds[j]);
        return 1;
    }

    /* Parent: close ns fds (we no longer need them in this process —
     * the cloned child inherited them then setns'd already), then
     * wait for the child. */
    for (int j = 0; j < nfds; j++) close(nsfds[j]);

    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

/* Phase 8a: replaces Phase 7b's state-only JSON dump. show_container_inspect
 * lives in inspector.c and reads /proc + cgroup data via libprocfs. */
int cmd_inspect(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: minicontainer inspect <id>\n");
        return 1;
    }
    return show_container_inspect(argv[1]) < 0 ? 1 : 0;
}

int cmd_stats(int argc, char *argv[]) {
    bool continuous = false;
    optind = 1;
    static struct option opts[] = {
        {"continuous", no_argument, NULL, 'c'},
        {0,0,0,0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "+c", opts, NULL)) != -1) {
        if (opt == 'c') continuous = true;
        else return 1;
    }
    if (optind >= argc) {
        fprintf(stderr, "Usage: minicontainer stats [-c] <id>\n");
        return 1;
    }
    return show_container_stats(argv[optind], continuous) < 0 ? 1 : 0;
}

int cmd_top(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: minicontainer top <id>\n");
        return 1;
    }
    return show_container_top(argv[1]) < 0 ? 1 : 0;
}

int cmd_netstat(int argc, char *argv[]) {
    bool verbose = false;
    optind = 1;
    static struct option opts[] = {
        {"verbose", no_argument, NULL, 'v'},
        {0,0,0,0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "+v", opts, NULL)) != -1) {
        if (opt == 'v') verbose = true;
        else return 1;
    }
    if (optind >= argc) {
        fprintf(stderr, "Usage: minicontainer netstat [-v] <id>\n");
        return 1;
    }
    return show_container_netstat(argv[optind], verbose) < 0 ? 1 : 0;
}

int cmd_cleanup(int argc, char *argv[]) {
    bool enable_debug = false;
    bool dry_run = false;

    optind = 1;
    static struct option opts[] = {
        {"debug",   no_argument, NULL, 'd'},
        {"dry-run", no_argument, NULL, 'n'},
        {0,0,0,0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "dn", opts, NULL)) != -1) {
        if (opt == 'd')      enable_debug = true;
        else if (opt == 'n') dry_run = true;
        else return 1;
    }

    int n = cleanup_stale_resources(enable_debug, dry_run);
    if (n < 0) return 1;
    printf("%s %d stale resource%s\n",
           dry_run ? "Would clean" : "Cleaned",
           n, n == 1 ? "" : "s");
    return 0;
}

void cli_usage(const char *progname) {
    fprintf(stderr, "Usage: %s <subcommand> [options] [args]\n\n", progname);
    fprintf(stderr, "Subcommands:\n");
    fprintf(stderr, "  run      Run a container in the foreground\n");
    fprintf(stderr, "  start    Run a container detached (background)\n");
    fprintf(stderr, "  stop     Stop a running container by ID\n");
    fprintf(stderr, "  exec     Run a command in an existing container\n");
    fprintf(stderr, "  inspect  Show a container's live /proc + cgroup report by ID\n");
    fprintf(stderr, "  stats    Show live resource usage (-c to refresh each second)\n");
    fprintf(stderr, "  top      Show the container's process tree (via nsenter)\n");
    fprintf(stderr, "  netstat  Show the container's network connections (-v for the table)\n");
    fprintf(stderr, "  list     List running containers\n");
    fprintf(stderr, "  cleanup  Remove stale state from crashed containers\n\n");
    fprintf(stderr, "Run-mode options (run | start):\n");
    fprintf(stderr, "  --debug                  Enable debug output\n");
    fprintf(stderr, "  --pid                    Enable PID namespace\n");
    fprintf(stderr, "  --rootfs <path>          Path to root filesystem\n");
    fprintf(stderr, "  --overlay                Enable copy-on-write overlay\n");
    fprintf(stderr, "  --container-dir <p>      Overlay data directory\n");
    fprintf(stderr, "  --hostname <name>        Set container hostname\n");
    fprintf(stderr, "  --user                   Enable user namespace\n");
    fprintf(stderr, "  --ipc                    Enable IPC namespace\n");
    fprintf(stderr, "  --memory <limit>         Memory cap (e.g., 100M)\n");
    fprintf(stderr, "  --cpus <fraction>        CPU cap (e.g., 0.5)\n");
    fprintf(stderr, "  --pids <max>             Max processes\n");
    fprintf(stderr, "  --net                    Enable network namespace + veth\n");
    fprintf(stderr, "  --net-host-ip <addr>     Host-side veth IP\n");
    fprintf(stderr, "  --net-container-ip <a>   Container-side veth IP\n");
    fprintf(stderr, "  --net-netmask <cidr>     CIDR suffix\n");
    fprintf(stderr, "  --no-nat                 Disable iptables MASQUERADE\n");
    fprintf(stderr, "  --volume host:cont[:ro]  Bind mount (Phase 7b)\n");
    fprintf(stderr, "  --interactive            Allocate PTY (run only)\n");
    fprintf(stderr, "  --secure                 Drop caps + NO_NEW_PRIVS + seccomp + RO /sys (Phase 8b)\n");
    fprintf(stderr, "  --env KEY=VALUE          Set environment variable\n");
    fprintf(stderr, "  --help                   Show this help\n\n");
    fprintf(stderr, "Backwards compat: %s [options] <command> [args]\n", progname);
    fprintf(stderr, "is equivalent to %s run [options] <command> [args].\n", progname);
}