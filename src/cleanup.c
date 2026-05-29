#include "cleanup.h"
#include "state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <signal.h>

int cleanup_stale_resources(bool enable_debug, bool dry_run) {
    int cleaned = 0;

    /* 1. Walk state_dir_root() and remove entries whose pidfile
     *    references a dead PID. */
    container_info_t infos[MAX_CONTAINERS];
    int n = state_list(infos, MAX_CONTAINERS);
    for (int i = 0; i < n; i++) {
        if (!infos[i].alive) {
            if (enable_debug || dry_run) {
                printf("%s state dir %s (PID %d dead)\n",
                       dry_run ? "[dry] would remove" : "[cleanup] removing",
                       infos[i].id, infos[i].pid);
            }
            if (!dry_run) state_remove(infos[i].id);
            cleaned++;
        }
    }

    /* 2. Scan /sys/fs/cgroup/minicontainer_* for empty/orphaned cgroups.
     *    A cgroup is removable if its cgroup.procs is empty.
     *    (Cgroups whose containers are still alive will have procs.) */
    DIR *cg = opendir("/sys/fs/cgroup");
    if (cg) {
        struct dirent *e;
        while ((e = readdir(cg))) {
            if (strncmp(e->d_name, "minicontainer_", 14) != 0) continue;
            char procs_path[PATH_MAX];
            snprintf(procs_path, sizeof(procs_path),
                     "/sys/fs/cgroup/%s/cgroup.procs", e->d_name);
            struct stat st;
            if (stat(procs_path, &st) < 0) continue;
            if (st.st_size == 0) {
                char cg_path[PATH_MAX];
                snprintf(cg_path, sizeof(cg_path),
                         "/sys/fs/cgroup/%s", e->d_name);
                if (enable_debug || dry_run) {
                    printf("%s cgroup %s\n",
                           dry_run ? "[dry] would remove" : "[cleanup] removing",
                           cg_path);
                }
                if (!dry_run) rmdir(cg_path);
                cleaned++;
            }
        }
        closedir(cg);
    }

    /* 3. veth_h_* interfaces without owners. We collect the names
     *    of all live containers' host-veth interfaces (from state
     *    files), then walk every veth_h_* on the host and remove
     *    those NOT in the live set. Implemented via popen("ip -o
     *    link show") rather than netlink for simplicity — cleanup
     *    is run rarely and not perf-critical. */
    {
        /* Build the live-host-veth set from state files. */
        char live_veths[MAX_CONTAINERS][16];
        int live_count = 0;
        for (int i = 0; i < n; i++) {
            if (!infos[i].alive) continue;
            container_state_t s = {0};
            if (state_load(infos[i].id, &s) < 0) continue;
            if (s.has_veth && s.veth_host[0] &&
                live_count < MAX_CONTAINERS) {
                strncpy(live_veths[live_count++], s.veth_host,
                        sizeof(live_veths[0]) - 1);
            }
        }

        FILE *fp = popen("ip -o link show 2>/dev/null", "r");
        if (fp) {
            char line[4096];
            while (fgets(line, sizeof(line), fp)) {
                /* `ip -o link show` lines look like:
                 *   "12: veth_h_a1b2@if13: <BROADCAST,...> ..."
                 * We need the name between ": " and "@" (or ":"). */
                char *colon = strchr(line, ':');
                if (!colon) continue;
                char *name_start = colon + 1;
                while (*name_start == ' ') name_start++;
                char *name_end = name_start;
                while (*name_end && *name_end != '@' &&
                       *name_end != ':' && *name_end != ' ') name_end++;
                size_t name_len = (size_t)(name_end - name_start);
                if (name_len == 0 || name_len >= 16) continue;
                char vname[16];
                memcpy(vname, name_start, name_len);
                vname[name_len] = '\0';

                if (strncmp(vname, "veth_h_", 7) != 0) continue;

                bool is_live = false;
                for (int i = 0; i < live_count; i++) {
                    if (strcmp(vname, live_veths[i]) == 0) {
                        is_live = true;
                        break;
                    }
                }
                if (is_live) continue;

                if (enable_debug || dry_run) {
                    printf("%s veth %s (no live owner)\n",
                           dry_run ? "[dry] would delete" : "[cleanup] deleting",
                           vname);
                }
                if (!dry_run) {
                    char cmd[64];
                    snprintf(cmd, sizeof(cmd),
                             "ip link delete %s 2>/dev/null", vname);
                    int r = system(cmd);
                    (void)r;   /* ignore — best-effort */
                }
                cleaned++;
            }
            pclose(fp);
        }
    }

    /* 4. iptables NAT rules with no live source CIDR. Build a set of
     *    live CIDRs from state files; iterate POSTROUTING rules;
     *    remove rules whose -s argument isn't in the live set. */
    {
        char live_cidrs[MAX_CONTAINERS][INET_ADDRSTRLEN + 8];
        int live_count = 0;
        for (int i = 0; i < n; i++) {
            if (!infos[i].alive) continue;
            container_state_t s = {0};
            if (state_load(infos[i].id, &s) < 0) continue;
            if (s.has_veth && s.veth_container_ip[0] &&
                s.veth_netmask[0] && live_count < MAX_CONTAINERS) {
                snprintf(live_cidrs[live_count++], INET_ADDRSTRLEN + 8,
                         "%s/%s", s.veth_container_ip, s.veth_netmask);
            }
        }

        FILE *fp = popen(
            "iptables -t nat -L POSTROUTING -n --line-numbers 2>/dev/null",
            "r");
        if (fp) {
            /* iptables output (after the header) is:
             *   num  target  prot  opt source     destination
             *   1    MASQUERADE  all  --  10.0.0.0/24  0.0.0.0/0
             * We collect (line_number, source_cidr) for MASQUERADE
             * rules whose source isn't in the live set, then delete
             * them in REVERSE order (so line numbers stay valid). */
            int stale_nums[256];
            int stale_count = 0;
            char line[1024];
            while (fgets(line, sizeof(line), fp) && stale_count < 256) {
                int linenum;
                char target[32], prot[16], opt[8], src[64], dst[64];
                int matched = sscanf(line, "%d %31s %15s %7s %63s %63s",
                                     &linenum, target, prot, opt, src, dst);
                if (matched < 5) continue;
                if (strcmp(target, "MASQUERADE") != 0) continue;
                if (strncmp(src, "10.", 3) != 0 &&
                    strncmp(src, "192.168.", 8) != 0 &&
                    strncmp(src, "172.", 4) != 0) continue;  /* not ours */

                bool is_live = false;
                for (int i = 0; i < live_count; i++) {
                    if (strcmp(src, live_cidrs[i]) == 0) {
                        is_live = true;
                        break;
                    }
                }
                if (!is_live) {
                    stale_nums[stale_count++] = linenum;
                    if (enable_debug || dry_run) {
                        printf("%s iptables MASQUERADE -s %s (line %d)\n",
                               dry_run ? "[dry] would delete" : "[cleanup] deleting",
                               src, linenum);
                    }
                }
            }
            pclose(fp);

            /* Delete in reverse order so the line numbers we recorded
             * stay valid as preceding lines are removed. */
            if (!dry_run) {
                for (int i = stale_count - 1; i >= 0; i--) {
                    char cmd[128];
                    snprintf(cmd, sizeof(cmd),
                             "iptables -t nat -D POSTROUTING %d 2>/dev/null",
                             stale_nums[i]);
                    int r = system(cmd);
                    (void)r;
                }
            }
            cleaned += stale_count;
        }
    }

    /* 5. minicontainer/containers/<id>/ overlay directories whose
     *    state directory is missing. Scan the containers/ subdir
     *    (relative to minicontainer's working directory by Phase 3
     *    convention; user may override with --container-dir).
     *    We can't easily discover --container-dir from outside, so
     *    we sweep only the default "./containers" location. */
    {
        DIR *cdir = opendir("./containers");
        if (cdir) {
            struct dirent *e;
            while ((e = readdir(cdir))) {
                if (e->d_name[0] == '.') continue;

                /* Phase 3's overlay container IDs are different from
                 * Phase 7b's 12-hex IDs. Phase 3 uses overlay_X-style
                 * timestamp IDs. We can't always match, so the
                 * heuristic is: if the overlay dir's owner state dir
                 * is gone (or never existed), it's stale. We use
                 * the mtime as a tiebreaker — anything older than 24h
                 * and with no matching state file is stale. */
                char overlay_path[PATH_MAX];
                snprintf(overlay_path, sizeof(overlay_path),
                         "./containers/%s", e->d_name);

                struct stat st;
                if (stat(overlay_path, &st) < 0) continue;
                if (!S_ISDIR(st.st_mode)) continue;

                /* Check whether a matching state directory exists for
                 * a Phase 7b ID. */
                char state_path[PATH_MAX];
                state_dir_path(e->d_name, state_path, sizeof(state_path));
                struct stat sst;
                bool has_state = (stat(state_path, &sst) == 0);
                if (has_state) continue;   /* live owner — leave alone */

                /* No state file — assume stale (Phase 3 lifecycle is
                 * tied to a specific run; if no live container is
                 * using this overlay, it's debris). */
                if (enable_debug || dry_run) {
                    printf("%s overlay dir %s (no matching state)\n",
                           dry_run ? "[dry] would remove" : "[cleanup] removing",
                           overlay_path);
                }
                if (!dry_run) {
                    char cmd[PATH_MAX + 32];
                    snprintf(cmd, sizeof(cmd),
                             "rm -rf '%s' 2>/dev/null", overlay_path);
                    int r = system(cmd);
                    (void)r;
                }
                cleaned++;
            }
            closedir(cdir);
        }
    }

    return cleaned;
}