# minicontainer

> A minimal container runtime built from scratch to understand the internals of Docker and Kubernetes

[![Phase](https://img.shields.io/badge/Phase-7a%20refactor-blue)]()
[![License](https://img.shields.io/badge/License-MIT-green)]()
[![C Standard](https://img.shields.io/badge/C-C11-orange)]()

---

## Overview

**minicontainer** is an educational project that implements a container runtime from first principles. Instead of using Docker or other high-level tools, this project builds process isolation step-by-step using low-level Linux system calls.

**Current Phase:** Phase 7a - Execution-Core Consolidation (pure refactor)

Phase 7a is a structural refactor with **no new user-visible features**. Phases 0-6 each added an isolation primitive (PID, mount, overlay, UTS, user, IPC, cgroups, network) by introducing a new `*_exec()` function in a new module. By Phase 6 those functions were ~90% identical boilerplate — the same `clone()` / `waitpid()` / stack-allocation / overlay-setup / cgroup-setup / sync-pipe / error-cleanup skeleton, repeated five times. Phase 7a collapses them into a single `container_exec()` in a new `core.c` driven by a unified `container_config_t`. Every Phase 0-6 behavior is preserved byte-identical when the corresponding flags are set; the helpers (`setup_uts`, `setup_rootfs`, `setup_overlay`, `setup_cgroup`, `setup_net`, etc.) remain in their respective modules and are now called once from `core.c`'s unified `child_func`.

---

## Features

### Phase 7a (Current)

- ✅ **Unified `container_exec()`** — One entry point in `core.c` replaces the per-phase `*_exec()` chain (`spawn_process` → `namespace_exec` → `mount_exec` → `overlay_exec` → `uts_exec` → `cgroup_exec` → `net_exec`). Behavior is driven entirely by `container_config_t` flags.
- ✅ **Unified `container_config_t`** — Subsumes every prior `*_config_t`. Sets the namespace flags, rootfs / overlay / hostname / user-mapping / cgroup-limits / veth fields in one struct.
- ✅ **Unified `container_context_t`** — Aggregates `overlay_ctx`, `cgroup_ctx`, `net_ctx`, and the clone stack pointer so `container_cleanup()` tears down everything in one call.
- ✅ **One `child_func()`, one `close_inherited_fds()`, one `child_args_t`** — Five duplicate copies (one per phase module) collapse into a single canonical definition inside `core.c`.
- ✅ **`env.c` / `env.h` Extracted** — `build_container_env()` moves out of `main.c` into its own module so tests and future subcommands can include it without dragging in the CLI layer. The function body is unchanged from Phase 5's defensive refactor.
- ✅ **`user_ns_mapping_t` Replaces Synthetic `uts_config_t` Workaround** — Phase 5's `cgroup_exec` had to construct a partial `uts_config_t` just to call `setup_user_namespace_mapping()`; Phase 7a introduces a dedicated five-field struct (`uts.h`) that captures only what the mapping actually needs.
- ✅ **`find_ip_binary()` Promoted to Public** — Declared in `net.h` so `core.c`'s parent-side early-failure check (Phase 6 invariant — fail loudly before `clone()` when `ip(8)` is missing) can call it without duplicating the path search.
- ✅ **`spawn.c` / `namespace.c` / `test_spawn.c` / `test_namespace.c` Deleted** — Their behavior is fully covered by `container_exec` with appropriate flag combinations; `test_core.c` retains the bare-spawn and PID-only regression cases.
- ✅ **All Phase 2-6 Tests Updated** — `test_mount.c`, `test_overlay.c`, `test_uts.c`, `test_cgroup.c`, `test_net.c` now call `container_exec(&cfg)` with `container_config_t` instead of the deleted per-phase APIs; local `build_container_env()` stubs replaced by `#include "env.h"`.
- ✅ **Makefile `HELPER_OBJS` Pattern** — Every test links against the same helper chain as `minicontainer` (minus `main.o`, plus the test's own `.o`). One list, six rules — Phase 7a's whole point translated to the build system.

### Phase 6

- ✅ **Network Namespace Isolation** - `CLONE_NEWNET` gives the container its own loopback, routing table, and interfaces
- ✅ **`--net` Flag** - Enables `CLONE_NEWNET` and provisions a veth pair connecting the container to the host
- ✅ **Veth Pair Networking** - Host end stays in root netns, container end is moved into the child via `ip link set ... netns <pid>`
- ✅ **Default Subnet** - Host `10.0.0.1`, container `10.0.0.2`, `/24`; override with `--net-host-ip`, `--net-container-ip`, `--net-netmask`
- ✅ **`--no-nat`** - Skip the `iptables` MASQUERADE rule (default-enabled) when outbound internet is not wanted
- ✅ **`ip(8)` Driver** - Network setup uses `fork+exec` of the `ip` binary rather than netlink — pedagogical clarity over performance
- ✅ **Three-Path `find_ip_binary()`** - Searches `/sbin/ip` → `/usr/sbin/ip` → `/bin/ip`, so the same helper works against the host (pre-clone) and the rootfs (post-pivot_root); requires `ip` in `BINS` of `build_rootfs.sh`
- ✅ **`generate_veth_names()` Before clone()** - Names are baked into `child_args` before clone(), because clone() without `CLONE_VM` copies the parent's address space; post-clone parent writes are invisible to the child
- ✅ **Widened Sync Pipe** - Created when `enable_user_namespace OR enable_network`, so veth move/configuration finishes before the child runs

### Phase 5

- ✅ **cgroups v2 Resource Control** - Limit memory, CPU, and process count per container
- ✅ **`--memory <limit>`** - Memory ceiling (e.g., `100M`, `1G`); OOM-killed if exceeded
- ✅ **`--cpus <fraction>`** - CPU bandwidth limit (e.g., `0.5` = 50% of one core)
- ✅ **`--pids <max>`** - Maximum process count; defends against fork bombs
- ✅ **Three-Phase Lifecycle** - Create cgroup before clone, add PID after clone, remove after exit
- ✅ **Auto-Enable Cgroup** - Any cgroup flag (`--memory`/`--cpus`/`--pids`) enables the cgroup subsystem
- ✅ **`build_container_env()` Defensive Refactor** - `calloc` + bounds check, preparing for Phase 7 extraction (see decisions.md #21)

### Phase 4c

- ✅ **IPC Namespace Isolation** - `CLONE_NEWIPC` gives containers independent IPC tables
- ✅ **`--ipc` Flag** - Explicit opt-in (unlike `--rootfs`, does not auto-enable)
- ✅ **System V IPC Isolated** - Shared memory, semaphores, message queues
- ✅ **POSIX Message Queues Isolated** - Via `/dev/mqueue` in the new namespace

### Phase 4b

- ✅ **User Namespace Isolation** - `CLONE_NEWUSER` enables rootless containers (`--user`)
- ✅ **UID/GID Mapping** - Container root (UID 0) maps to host user (e.g., UID 1000)
- ✅ **Sync Pipe** - Parent-child coordination ensures UID/GID maps are written before child proceeds
- ✅ **Graceful /proc Degradation** - `/proc` mount failure in user namespace downgrades to warning
- ✅ **Proc Mount Hardening** - `/proc` mounted with `MS_NOSUID | MS_NODEV | MS_NOEXEC`

### Phase 4

- ✅ **UTS Namespace Isolation** - Container gets its own hostname via `CLONE_NEWUTS`
- ✅ **`--hostname <name>`** - Set a custom hostname; auto-enables UTS namespace
- ✅ **Host Unchanged** - `sethostname()` in child only affects the container's namespace

### Phase 3

- ✅ **OverlayFS Copy-on-Write** - Base image stays read-only; writes go to per-container upper layer
- ✅ **Automatic Cleanup** - Upper layer, work directory, and merged mount removed on container exit
- ✅ **Clean Environment** - Minimal `PATH`, `HOME`, `TERM` instead of host's full `environ`
- ✅ **`--env KEY=VALUE`** - Custom environment variables with proper key replacement (no duplicates)
- ✅ **File Descriptor Cleanup** - `close_inherited_fds()` prevents container escape via leaked fds (CVE-2024-21626, CVE-2016-9962)
- ✅ **Mount Hardening** - Overlay mounted with `MS_NODEV | MS_NOSUID` to block device node access and SUID escalation
- ✅ **Misplaced Flag Detection** - Warns when minicontainer flags appear after the command
- ✅ **Auto-enable PID Namespace** - `--rootfs` now implies `--pid`

### Phase 2

- ✅ **Mount Namespace Isolation** - `CLONE_NEWNS` gives the container its own mount table
- ✅ **Root Filesystem Pivot** - `pivot_root()` switches the container to a custom rootfs
- ✅ **Private /proc Mount** - Container's `/proc` reflects only its own PID namespace
- ✅ **Automatic Mount Namespace** - `--rootfs` flag auto-enables mount namespace
- ✅ **Old Root Cleanup** - Old root is lazily unmounted and the mount point removed

### Phase 1

- ✅ **PID Namespace Isolation** - Child process runs as PID 1 in its own namespace via `clone(CLONE_NEWPID)`
- ✅ **Manual Stack Allocation** - Allocates and manages stack for `clone()` child
- ✅ **Namespace Cleanup** - Proper deallocation of clone stack via `namespace_cleanup()`
- ✅ **Conditional Isolation** - `--pid` flag enables namespace; without it, runs like Phase 0

### Phase 0 (Foundation)

- ✅ **Process Spawning** - Fork child processes and execute arbitrary commands
- ✅ **Exit Status Handling** - Properly report exit codes and signal deaths
- ✅ **Zombie Prevention** - SIGCHLD handler prevents zombie processes
- ✅ **Environment Variables** - Pass custom environment to child processes
- ✅ **Error Handling** - Robust error checking on all system calls
- ✅ **Debug Mode** - Optional verbose output for troubleshooting

---

## Quick Start

### Build

```bash
make
```

This compiles the `minicontainer` executable in the current directory.

#### Ubuntu 24.04+ rootless users — one-time AppArmor setup

Ubuntu 24.04 sets `kernel.apparmor_restrict_unprivileged_userns=1`,
which silently restricts what unprivileged user namespaces created by
unconfined processes can do. Without a per-binary AppArmor profile,
rootless minicontainer (`--user`) fails with `sethostname: Operation
not permitted` and similar EPERM surface. Run this once:

```bash
make install-apparmor      # asks for sudo once; loads /etc/apparmor.d/minicontainer
```

`make uninstall-apparmor` removes it. The profile only declares
`userns,` (it's not a sandbox) — same pattern Docker, Podman,
Chromium, and Firefox use for their unprivileged-userns needs. See
[docs/decisions.md](docs/decisions.md) Error #19 for the full story.

If you move or rename the built binary afterward, re-run
`make install-apparmor` so the profile's path matches. Other distros
(earlier Ubuntu, Fedora, Arch, RHEL/Rocky) don't need this step.

### Run

```bash
# Basic usage (no namespace)
./minicontainer /bin/ls -la

# Rootless container (no sudo — user namespace maps UID 0 to host user)
# Ubuntu 24.04+: requires `make install-apparmor` once (see Build above)
./minicontainer --user --pid --hostname mycontainer /bin/sh -c 'id && hostname'

# Rootless with rootfs (proc mount may warn — see Troubleshooting)
./minicontainer --user --pid --rootfs ./rootfs --hostname web /bin/sh

# IPC namespace isolation (container gets empty IPC tables)
sudo ./minicontainer --pid --ipc /bin/sh -c 'ipcs'

# Rootless + IPC isolation (no sudo, full IPC isolation)
./minicontainer --user --pid --ipc --hostname test /bin/sh -c 'ipcs; id'

# Memory limit — container OOM-killed if it exceeds 100MB (Phase 5)
sudo ./minicontainer --pid --memory 100M /bin/sh -c 'head -c 50M /dev/zero | wc -c'

# CPU + PID limits — 50% of one core, max 20 processes (fork bomb defense)
sudo ./minicontainer --pid --cpus 0.5 --pids 20 /bin/sh

# Full Phase 5: all namespaces + cgroup limits
sudo ./minicontainer --pid --rootfs ./rootfs --overlay --hostname web --ipc \
    --memory 100M --cpus 0.5 --pids 20 /bin/sh

# Network namespace (Phase 6) — default subnet 10.0.0.0/24, NAT on
sudo ./minicontainer --pid --rootfs ./rootfs --net /bin/sh -c 'ip addr show'

# Custom subnet for the veth pair
sudo ./minicontainer --pid --rootfs ./rootfs --net \
    --net-host-ip 10.42.0.1 --net-container-ip 10.42.0.2 --net-netmask 30 \
    /bin/sh -c 'ip addr show && ip route'

# Network namespace without iptables MASQUERADE (no outbound internet)
sudo ./minicontainer --pid --rootfs ./rootfs --net --no-nat /bin/sh

# HTTPS from inside the container — exercises veth + NAT + DNS + TLS
sudo ./minicontainer --pid --rootfs ./rootfs --net /bin/sh -c \
    'curl -sS --connect-timeout 5 --max-time 20 \
        -o /tmp/curl_out -w "%{http_code}\n" https://example.com'
# Expected: 200. If it hangs and times out instead, the host's iptables
# FORWARD chain is dropping the container's packets — see Known
# Limitation #8 in docs/decisions.md for the two ACCEPT rules to add.

# Full stack: every isolation + cgroups + network
sudo ./minicontainer --pid --rootfs ./rootfs --overlay --hostname web --ipc \
    --memory 100M --cpus 0.5 --pids 20 --net \
    /bin/sh -c 'hostname && id && ip addr show && ipcs'

# Hostname isolation (container gets its own hostname)
sudo ./minicontainer --rootfs ./rootfs --hostname mycontainer /bin/sh -c 'hostname'

# Full isolation: overlay + hostname (independent flags, combine for full isolation)
sudo ./minicontainer --rootfs ./rootfs --overlay --hostname mycontainer /bin/sh

# Rootfs + overlay (copy-on-write, base image untouched)
sudo ./minicontainer --rootfs ./rootfs --overlay /bin/sh

# Custom environment variables
sudo ./minicontainer --rootfs ./rootfs --overlay \
    --env FOO=bar /bin/sh -c 'echo $FOO'

# Custom container directory for overlay data
sudo ./minicontainer --rootfs ./rootfs --overlay \
    --container-dir ./my_containers /bin/sh

# PID namespace isolation (requires root or CAP_SYS_ADMIN)
sudo ./minicontainer --pid /bin/sh -c 'echo $$'    # Prints: 1

# Full isolation: PID + mount namespace with custom rootfs (no overlay)
sudo ./minicontainer --pid --rootfs ./rootfs /bin/sh -c 'echo PID: $$ && ls /'

# Debug mode with full isolation + overlay
sudo ./minicontainer --debug --rootfs ./rootfs --overlay /bin/sh -c 'echo $$'

# Get help
./minicontainer --help
```

**Flag relationships:** `--rootfs` implies `--pid` (in `main.c`'s config setup),
`--hostname` auto-enables `CLONE_NEWUTS`, and `--user` auto-enables
`CLONE_NEWUSER`. `--ipc` is explicit opt-in and does **not** auto-enable with
any other flag (IPC isolation is only meaningful for workloads that use System V
IPC or POSIX message queues, so auto-enabling would add overhead for the common
case). Any of `--memory`/`--cpus`/`--pids` auto-enables the cgroup subsystem
(`enable_cgroup`); supplying none of them skips cgroup setup entirely.
`--net` is explicit opt-in (Phase 6) and the `--net-host-ip` /
`--net-container-ip` / `--net-netmask` / `--no-nat` sub-flags **require** `--net`
— passing them without `--net` is a hard error rather than a silent no-op.
These flags are otherwise independent — `--hostname` does not imply
`--overlay` and vice versa. For full isolation, combine them explicitly. Note
that this auto-enable behavior is a `main.c` design choice; the underlying
library functions accept each flag independently, allowing other entry points
to compose isolation differently.

**Note:** Without `--user`, namespace features require root or `CAP_SYS_ADMIN`.
With `--user`, `CLONE_NEWUSER` grants capabilities inside the namespace without
host privileges. **Cgroup operations (Phase 5) always require root** —
`/sys/fs/cgroup/` is owned by root and `--user` does not delegate write access:
```bash
# Option 1: Run with sudo
sudo ./minicontainer --pid --rootfs ./rootfs /bin/sh

# Option 2: Grant capability (avoids sudo for subsequent runs)
sudo setcap cap_sys_admin+ep ./minicontainer
./minicontainer --pid /bin/sh -c 'echo $$'
```

### Test

```bash
# Run all tests (Phase 0 + 1 + 2 + 3 + 4 + 4b + 4c + 5 + 6)
make test

# Run example commands
make examples

# Check for memory leaks
make valgrind
```

---

## Project Structure

```
minicontainer/
├── include/
│   ├── core.h               # Phase 7a: container_config_t, container_exec(), container_cleanup()
│   ├── env.h                # Phase 7a: build_container_env() (extracted from main.c)
│   ├── net.h                # Phase 6: veth setup helpers, find_ip_binary() (public since 7a)
│   ├── cgroup.h             # Phase 5: cgroups v2 setup/limits helpers
│   ├── uts.h                # Phase 4/4b/4c: setup_uts(), setup_user_namespace_mapping(), user_ns_mapping_t (since 7a)
│   ├── overlay.h            # Phase 3: setup_overlay(), teardown_overlay()
│   └── mount.h              # Phase 2: setup_rootfs(), mount_proc()
├── src/
│   ├── main.c               # CLI entry point — populates container_config_t, calls container_exec()
│   ├── core.c               # Phase 7a: unified container_exec / child_func / close_inherited_fds (one canonical copy)
│   ├── env.c                # Phase 7a: build_container_env() (calloc + bounds-check version from Phase 5)
│   ├── net.c                # Phase 6: setup_net, configure_container_net, cleanup_net, generate_veth_names, find_ip_binary
│   ├── cgroup.c             # Phase 5: setup_cgroup, add_pid_to_cgroup, remove_cgroup
│   ├── uts.c                # Phase 4/4b: setup_uts, setup_user_namespace_mapping
│   ├── overlay.c            # Phase 3: setup_overlay, teardown_overlay (+ static path/dir helpers)
│   └── mount.c              # Phase 2: setup_rootfs, mount_proc
├── tests/
│   ├── test_core.c          # Phase 7a: bare-exec + pid-only regressions (covers prior test_spawn / test_namespace cases)
│   ├── test_net.c           # Phase 6: namespace creation, backward compat, net + cgroup (requires root)
│   ├── test_cgroup.c        # Phase 5: cgroup lifecycle, memory/CPU/PID limits (requires root)
│   ├── test_uts.c           # Phase 4/4b/4c: UTS + user + IPC namespace tests
│   ├── test_overlay.c       # Phase 3: Overlay/env/fd tests (requires root + rootfs)
│   └── test_mount.c         # Phase 2: Mount/rootfs tests (requires root + rootfs)
├── scripts/
│   └── build_rootfs.sh      # Builds minimal rootfs from host binaries (BINS includes `ip`, `curl`, NSS modules)
├── docs/
│   └── decisions.md         # Design decisions and error log
├── Makefile                 # Build system (HELPER_OBJS unified link chain since 7a)
└── README.md                # This file
```

**Files removed in Phase 7a:** `include/spawn.h`, `include/namespace.h`,
`src/spawn.c`, `src/namespace.c`, `tests/test_spawn.c`,
`tests/test_namespace.c`. Their functionality is fully covered by
`container_exec()` with `enable_pid_namespace` set (or unset, for the
bare-spawn case). The deletions are why Phase 7a is a *pure refactor*
— old code paths are replaced 1:1, no new behavior.

---

## Architecture

### Module Separation

```
┌──────────────────────────────────────────────────────┐
│                main.c (CLI Layer)                    │
│  • Parses --pid, --rootfs, --overlay, --hostname,    │
│    --memory, --cpus, --pids, --net, --net-host-ip,   │
│    --net-container-ip, --net-netmask, --no-nat, etc. │
│  • build_container_env() — clean environment (env.c) │
│  • Misplaced flag detection                          │
│  • Populates container_config_t, calls               │
│    container_exec()  (Phase 7a: one entry point)     │
└─────────────────────┬────────────────────────────────┘
                      │
                      ▼
┌──────────────────────────────────────────────────────┐
│      core.c — Unified Orchestrator (Phase 7a)        │
│                                                      │
│  container_exec(const container_config_t *)          │
│  - 15-step parent-side lifecycle (preserved from     │
│    Phase 5/6, now a single canonical copy):          │
│    1.  setup_cgroup()        (if enable_cgroup)      │
│    2.  pipe(sync_pipe)       (if user OR network)    │
│    3.  setup_overlay()       (if overlay + rootfs)   │
│    4.  malloc(STACK_SIZE)                            │
│    5.  generate_veth_names() BEFORE clone()          │
│    6.  populate child_args (snapshot, pre-clone)     │
│    7.  clone(CLONE_NEW* per config flags)            │
│    8.  setup_user_namespace_mapping() (user_ns)      │
│    9.  setup_net()           (if network)            │
│   10.  write sync byte → child unblocks              │
│   11.  add_pid_to_cgroup()   (AFTER sync)            │
│   12.  waitpid()                                     │
│   13.  parse WIFEXITED / WIFSIGNALED                 │
│   14.  teardown_overlay() if active                  │
│   15.  (cleanup_net / remove_cgroup / free stack     │
│         deferred to container_cleanup())             │
│                                                      │
│  container_cleanup(container_result_t *)             │
│  - cleanup_net() → delete host veth + iptables rule  │
│  - remove_cgroup() → rmdir /sys/fs/cgroup/...        │
│  - free(stack_ptr)                                   │
└─────────────────────┬────────────────────────────────┘
                      │
                      ▼
┌──────────────────────────────────────────────────────┐
│   Child Process (core.c child_func — single copy)    │
│  • Wait on sync pipe (if --user OR --net)            │
│  • setup_uts()              (uts.c)                  │
│  • setup_rootfs()           (mount.c)                │
│  • mount_proc() with graceful /proc degradation      │
│  • configure_container_net() (net.c, if network)     │
│  • close_inherited_fds() — CVE-2024-21626/CVE-2016-  │
│    9962 mitigation, single canonical copy            │
│  • execve(program, argv, envp) — clean container env │
└──────────────────────────────────────────────────────┘

Helper modules called from core.c's parent and child halves:
  mount.c, overlay.c, uts.c, cgroup.c, net.c
Each retains only the helper functions it owns — no per-phase
*_exec() orchestrator, no duplicate child_func, no duplicate
close_inherited_fds.
```

**Phase transitions:**
- Phase 0: bare `fork/execve` (`spawn_process`)
- Phase 1: `clone(CLONE_NEWPID)` (`namespace_exec`)
- Phase 2: `mount.c` (`mount_exec` — `CLONE_NEWPID | CLONE_NEWNS` + `pivot_root`)
- Phase 3: `overlay.c` (`overlay_exec` — OverlayFS + env isolation + fd cleanup), calling into `mount.c`
- Phase 4: `uts.c` (`uts_exec` — `CLONE_NEWUTS` + `sethostname`), calling into `overlay.c` and `mount.c`
- Phase 4b: `uts.c` extended in place with `CLONE_NEWUSER`, sync pipe, UID/GID mapping
- Phase 4c: `uts.c` extended in place with `CLONE_NEWIPC` (one flag, zero new child-side code)
- Phase 5: `cgroup.c` (`cgroup_exec`) supersedes `uts.c` as the top-level exec module
- Phase 6: `net.c` (`net_exec`) supersedes `cgroup.c` as the top-level exec module
- **Phase 7a:** every `*_exec()` collapsed into `container_exec()` in `core.c`. The helper modules (mount.c, overlay.c, uts.c, cgroup.c, net.c) keep their helpers (`setup_*`, `cleanup_*`) — they no longer carry the orchestration boilerplate.

**Why the duplication existed (and why 7a removed it):** Each phase's
`*_exec()` shared ~90% of its structure with the previous phase —
`clone()`, `waitpid()`, stack allocation, sync-pipe orchestration, and
exit status parsing repeated five times by Phase 6. This was a
deliberate pedagogical choice: each module was self-contained and
readable without cross-referencing earlier phases. The cost
accumulated: bug fixes had to propagate to five copies (Phase 5's first
cut dropped invariants from its copy and required a retro audit; Phase
6 repeated the pattern), and every new phase added ~250 lines of
boilerplate before introducing one line of actual isolation logic.
Phase 7a's `container_exec()` is a single 250-line orchestrator that
covers every flag combination. The price is that `core.c` is harder to
read in one pass than any single `*_exec()` was — but it's exactly as
hard to read as the most complex one (`net_exec`), and it doesn't
multiply. Decisions.md #30 has the full rationale.

### API Design

**Configuration structure (Phase 7a — unified):**

A single `container_config_t` carries every flag and parameter. Setting
a flag without its dependent fields is harmless (e.g.,
`enable_uts_namespace = true` with `hostname = NULL` creates the
namespace but doesn't change the hostname); setting a parameter without
the gating flag is silently ignored (e.g., `hostname = "foo"` with
`enable_uts_namespace = false` doesn't sethostname).

```c
#include "core.h"
#include "env.h"

char **env = build_container_env(NULL, false);
char *argv[] = {"/bin/sh", "-c", "echo hello", NULL};

container_config_t cfg = {
    .program = "/bin/sh",
    .argv    = argv,                  // NULL-terminated
    .envp    = env,                   // From build_container_env()
    .enable_debug = false,

    // Namespace flags (Phase 1 / 2 / 4 / 4b / 4c / 6)
    .enable_pid_namespace   = true,   // CLONE_NEWPID
    .enable_mount_namespace = true,   // CLONE_NEWNS
    .enable_uts_namespace   = true,   // CLONE_NEWUTS
    .enable_user_namespace  = true,   // CLONE_NEWUSER (rootless)
    .enable_ipc_namespace   = true,   // CLONE_NEWIPC
    .enable_network         = true,   // CLONE_NEWNET

    // Filesystem (Phase 2 / 3)
    .rootfs_path   = "./rootfs",
    .enable_overlay = true,
    .container_dir = NULL,            // NULL = "./containers"

    // UTS (Phase 4)
    .hostname = "mycontainer",

    // User namespace mapping (Phase 4b)
    .uid_map_inside  = 0,
    .uid_map_outside = getuid(),
    .uid_map_range   = 1,
    .gid_map_inside  = 0,
    .gid_map_outside = getgid(),
    .gid_map_range   = 1,

    // cgroup resource limits (Phase 5)
    .enable_cgroup = true,
    .cgroup_limits = {
        .memory_limit = 100 * 1024 * 1024,  // 100 MB
        .cpu_quota    = 50000,               // 50% of one core
        .cpu_period   = 100000,              // (over 100ms period)
        .pid_limit    = 20                   // Max 20 processes
    },

    // Network — veth + optional NAT (Phase 6)
    .veth = {
        .host_ip      = "10.0.0.1",
        .container_ip = "10.0.0.2",
        .netmask      = "24",
        .enable_nat   = true                 // iptables MASQUERADE
    }
};
```

**Result structure (Phase 7a — unified):**

```c
container_result_t r = container_exec(&cfg);

if (r.exited_normally) {
    printf("Exit code: %d\n", r.exit_status);
} else {
    printf("Killed by signal %d\n", r.signal);   // 137 = OOM-killed
}

container_cleanup(&r);   // cleanup_net + remove_cgroup + free(stack)
free(env);
```

`container_result_t.ctx` is a `container_context_t` aggregating the
runtime state each helper produced (`overlay_ctx`, `cgroup_ctx`,
`net_ctx`, `stack_ptr`). `container_cleanup()` walks the context in
reverse setup order; calling it on a zero-initialized result is a
no-op (idempotent).

**user_ns_mapping_t** — a Phase 7a addition in `uts.h`. Replaces the
synthetic-`uts_config_t` workaround that Phase 5's `cgroup_exec` used
to thread mapping data through `setup_user_namespace_mapping()`. The
flat `uid_map_*` / `gid_map_*` fields on `container_config_t` are
packed into a `user_ns_mapping_t` inside `core.c` and passed to the
helper:

```c
typedef struct {
    uid_t uid_map_inside;
    uid_t uid_map_outside;
    size_t uid_map_range;
    gid_t gid_map_inside;
    gid_t gid_map_outside;
    size_t gid_map_range;
    bool  enable_debug;
} user_ns_mapping_t;
```

---

## How It Works

### Network Namespace + veth Pair (Phase 6)

```
Host Network Namespace (root netns)              Container Network Namespace
┌──────────────────────────────────────┐         ┌──────────────────────────────┐
│  eth0  (real NIC, 192.168.x.x)       │         │  lo                          │
│                                      │         │                              │
│  veth_h_<id>  10.0.0.1/24            │◄═══════►│  veth_c_<id>  10.0.0.2/24    │
│       │                              │  veth   │       │                      │
│       │                              │  pair   │       │ default route via    │
│       ▼                              │         │       │ 10.0.0.1             │
│  iptables -t nat -A POSTROUTING      │         │       ▼                      │
│  -s 10.0.0.0/24 -j MASQUERADE        │         │  10.0.0.0/24 dev veth_c_<id> │
│  (skipped under --no-nat)            │         │                              │
└──────────────────────────────────────┘         └──────────────────────────────┘
```

**Setup ordering (parent side):**

1. `generate_veth_names(&ctx)` — BEFORE `clone()`. Names like `veth_h_a1b2` and
   `veth_c_a1b2` get baked into `child_args`. Clone without `CLONE_VM` copies
   the parent's address space; anything the parent writes to `child_args`
   AFTER clone is invisible to the child.
2. `clone(CLONE_NEWNET | …)` — child is born in a fresh, empty netns.
3. `setup_net()` — parent runs `ip link add` to create the pair, then
   `ip link set veth_c_<id> netns <child_pid>` to move the container end into
   the child's netns; assigns `host_ip` to `veth_h_<id>`, brings it up;
   optionally appends the `MASQUERADE` rule.
4. Sync pipe write — child unblocks only after the veth has been moved.
5. Child runs `configure_container_net()`: `lo` up, IP add to `veth_c_<id>`,
   default route via `host_ip`.

**Teardown (`cleanup_net()`):** delete `veth_h_<id>` (the kernel removes the
container end automatically when the netns is destroyed at child exit), delete
the iptables NAT rule using the CIDR stored in `ctx->nat_source_cidr` (so
cleanup is self-contained and doesn't need the original `veth_config_t`).

**`find_ip_binary()`** searches `/sbin/ip` → `/usr/sbin/ip` → `/bin/ip` so the
same helper resolves both pre-clone (against the host filesystem) and
post-pivot_root (against the rootfs — `build_rootfs.sh` puts `ip` at
`/bin/ip`).

### OverlayFS Copy-on-Write (Phase 3)

```
rootfs/ (lowerdir — read-only)         containers/<id>/upper/ (writable)
├── bin/sh                             ├── tmp/newfile.txt  ← container created
├── bin/ls                             └── bin/ls           ← copy-up (modified)
├── etc/
└── tmp/
         ↓                                      ↓
    ┌─────────────────────────────────────────────────┐
    │        containers/<id>/merged/ (unified view)   │
    │  Container sees: rootfs + writes merged together│
    │  Reads: lowerdir first, then upperdir           │
    │  Writes: always go to upperdir                  │
    └─────────────────────────────────────────────────┘

On container exit: upper/, work/, merged/ are removed.
rootfs/ is never modified.
```

### The clone/pivot_root/execve Pattern (introduced in Phase 3, unified in Phase 7a)

The Phase 3 introduction of OverlayFS established the parent/child
split that every subsequent phase elaborated. Phase 7a moved the
parent-side orchestration into `container_exec()` and the child-side
sequence into `core.c`'s `child_func()`, so the diagram below now
describes the canonical structure of those two functions rather than a
per-module `overlay_exec`.

```
Parent (container_exec)          Child Process (new PID + mount namespace)
     │
     ├── setup_overlay()
     │   ├── init_overlay_paths()
     │   ├── create_overlay_dirs()
     │   └── mount_overlay(MS_NODEV|MS_NOSUID)
     │
     ├── clone(NEWPID|NEWNS) ───────► child_func() [PID 1 inside ns]
     │                                      │
     │                                setup_rootfs(merged/)
     │                                  ├── mount(MS_PRIVATE /)
     │                                  ├── bind mount merged/
     │                                  ├── pivot_root(".", "old_root")
     │                                  ├── chdir("/")
     │                                  └── umount2("/old_root", MNT_DETACH)
     │                                      │
     │                                mount_proc()
     │                                  └── mount("proc", "/proc", "proc")
     │                                      │
     │                                close_inherited_fds()
     │                                  └── close fds 3+ via /proc/self/fd
     │                                      │
     │                                execve("/bin/sh", envp=clean_env)
     │                                      │
     │                                 exit(status)
     │                                      │
     │◄──── waitpid() ──────────────── [Reaped]
     │
     ├── teardown_overlay()
     │   ├── umount2(merged/, MNT_DETACH)
     │   └── remove upper/, work/, merged/, container_base/
     │
     │ container_cleanup()   (Phase 7a — was overlay_cleanup() pre-7a)
     │ ├── cleanup_net()
     │ ├── remove_cgroup()
     │ └── free(stack_ptr)
     ▼
```

### PID Namespace Isolation (Phase 1)

```
Host PID Namespace (Initial)
├── PID 1: systemd/init
├── PID 1234: minicontainer (parent)
│   └── clone(CLONE_NEWPID) creates child in new namespace
└── PID 5678: other processes

Container PID Namespace
├── PID 1: /bin/sh (same process is PID 5679 on host)
└── PID 2: ls (if sh spawns it)
```

The same process has **two PIDs**: one in the host namespace (real) and one in the container namespace (virtual). The child cannot see or signal host processes.

### Key System Calls

| System Call | Purpose | Phase |
|-------------|---------|-------|
| `clone(CLONE_NEWNET)` | New network namespace — empty interface and routing tables | 6 |
| `fork()` + `execve("ip")` | Drive veth creation, address assignment, route install, namespace move | 6 |
| `fork()` + `execve("iptables")` | Append/delete `MASQUERADE` rule for outbound NAT (optional) | 6 |
| `mkdir()` / `rmdir()` | Create/destroy cgroup directory under `/sys/fs/cgroup/` | 5 |
| `write()` (to cgroup files) | Set `memory.max`/`cpu.max`/`pids.max`; add PID to `cgroup.procs` | 5 |
| `clock_gettime()` | Generate unique cgroup name from timestamp + nanoseconds | 5 |
| `pipe()` | Creates sync pipe for parent-child UID/GID map coordination | 4b |
| `open()`/`write()` | Writes `/proc/<pid>/setgroups`, `uid_map`, `gid_map` | 4b |
| `sethostname()` | Sets container hostname inside UTS namespace | 4 |
| `mount("overlay")` | Mounts OverlayFS with `MS_NODEV \| MS_NOSUID` | 3 |
| `close()` | Closes inherited parent fds before `execve()` | 3 |
| `getrlimit()` | Gets fd limit for brute-force fallback | 3 |
| `pivot_root()` | Swaps root filesystem for the container | 2 |
| `mount()` | Bind mounts rootfs, sets propagation, mounts `/proc` | 2 |
| `umount2()` | Lazily unmounts old root and overlay (`MNT_DETACH`) | 2, 3 |
| `clone()` | Creates child with namespace flags (`NEWPID`, `NEWNS`, `NEWUTS`, `NEWUSER`, `NEWIPC`, `NEWNET`) | 1–6 |
| `fork()` | Creates a new process (duplicate of parent) | 0 |
| `execve()` | Replaces process image with new program | 0–4c |
| `waitpid()` | Waits for child to exit and reaps zombie | 0–4c |
| `sigaction()` | Installs SIGCHLD handler to prevent zombies | 0 |

---

## Usage Examples

### Example 1: Network Namespace (Phase 6)

```bash
# Default subnet 10.0.0.0/24, host 10.0.0.1, container 10.0.0.2, NAT on
$ sudo ./minicontainer --pid --rootfs ./rootfs --net /bin/sh -c 'ip addr show'
1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 ...
    inet 127.0.0.1/8 scope host lo
2: veth_c_a1b2: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 ...
    inet 10.0.0.2/24 scope global veth_c_a1b2

# Host side after exit: veth_h_<id> is gone, NAT rule is gone
$ ip link show | grep veth_h_  # (no output)
$ sudo iptables -t nat -S POSTROUTING | grep 10.0.0  # (no output)

# Custom subnet — useful when 10.0.0.0/24 conflicts with another network
$ sudo ./minicontainer --pid --rootfs ./rootfs --net \
    --net-host-ip 10.42.0.1 --net-container-ip 10.42.0.2 --net-netmask 30 \
    /bin/sh -c 'ip route'
default via 10.42.0.1 dev veth_c_a1b2
10.42.0.0/30 dev veth_c_a1b2 scope link

# --no-nat: container can reach the host's veth_h_<id> end but has no
# outbound internet (no MASQUERADE rule appended)
$ sudo ./minicontainer --pid --rootfs ./rootfs --net --no-nat \
    /bin/sh -c 'ping -c 1 10.0.0.1'   # works
$ sudo ./minicontainer --pid --rootfs ./rootfs --net --no-nat \
    /bin/sh -c 'ping -c 1 8.8.8.8'    # times out

# Sub-flag validation — passing --net-host-ip without --net is a hard error
$ sudo ./minicontainer --pid --net-host-ip 10.42.0.1 /bin/sh
Error: --net-host-ip / --net-container-ip / --net-netmask / --no-nat require --net
```

**Setup ordering matters:** `generate_veth_names()` runs *before* `clone()` so
the names live in `child_args`; the parent's `setup_net()` moves
`veth_c_<id>` into the child's netns and only then signals the sync pipe; the
child runs `configure_container_net()` (lo up, IP add, default route) once
unblocked. See "How It Works → Network Namespace + veth Pair" above.

#### HTTPS from inside the container (curl)

Phase 6's `scripts/build_rootfs.sh` installs `curl` plus the CA bundle,
glibc NSS modules, and a systemd-resolved-aware `/etc/resolv.conf` so
HTTPS clients work end-to-end from inside the container's netns
(Decision #29). The simplest demonstration:

```bash
$ sudo ./minicontainer --pid --rootfs ./rootfs --net /bin/sh -c \
    'curl -sS --connect-timeout 5 --max-time 20 \
        -o /tmp/curl_out -w "%{http_code}\n" https://example.com'
200

# The downloaded body lives in the rootfs at rootfs/tmp/curl_out
# (the container's /tmp is the host's rootfs/tmp directory):
$ head -c 80 rootfs/tmp/curl_out
<!doctype html><html lang="en"><head><title>Example Domain</title>
```

> **Iptables prerequisite.** On hosts with Docker, UFW, or firewalld
> installed, the `filter/FORWARD` chain defaults to `DROP` and the
> curl command above will hang until `--max-time 20` fires. Two
> ACCEPT rules unblock the container subnet:
>
> ```bash
> sudo iptables -I FORWARD 1 -s 10.0.0.0/24 -j ACCEPT
> sudo iptables -I FORWARD 1 -d 10.0.0.0/24 -j ACCEPT
> ```
>
> See Known Limitation #8 in `docs/decisions.md` for the full
> diagnostic checklist, persistence guidance, and the rationale for
> why minicontainer does not auto-install these rules.

#### `--overlay`: the curl write does not persist in the rootfs

The previous example left `rootfs/tmp/curl_out` sitting in the rootfs
because `--overlay` was not passed — every write inside the container
hit the rootfs directory directly. Adding `--overlay` changes that:

```bash
# Clean slate first, so the difference is unambiguous.
$ rm -f rootfs/tmp/curl_out

# Run the same curl with --overlay.
$ sudo ./minicontainer --pid --rootfs ./rootfs --overlay --net /bin/sh -c 'curl -sS --connect-timeout 5 --max-time 20 -o /tmp/curl_out -w "%{http_code}\n" https://example.com'
200

# The write went to the overlay's upperdir, which was discarded on
# container exit. The rootfs is untouched:
$ ls rootfs/tmp/curl_out
ls: cannot access 'rootfs/tmp/curl_out': No such file or directory
```

This is the OverlayFS copy-on-write contract from Phase 3 applied to
network writes: every modification the container makes — whether it's
`echo > file`, `apt install`, or `curl -o`, lands in the upper layer
and goes away when the container exits.

#### Seeing the write while it exists: interactive shell + `--overlay`

To watch the curl write actually exist inside the container before it
gets discarded, drop into an interactive shell instead of running
`-c '...'`. The shell stays in the foreground; you can run curl,
inspect the result, and then exit:

```bash
$ sudo ./minicontainer --pid --rootfs ./rootfs --overlay --net /bin/sh

# Inside the container (no PS1 prompt — Phase 7b adds proper PTY/TTY
# allocation; today the shell runs in line-mode without job control):
curl -sS --connect-timeout 5 --max-time 20 -o /tmp/curl_out -w "%{http_code}\n" https://enginerd2019.dev
# 200

ls -l /tmp/curl_out
# -rw-r--r-- 1 root root 1256 Jan  1 00:00 /tmp/curl_out

head -c 80 /tmp/curl_out
# <!doctype html><html lang="en"><head><title>Example Domain</title>

exit
# Note: exiting the shell currently emits a benign error (Phase 7b
# adds proper PTY teardown). The container has already exited cleanly
# by the time you see it; minicontainer is just unwinding the parent
# side without a TTY to release.

# Back on the host. The overlay's upperdir has been torn down:
$ ls rootfs/tmp/curl_out
ls: cannot access 'rootfs/tmp/curl_out': No such file or directory
```

The interactive shell is the clearest way to see that **the file
genuinely existed inside the container** — `ls` and `head` both see
it through the overlay merge — and that **nothing of it survives**
once the container exits and `teardown_overlay()` runs. Without the
interactive step the lifecycle is invisible; the curl write happens
and the overlay is torn down between `exec` and the next shell
prompt, so a `-c '...'` invocation gives no opportunity to inspect
the intermediate state.

### Example 2: cgroup Resource Limits (Phase 5)

```bash
# Memory limit: container OOM-killed when it exceeds 100MB
$ sudo ./minicontainer --pid --memory 100M /bin/sh -c \
    'head -c 200M /dev/zero | wc -c'
# Killed
$ echo $?
137                          # 128 + SIGKILL(9) = OOM kill

# CPU limit: 50% of one core
$ sudo ./minicontainer --pid --cpus 0.5 /bin/sh -c 'while true; do :; done' &
# Check with top — process shows ~50% CPU, not 100%
$ kill %1

# PID limit: fork bomb defense
$ sudo ./minicontainer --pid --pids 5 /bin/sh -c \
    'for i in $(seq 1 100); do sleep 1000 & done'
# /bin/sh: Cannot fork  ← kernel returns EAGAIN after pids.max hit

# Inspect cgroup directly while container runs
$ sudo ./minicontainer --pid --memory 100M --cpus 0.5 --pids 20 \
    /bin/sleep 60 &
$ cat /sys/fs/cgroup/minicontainer_*/memory.max
104857600
$ cat /sys/fs/cgroup/minicontainer_*/cpu.max
50000 100000
$ cat /sys/fs/cgroup/minicontainer_*/pids.max
20
```

**Three-phase lifecycle:** Before `clone()` the parent creates the cgroup
and writes limits. After `clone()` (and after UID/GID maps if `--user`) the
parent adds the child PID to `cgroup.procs`. After `waitpid()` returns, the
cgroup directory is `rmdir()`'d. Missed cleanup is a resource leak — see
Troubleshooting if `/sys/fs/cgroup/minicontainer_*` accumulates entries.

### Example 3: IPC Namespace Isolation (Phase 4c)

```bash
# Host: create a shared memory segment
$ ipcmk -M 1024
Shared memory id: 294929

# Without --ipc: container sees host IPC objects
$ sudo ./minicontainer --pid /bin/sh -c 'ipcs -m | grep 294929'
0xa5105110 294929     tcrumb     644        1024       0

# With --ipc: container sees empty IPC tables
$ sudo ./minicontainer --pid --ipc /bin/sh -c 'ipcs'
------ Message Queues --------
(empty)
------ Shared Memory Segments --------
(empty)
------ Semaphore Arrays --------
(empty)

# Clean up host IPC object
$ ipcrm -m 294929
```

**Note:** POSIX shared memory (`shm_open`) is isolated by the **mount
namespace** (Phase 2 `/dev/shm`), not by `CLONE_NEWIPC`. The IPC namespace
covers System V IPC and POSIX message queues only. See decisions.md for
details.

### Example 4: Hostname Isolation (Phase 4)

```bash
# Host hostname is unchanged
$ hostname
tcrumb-desktop

# Container gets its own hostname
$ sudo ./minicontainer --rootfs ./rootfs --hostname mycontainer \
    /bin/sh -c 'hostname'
mycontainer

# Host is still unchanged
$ hostname
tcrumb-desktop

# Combine with --overlay for full filesystem + hostname isolation
$ sudo ./minicontainer --rootfs ./rootfs --overlay --hostname mycontainer \
    /bin/sh -c 'hostname && echo "hello" > /tmp/test && cat /tmp/test'
mycontainer
hello
# rootfs is untouched, hostname was scoped to the container
```

### Example 5: OverlayFS — Base Image Untouched (Phase 3)

```bash
# Create a marker in the base image
$ echo "original" > rootfs/tmp/marker.txt

# Modify it inside the container (with overlay)
$ sudo ./minicontainer --rootfs ./rootfs --overlay /bin/sh -c \
    'echo "modified" > /tmp/marker.txt && cat /tmp/marker.txt'
modified

# Base image is untouched
$ cat rootfs/tmp/marker.txt
original
```

### Example 6: Clean Container Environment (Phase 3)

```bash
# Container sees minimal environment, not host's 30+ variables
$ sudo ./minicontainer --rootfs ./rootfs --overlay /bin/sh -c 'env'
HOME=/root
TERM=xterm
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
PWD=/

# Custom env variable with --env
$ sudo ./minicontainer --rootfs ./rootfs --overlay \
    --env MY_APP=production /bin/sh -c 'echo $MY_APP'
production
```

### Example 7: Debug Mode with Overlay (Phase 3)

```bash
$ sudo ./minicontainer --debug --rootfs ./rootfs --overlay /bin/echo "Hello"
[parent] Container environment:
[parent]   PATH=/usr/local/sbin:...
[parent]   HOME=/root
[parent]   TERM=xterm
[parent] Executing: /bin/echo Hello
[parent] Rootfs: ./rootfs
[parent] Overlay: enabled
[overlay] Container ID: a1b2c3d4e5f6
[overlay] Lower: .../rootfs
[overlay] Upper: .../containers/a1b2c3d4e5f6/upper
[overlay] Overlay mounted at .../containers/a1b2c3d4e5f6/merged
[child] PID: 1
[child] Closing inherited fd 3
Hello
[parent] Child exited: 0
[overlay] Removing upper layer: ...
[overlay] Cleanup complete
```

### Example 8: PID Namespace Isolation (Phase 1)

```bash
# Without --pid: shell reports its real PID
$ ./minicontainer /bin/sh -c 'echo $$'
5678

# With --pid: shell sees itself as PID 1
$ sudo ./minicontainer --pid /bin/sh -c 'echo $$'
1
```

### Example 9: Basic Command Execution

```bash
$ ./minicontainer /bin/echo "Hello from minicontainer"
Hello from minicontainer
```

### Example 10: Exit Code Handling

```bash
$ ./minicontainer /bin/false
$ echo $?
1

$ ./minicontainer /nonexistent/command
execve: No such file or directory
$ echo $?
127
```

---

## Testing

### Unit Tests

```bash
make test
```

**Phase 0 tests** (`test_spawn` - runs unprivileged):
- ✓ Basic execution (`/bin/true`)
- ✓ Exit code propagation (exit 42)
- ✓ Signal death (killed by SIGTERM)
- ✓ execve failure (command not found)

**Phase 1 tests** (`test_namespace` - requires root):
- ✓ PID namespace isolation (child is PID 1)
- ✓ Non-namespaced execution (fallback path)
- ✓ Stack allocation and cleanup

**Phase 2 tests** (`test_mount` - requires root + rootfs):
- ✓ Rootfs isolation (container sees only rootfs contents)
- ✓ /proc mount (proc filesystem mounted inside container)

**Phase 3 tests** (`test_overlay` - requires root + rootfs):
- ✓ Base image untouched after container writes (overlay)
- ✓ Overlay cleanup (directories removed on exit)
- ✓ Backward compatibility (no overlay, Phase 2 behavior)

**Phase 4 tests** (`test_uts` - requires root):
- ✓ Hostname isolation (container gets custom hostname)
- ✓ Backward compatibility (no UTS namespace, Phase 3 behavior)

**Phase 4b tests** (`test_uts` - unprivileged subset runs without root):
- ✓ User namespace unprivileged (container root maps to host user)
- ✓ User namespace + hostname (combined isolation without sudo)

**Phase 4c tests** (`test_uts` - mixed privileges):
- ✓ IPC isolation (requires root — container sees empty IPC tables)
- ✓ IPC + user namespace (rootless with IPC isolation)
- ✓ Backward compatibility (no IPC namespace, rootless Phase 4b behavior)

**Phase 5 tests** (`test_cgroup` - requires root):
- ✓ Cgroup creation and cleanup (mkdir/rmdir lifecycle)
- ✓ Memory limit enforcement (process exits cleanly within budget)
- ✓ PID limit (fork bomb defense — kernel returns EAGAIN at the cap)
- ✓ Backward compatibility (no cgroup, Phase 4c behavior)
- ✓ Cgroup combined with IPC namespace (all isolation layers active)

**Phase 6 tests** (`test_net` - requires root + iproute2):
- ✓ Network namespace creation (`CLONE_NEWNET` + veth pair, container sees `veth_c_<id>`)
- ✓ Backward compatibility (no `--net`, Phase 5 behavior preserved)
- ✓ Network namespace combined with cgroup (veth + memory/cpu/pids active simultaneously)

### Manual Testing

```bash
# Cgroup limits (Phase 5)
sudo ./minicontainer --pid --memory 100M /bin/sh -c 'head -c 200M /dev/zero | wc -c'
echo $?                           # Should be 137 (OOM killed)
sudo ./minicontainer --pid --pids 5 /bin/sh -c 'for i in 1 2 3 4 5 6 7 8 9; do sleep 1 & done'
                                  # Should print "Cannot fork" once limit hit
ls /sys/fs/cgroup/minicontainer_* # After exit: should be empty (auto-cleanup)

# IPC: container should see empty tables with --ipc
ipcmk -M 1024                     # Create a host shared memory segment
ipcs -m                           # Note the shmid
sudo ./minicontainer --pid --ipc /bin/sh -c 'ipcs'  # Should be empty
sudo ./minicontainer --pid /bin/sh -c 'ipcs -m'     # Should include the shmid
ipcrm -m <shmid>                  # Clean up

# Hostname: container should have custom hostname, host unchanged
hostname                          # Note the host's hostname
sudo ./minicontainer --pid --rootfs ./rootfs --hostname testbox \
    /bin/sh -c 'hostname'         # Should show "testbox"
hostname                          # Should still show original

# Overlay: base image should be untouched after container modifies a file
echo "original" > rootfs/tmp/marker.txt
sudo ./minicontainer --rootfs ./rootfs --overlay /bin/sh -c \
    'echo "modified" > /tmp/marker.txt'
cat rootfs/tmp/marker.txt    # Should show "original"

# Environment: container should have minimal env
sudo ./minicontainer --rootfs ./rootfs --overlay /bin/sh -c 'env'
# Should show only PATH, HOME, TERM, PWD

# File descriptors: container should have only fds 0, 1, 2
sudo ./minicontainer --rootfs ./rootfs --overlay /bin/sh -c \
    'ls -la /proc/self/fd'

# Misplaced flag detection
./minicontainer --rootfs ./rootfs /bin/sh --pid
# Should print error about --pid after command

# PID namespace: should print 1
sudo ./minicontainer --pid /bin/sh -c 'echo $$'

# Exit codes
sudo ./minicontainer --pid /bin/sh -c 'exit 42'
echo $?  # Should be 42
```

### Memory Leak Detection

```bash
make valgrind
```

Should report: "All heap blocks were freed -- no leaks are possible"

---

## Exit Code Conventions

Following shell/POSIX conventions:

| Exit Code | Meaning |
|-----------|---------|
| `0` | Success |
| `1-125` | Program-specific error |
| `126` | Command found but not executable |
| `127` | Command not found (execve failed) |
| `128+N` | Killed by signal N (e.g., 137 = SIGKILL) |

---

## Design Decisions

See [docs/decisions.md](docs/decisions.md) for detailed rationale on:
- **Phase 7a execution-core consolidation** — why five `*_exec()` functions collapse into one `container_exec()`, how the unified `container_config_t` / `container_context_t` / `container_result_t` are organized, what the `child_args_t` snapshot semantics buy you (Decision #30)
- **`user_ns_mapping_t` replaces the synthetic `uts_config_t` workaround** from Phase 5 — why the signature change to `setup_user_namespace_mapping()` is worth a dedicated five-field struct (Decision #31)
- **`find_ip_binary()` promoted from `static` to public** — the second half of the promotion (the `static` qualifier on the definition has to be dropped in lockstep with adding the declaration to `net.h`) (Decision #32)
- **Makefile `HELPER_OBJS` unified link chain** — one list, six rules; the consequence of Phase 7a's consolidation reaching the build system (Decision #33)
- **Helper `.c` cleanup lockstep with helper `.h` cleanup** — why removing a type from a header obligates removing the function bodies that referenced it from the same module's `.c` file (Error #18)
- Why `net.c` is a separate module (supersedes `cgroup.c`, imports its API)
- Why network setup uses `fork+exec` of `ip(8)` instead of raw netlink (pedagogical clarity > performance)
- Three-path `find_ip_binary()` search — same helper used pre-clone (host) and post-pivot_root (rootfs)
- Why `generate_veth_names()` runs BEFORE `clone()` (clone copies the parent's address space)
- Why the sync pipe is widened to fire on `enable_user_namespace OR enable_network` (veth move must finish before the child runs)
- Why `cleanup_net()` stores `nat_source_cidr` in the context (so cleanup doesn't need the original config)
- Why `cgroup.c` is a separate module (vs. extending `uts.c` like Phase 4b/4c)
- `build_container_env()` defensive refactor that enabled the Phase 7a extraction into `env.c` (Decision #21)
- Cgroup three-phase lifecycle (create before clone, add PID after, remove after exit)
- IPC namespace isolation and why it's explicit opt-in (not auto-enabled)
- Why IPC namespace ≠ all shared memory (POSIX `shm_open` is a mount namespace concern)
- User namespace support and why Phase 4b extends uts.c instead of creating a new module
- Proc mount hardening (`MS_NOSUID | MS_NODEV | MS_NOEXEC`) and graceful degradation in user namespaces
- Phase 3 → Phase 4 architecture transition (overlay → uts API)
- UTS namespace isolation and why sethostname() must run in the child
- OverlayFS layer management (split setup_overlay into init/create/mount)
- Why `close_inherited_fds()` is needed despite `sudo` masking fd leaks
- Mount hardening with `MS_NODEV | MS_NOSUID`
- Environment isolation via `build_container_env()` vs inheriting host environ
- Why pivot_root() instead of chroot()
- Mount propagation (MS_PRIVATE | MS_REC) before pivot_root
- Lazy unmount (MNT_DETACH) for old root and overlay cleanup
- Modular design (mount.c/overlay.c/uts.c/cgroup.c/net.c — helpers retained after Phase 7a consolidation)
- All errors found and fixed during implementation (Errors #1–#18)

---

## Known Limitations

1. **No PATH search** — Must use absolute paths (`/bin/ls`, not `ls`)
2. ~~**No FD management**~~ — **Fixed in Phase 3** via `close_inherited_fds()`
3. ~~**Environment variable leak**~~ — **Fixed in Phase 3** via `build_container_env()` and `--env`
4. ~~**No copy-on-write filesystem**~~ — **Fixed in Phase 3** via OverlayFS
5. ~~**No mount hardening**~~ — **Fixed in Phase 3** via `MS_NODEV | MS_NOSUID`
6. **Single command** — Runs one command then exits (no daemon mode)
7. ~~**Requires root for namespaces**~~ — **Mitigated in Phase 4b** via `CLONE_NEWUSER` and `--user` flag (rootless containers). `/proc` mount may be restricted by AppArmor in user namespace mode
8. **Rootfs must be pre-built** — No image pull or layer support; rootfs directory must exist before running
9. **No tmpfs mounts** — Only `/proc` is mounted automatically; `/tmp`, `/dev`, etc. are not set up
10. ~~**No hostname isolation**~~ — **Fixed in Phase 4** via `CLONE_NEWUTS` and `--hostname`
11. ~~**No resource limits**~~ — **Fixed in Phase 5** via cgroups v2 (`--memory`, `--cpus`, `--pids`). Cgroup operations require root (`/sys/fs/cgroup/` is root-owned); `--user` does not grant write access there.
12. ~~**No network isolation**~~ — **Fixed in Phase 6** via `CLONE_NEWNET` + veth pair (`--net`). Network setup uses `fork+exec` of `ip(8)` (and optionally `iptables`), so the host must have `iproute2` installed; the rootfs must include `/bin/ip` (handled by `BINS` in `build_rootfs.sh`). `--net` requires root because veth creation, namespace move, and iptables manipulation all need `CAP_NET_ADMIN` in the root user namespace.

---

## Roadmap

### ✅ Phase 0: Foundation
- [x] fork/execve pattern
- [x] Signal handling (SIGCHLD)
- [x] Exit status parsing
- [x] Environment variables
- [x] Unit tests

### ✅ Phase 1: PID Namespace
- [x] Replace fork() with clone(CLONE_NEWPID)
- [x] Child sees itself as PID 1
- [x] Manual stack allocation for clone()
- [x] Namespace cleanup (stack deallocation)
- [x] Unit tests (requires root)

### ✅ Phase 2: Mount Namespace & Filesystem Isolation
- [x] CLONE_NEWNS for mount isolation
- [x] pivot_root() to custom rootfs
- [x] Mount /proc inside container
- [x] --rootfs flag
- [x] Unit tests (requires root + rootfs)

### ✅ Phase 3: OverlayFS & Security Corrections
- [x] OverlayFS copy-on-write filesystem (read-only base image + writable upper layer)
- [x] Per-container writable layers with automatic cleanup on exit
- [x] `--overlay` and `--container-dir` CLI flags
- [x] `build_container_env()` — clean container environment with `--env` support
- [x] `close_inherited_fds()` — close all inherited parent fds before `execve()`
- [x] Mount overlay with `MS_NODEV | MS_NOSUID`
- [x] Misplaced flag detection
- [x] `--rootfs` auto-enables `--pid`
- [x] Unit tests (requires root + rootfs)

### ✅ Phase 4: UTS Namespace — Hostname Isolation
- [x] `CLONE_NEWUTS` for hostname isolation
- [x] `--hostname <name>` flag (auto-enables UTS namespace)
- [x] `sethostname()` in child — host hostname unchanged
- [x] Unit tests (requires root)

### ✅ Phase 4b: User Namespace — Rootless Containers
- [x] `CLONE_NEWUSER` for unprivileged containers
- [x] UID/GID mapping via `/proc/<pid>/uid_map` and `gid_map`
- [x] Parent-child synchronization pipe
- [x] `setgroups deny` security requirement
- [x] `--user` CLI flag (maps container root to host user)
- [x] Proc mount hardening (`MS_NOSUID | MS_NODEV | MS_NOEXEC`)
- [x] Graceful `/proc` mount degradation in user namespace
- [x] `close_inherited_fds()` brute-force fallback when `/proc` unavailable

### ✅ Phase 4c: IPC Namespace — System V IPC Isolation
- [x] `CLONE_NEWIPC` for independent IPC tables
- [x] `--ipc` CLI flag (explicit opt-in, not auto-enabled)
- [x] System V shared memory / semaphores / message queues isolated
- [x] POSIX message queues isolated (via `/dev/mqueue` in new namespace)
- [x] Unit tests (`test_ipc_isolation` root; `test_ipc_with_user_namespace` rootless)

### ✅ Phase 5: cgroups v2 — Resource Limits
- [x] New `cgroup.c`/`cgroup.h` module supersedes `uts.c` as exec path
- [x] Three-phase cgroup lifecycle: create → add PID → remove
- [x] `memory.max` limit (`--memory <100M|1G>`); OOM kill at exit code 137
- [x] `cpu.max` limit (`--cpus 0.5` = 50% of one core)
- [x] `pids.max` limit (`--pids 20`) for fork bomb defense
- [x] Auto-enable `enable_cgroup` when any limit flag is set
- [x] `build_container_env()` defensive refactor (decisions.md #21)
- [x] Unit tests (`test_cgroup` requires root)

### ✅ Phase 6: Network Namespace
- [x] New `net.c`/`net.h` module supersedes `cgroup.c` as exec path
- [x] `CLONE_NEWNET` — container gets its own loopback, routing table, interfaces
- [x] veth pair created via `ip link add`; container end moved with `ip link set ... netns <pid>`
- [x] `--net` flag plus `--net-host-ip` / `--net-container-ip` / `--net-netmask` / `--no-nat`
- [x] Default subnet `10.0.0.0/24` (host `10.0.0.1`, container `10.0.0.2`)
- [x] Optional `iptables -t nat -A POSTROUTING -j MASQUERADE` for outbound internet
- [x] `find_ip_binary()` three-path search (`/sbin/ip` → `/usr/sbin/ip` → `/bin/ip`)
- [x] `generate_veth_names()` runs **before** `clone()` (clone copies address space)
- [x] Sync pipe widened to fire on `enable_user_namespace OR enable_network`
- [x] `cleanup_net()` deletes host veth + iptables rule (kernel handles container end at netns destruction)
- [x] Unit tests (`test_net` requires root + iproute2)

### ✅ Phase 7a: Execution-Core Consolidation (Current)
- [x] New `core.c`/`core.h` module — unified `container_exec()`, `container_cleanup()`, `container_config_t`, `container_context_t`, `container_result_t`
- [x] Five duplicate `child_func` / `close_inherited_fds` / `child_args_t` copies collapsed to one canonical definition each in `core.c`
- [x] New `env.c`/`env.h` — `build_container_env()` extracted from `main.c` for reuse by tests and (future) subcommands
- [x] `user_ns_mapping_t` (in `uts.h`) replaces the synthetic-`uts_config_t` workaround from Phase 5
- [x] `find_ip_binary()` promoted from `static` to public (declared in `net.h`) so `core.c`'s early-failure check can call it
- [x] Helper headers slimmed: `mount.h`, `overlay.h`, `uts.h`, `cgroup.h`, `net.h` drop their per-phase `*_config_t` / `*_result_t` / `*_exec()` / `*_cleanup()` declarations
- [x] Helper sources slimmed in lockstep: each `.c` drops its file-local `child_args_t`, `static child_func()`, `static close_inherited_fds()`, `*_exec()`, `*_cleanup()`
- [x] `spawn.c` / `namespace.c` / `test_spawn.c` / `test_namespace.c` deleted entirely
- [x] All Phase 2-6 tests migrated to `container_exec()` / `container_config_t`; local `build_container_env()` stubs replaced by `#include "env.h"`
- [x] New `test_core.c` covers the bare-exec and PID-only cases (former `test_spawn` / `test_namespace` coverage)
- [x] Makefile `HELPER_OBJS` unified link chain — every test links against the same helper set as `minicontainer` (minus `main.o`, plus the test's own `.o`)
- [x] Zero new user-visible features — pure refactor; every Phase 0-6 test passes behaviorally identical

### 📋 Phase 7b: CLI & Lifecycle + Bind Mounts
- [ ] Subcommand dispatch (`run` / `start` / `stop` / `exec` / `inspect` / `list` / `cleanup`)
- [ ] Container state files (`/run/minicontainer/<id>/state.json` + pidfile, `$XDG_RUNTIME_DIR/...` for rootless)
- [ ] `container_exec()` split into `container_start()` + `container_wait()` so state can be written between the two halves
- [ ] `--volume host:container[:ro]` bind mounts
- [ ] `--interactive` PTY allocation
- [ ] `cleanup` subcommand scans for stale containers

### 📋 Phase 8: Inspector + Hardening + OCI Bundles
- [ ] **8a:** Static `libprocfs.a` extracted as a sibling project; cgroup read-side + `/proc` parsing reused by container `inspect` / `stats` / `top` / `netstat` subcommands
- [ ] **8b:** Production hardening — opt-in `--secure` flag: capability dropping (raw `capset(2)`, no libcap), `PR_SET_NO_NEW_PRIVS`, seccomp BPF allow-list (raw `prctl(PR_SET_SECCOMP)`, no libseccomp), read-only `/sys`, atomic state-file writes, container ID collision retry
- [ ] **8c:** OCI Runtime Spec compliance — hand-rolled `config.json` parser, `--bundle <dir>` flag, minimal `pull <tag>` whitelist (alpine/ubuntu, explicitly not a registry client), `oci-state.json` round-trip with runc-compatible tools

---

## Development

### Build Targets

```bash
make              # Build minicontainer
make test         # Build and run all tests (Phase 0 + 1 + 2 + 3 + 4 + 4b + 4c + 5 + 6)
make clean        # Remove build artifacts
make debug        # Build with debug symbols (-g)
make valgrind     # Run memory leak detection
make examples     # Run demonstration commands (includes rootfs examples)
make help         # Show all available targets
```

### Compiler Flags

```
-Wall -Wextra           # Enable all warnings
-std=c11                # C11 standard
-D_GNU_SOURCE           # Enable clone(), CLONE_NEWPID, etc.
-D_POSIX_C_SOURCE       # Enable POSIX features
-I./include             # Header search path
```

### Code Style

- Follow Linux kernel style (mostly)
- Use snake_case for functions and variables
- Use typedef for structs (e.g., `spawn_config_t`)
- Comment all non-obvious code
- Use const correctness

---

## Learning Resources

### Man Pages (Essential Reading)
```bash
man 7 network_namespaces # Network namespace details (Phase 6)
man 8 ip                 # iproute2 — link/addr/route/netns subcommands (Phase 6)
man 8 iptables           # NAT (MASQUERADE), filter rules (Phase 6 outbound)
man 8 iptables-extensions # nat table specifics (Phase 6)
man 7 cgroups         # cgroup overview, v1 vs v2, controllers (Phase 5)
man 5 cgroup.controllers # File interface for memory.max/cpu.max/pids.max (Phase 5)
man 7 ipc_namespaces  # IPC namespace details (Phase 4c)
man 7 sysvipc         # System V IPC overview — shmget/semget/msgget (Phase 4c)
man 7 user_namespaces # User namespace details — UID/GID mapping (Phase 4b)
man 2 sethostname     # Set hostname (Phase 4 — UTS namespace isolation)
man 7 uts_namespaces  # UTS namespace details (Phase 4)
man 2 mount           # Mount filesystem — see overlay type and MS_NODEV/MS_NOSUID
man 2 umount2         # Unmount filesystem with flags (MNT_DETACH)
man 2 getrlimit       # Get fd limit for close_inherited_fds fallback (Phase 3)
man 2 pivot_root      # Change the root filesystem (Phase 2)
man 7 mount_namespaces  # Mount namespace details (Phase 2)
man 2 clone           # Process creation with namespaces
man 2 fork            # Process creation (Phase 0)
man 2 execve          # Execute program
man 2 waitpid         # Wait for process
man 7 namespaces      # Overview of Linux namespaces
man 7 pid_namespaces  # PID namespace details
```

### Recommended Books
- **Linux System Programming** by Robert Love
- **The Linux Programming Interface** by Michael Kerrisk
- **Advanced Programming in the UNIX Environment** by Stevens & Rago

### Online Resources
- [LWN: Namespaces in operation](https://lwn.net/Articles/531114/)
- [Docker Internals](https://www.docker.com/blog/how-docker-works/)
- [OCI Runtime Specification](https://github.com/opencontainers/runtime-spec)

---

## Troubleshooting

### "find_ip_binary: not found" (Phase 6)

**Problem:** `net.c` cannot locate the `ip` binary at any of `/sbin/ip`,
`/usr/sbin/ip`, or `/bin/ip`.

**Cause:** Either iproute2 is missing on the host, or the rootfs is missing
`/bin/ip` (which is where `scripts/build_rootfs.sh` places it). The same
helper is called twice: once parent-side against the host, once child-side
against the rootfs (post-pivot_root), so a failure on either side trips the
same error.

**Solution:**
```bash
# Host
sudo apt-get install iproute2

# Rootfs: rebuild so `ip` ends up in /bin/ip
./scripts/build_rootfs.sh
ls rootfs/bin/ip   # should exist
```

`BINS=(... ip ...)` in `build_rootfs.sh` is the canonical place for this;
removing `ip` from `BINS` will break Phase 6.

---

### "RTNETLINK answers: File exists" when creating veth (Phase 6)

**Problem:** `ip link add` fails because a veth with the chosen name already
exists. minicontainer generates names like `veth_h_<6 hex>` from
`/dev/urandom`, so collisions are astronomically unlikely; if you see this,
something else is wrong.

**Causes:**
1. A previous container crashed mid-setup and left a stale `veth_h_<id>`.
2. Another process is holding the name.

**Solution:**
```bash
# Find and remove stale veths
ip link show | grep veth_h_
sudo ip link delete veth_h_a1b2  # by name

# As a nuclear option, remove every minicontainer veth
for v in $(ip -o link show | awk -F: '/veth_h_/ {print $2}'); do
    sudo ip link delete $v
done
```

A `minicontainer cleanup` subcommand (Phase 7) will handle this automatically.

---

### `--net` works but container has no internet (Phase 6)

**Problem:** Container can ping `10.0.0.1` (the host end of the veth) but
external IPs (e.g., `8.8.8.8`) time out.

**Causes:**
1. `--no-nat` was passed — by design no `MASQUERADE` rule is added.
2. Host's `net.ipv4.ip_forward` is 0 (kernel won't forward across interfaces).
3. A host firewall (`ufw`, `firewalld`, custom `iptables`) is dropping
   forwarded traffic.

**Solution:**
```bash
# Enable IP forwarding (transient)
sudo sysctl -w net.ipv4.ip_forward=1

# Or persistently: /etc/sysctl.d/10-ip-forward.conf
echo 'net.ipv4.ip_forward = 1' | sudo tee /etc/sysctl.d/10-ip-forward.conf

# Inspect firewall
sudo iptables -L FORWARD -v
sudo iptables -t nat -L POSTROUTING -v
```

---

### "Error: --net-host-ip ... require --net" (Phase 6)

**Problem:** You passed `--net-host-ip` / `--net-container-ip` /
`--net-netmask` / `--no-nat` without `--net`.

**This is intentional.** Silently ignoring sub-flags would mask typos. Either
add `--net`, or drop the sub-flags.

---

### IP-pair conflicts with an existing network

**Problem:** Default `10.0.0.0/24` overlaps a VPN, Docker bridge, or LAN.

**Solution:** Pick a different subnet:
```bash
sudo ./minicontainer --pid --rootfs ./rootfs --net \
    --net-host-ip 172.30.0.1 --net-container-ip 172.30.0.2 \
    --net-netmask 30 /bin/sh
```

A `/30` is the smallest useful pair (network/broadcast eat two addresses,
leaving exactly two host addresses — one for the host end, one for the
container end).

---

### "mount(overlay): No such device"

**Problem:** Overlay mount fails because the kernel module is not loaded.

**Solution:**
```bash
# Check if overlay is supported
cat /proc/filesystems | grep overlay

# If not listed, load the module
sudo modprobe overlay
```

---

### "mkdir(cgroup): Permission denied" (Phase 5)

**Problem:** Cgroup creation requires write access to `/sys/fs/cgroup/`,
which is root-owned. `--user` does **not** delegate this.

**Solution:** Always use `sudo` for cgroup operations:
```bash
sudo ./minicontainer --pid --memory 100M /bin/sh
```

This is a known limitation matching Docker/Podman's "cgroup manager"
delegation problem. A production runtime would use systemd's `--scope`
delegation; minicontainer keeps the mechanism direct for educational clarity.

---

### "rmdir(cgroup): Device or resource busy" (Phase 5)

**Problem:** Cgroup cleanup fails because processes still exist in
`cgroup.procs`.

**Cause:** A subprocess outlived the container's primary process, or a
zombie hasn't been reaped, or the test exited via signal.

**Solution:**
```bash
# Find stale cgroup
ls /sys/fs/cgroup/minicontainer_*

# Check what's still there
cat /sys/fs/cgroup/minicontainer_XXX/cgroup.procs

# Kill remaining processes
for pid in $(cat /sys/fs/cgroup/minicontainer_XXX/cgroup.procs); do
    sudo kill -9 $pid
done

# Then remove
sudo rmdir /sys/fs/cgroup/minicontainer_XXX
```

Phase 7 will add a `minicontainer cleanup` subcommand to handle this.

---

### Container exits with code 137 (Phase 5)

**Problem:** Container terminated unexpectedly with exit code 137.

**Cause:** Exit code 137 = 128 + signal 9 (SIGKILL). The OOM killer fired —
the container exceeded `--memory` and the kernel killed it.

**This is correct behavior.** It's how `--memory` enforcement works. Either
raise the limit or check what the container is allocating:
```bash
# Inspect peak memory usage
cat /sys/fs/cgroup/minicontainer_*/memory.peak  # If kernel ≥ 5.19
```

---

### "clone: Operation not permitted" with --ipc

**Problem:** `clone(CLONE_NEWIPC)` requires `CAP_SYS_ADMIN`. Without `--user`,
this means the command needs sudo.

**Solution:** Either use sudo, or combine `--ipc` with `--user` so the user
namespace grants `CAP_SYS_ADMIN` inside the namespace:
```bash
# Option 1: sudo
sudo ./minicontainer --pid --ipc /bin/sh

# Option 2: rootless via --user
./minicontainer --user --pid --ipc /bin/sh
```

---

### POSIX shared memory still visible with --ipc

**Problem:** `ls /dev/shm` inside the container shows host POSIX shared memory
objects even with `--ipc`.

**Cause:** POSIX shared memory (`shm_open`) is backed by `tmpfs` at
`/dev/shm` and is isolated by the **mount namespace** (Phase 2), not the IPC
namespace. `CLONE_NEWIPC` only isolates System V IPC and POSIX message queues.

**Solution:** Add `--rootfs` to enable the mount namespace:
```bash
sudo ./minicontainer --pid --ipc --rootfs ./rootfs /bin/sh
```

---

### "/proc mount denied (user namespace restriction)"

**Problem:** When using `--user --rootfs`, you see:
```
[child] Warning: /proc mount denied (user namespace restriction) — continuing without /proc
```

**This is expected.** The kernel (or AppArmor on Ubuntu) restricts proc mounts
from within unprivileged user namespaces. The container continues without
`/proc` — most commands work fine. Commands that read `/proc` (like `ps`) will
not work. The `close_inherited_fds()` function falls back to brute-force fd
closing (3 through `RLIMIT_NOFILE`) when `/proc/self/fd` is unavailable.

---

### "sethostname: Operation not permitted"

**Symptom:**

```
$ ./minicontainer --user --pid --hostname mycontainer /bin/sh -c 'id && hostname'
sethostname: Operation not permitted
[child] Failed to setup UTS
```

This bites the rootless flow specifically — every `--user --hostname …`
example in the quickstart trips it. The first thing to check is which
of two distinct causes is biting you.

#### Cause A — Ubuntu 24.04 (and newer) restricts unprivileged user namespaces

If you're on Ubuntu 24.04+ and the symptom appears only when you add
`--user`, AppArmor is mediating your unprivileged user namespace.
Ubuntu 24.04 ships with `kernel.apparmor_restrict_unprivileged_userns=1`.
When an *unconfined* process creates a new user namespace, the kernel
attaches a default AppArmor profile that masks `CAP_SYS_ADMIN` inside
the new namespace — so `sethostname(2)` returns `EPERM` even though
the child is "root" within its own userns and `CLONE_NEWUTS` was set
correctly.

**Verify:**

```bash
cat /etc/os-release | grep PRETTY_NAME
# PRETTY_NAME="Ubuntu 24.04.4 LTS"

cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns
# 1 — restriction is active

cat /proc/self/attr/current
# unconfined  ← restriction will be applied when you create a userns
# (a value like "snap.code.code (complain)" means you're already
#  inside a complain-mode profile and the restriction is bypassed —
#  which is why the same command works in a VS Code terminal)
```

**Fix (recommended) — install the shipped AppArmor profile:**

```bash
make install-apparmor       # asks for sudo once
```

This renders `scripts/apparmor/minicontainer.profile.in` with the
binary's absolute path, installs it to `/etc/apparmor.d/minicontainer`,
and loads it via `apparmor_parser -r`. The profile declares `userns,`
and otherwise leaves the binary unconfined — the same pattern Docker,
Podman, Chromium, and Firefox use for their unprivileged-userns needs.
This preserves the host's system-wide userns mediation for every other
program. Remove with `make uninstall-apparmor`. If you move or rename
the binary, re-run `make install-apparmor` so the profile's path
matches.

**Alternatives** (in roughly decreasing order of preference):

```bash
# Option A — Run from a process already under a complain-mode AppArmor
#   profile (VS Code's integrated terminal qualifies on a default
#   Ubuntu install). Useful for one-off experiments, not a real fix.

# Option B — Use sudo. Defeats the rootless goal but works everywhere.
sudo ./minicontainer --pid --hostname mycontainer /bin/sh -c 'id && hostname'

# Option C — Disable the mediation host-wide. NOT RECOMMENDED.
#   This lowers the system security baseline for every program on the
#   host (including programs that exploit unprivileged-userns bugs);
#   it's the wrong knob to turn for one binary's needs.
sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0
```

The code's step ordering (sync wait on UID/GID map → `setup_uts`) is
correct; this is not a bug to reorder around. The kernel's mediation
is opaque to `sethostname` itself, which just sees `EPERM`. See Error
#19 in `docs/decisions.md` for the longer write-up.

#### Cause B — `--hostname` is not actually enabling `CLONE_NEWUTS`

This shouldn't happen with the standard build (the `--hostname` flag
auto-enables `enable_uts_namespace`) but is the right thing to check
when the symptom appears *without* `--user`:

1. Confirm `--hostname <name>` is on the command line (not a typo like `-hostname`).
2. Confirm you're running the freshly built binary (`make clean && make` then
   `./minicontainer …` from `minicontainer/`).
3. With `--debug`, the parent prints `[parent] Creating UTS namespace` before
   `clone()` — its absence means `enable_uts_namespace` was false.

If `CLONE_NEWUTS` is set, the child is in a new UTS namespace and
`sethostname` should succeed — under root, or under `--user` once one
of Cause A's workarounds is in place.

---

### "pivot_root: Invalid argument"

**Problem:** `pivot_root` fails with `EINVAL`.

**Common causes:**
1. The rootfs path is not a mount point. `setup_rootfs()` handles this with a bind mount, but verify your rootfs path exists and is a valid directory.
2. The current root has shared propagation. The code applies `MS_PRIVATE | MS_REC` to `/` before pivoting, but if something overrides this, pivot_root will fail.

---

### "mount(proc): No such file or directory"

**Problem:** `/proc` doesn't exist inside the rootfs.

**Solution:** Create the directory in your rootfs:
```bash
mkdir -p ./rootfs/proc
```

---

### Container still sees host processes

**Problem:** `ps` inside the container shows host processes.

**Check:**
1. Are you using `--rootfs`? Without it, /proc is not remounted
2. Does the rootfs have a `/proc` directory?
3. Are both `--pid` and `--rootfs` flags set? You need PID namespace + mount namespace for proper process isolation

---

### "Operation not permitted" with --pid or --rootfs

**Problem:** `clone: Operation not permitted`

**Solution:** Namespaces require `CAP_SYS_ADMIN`:
```bash
sudo ./minicontainer --pid --rootfs ./rootfs /bin/sh
# or
sudo setcap cap_sys_admin+ep ./minicontainer
```

---

### PPID is 0 inside container

**Problem:** `getppid()` returns 0 inside the namespace.

**This is expected.** The parent is in a different PID namespace, so it's not visible to the child. PID 0 means "parent not visible in this namespace."

---

### "Command not found" error

**Problem:** `execve: No such file or directory`

**Solution:** Use absolute path: `/bin/ls` instead of `ls`. When using `--rootfs`, the command must exist inside the rootfs.

---

### Segmentation fault in child

**Problem:** Crash when clone'd child starts executing.

**Check:**
1. Is the stack pointer correct? Must pass `stack + STACK_SIZE` (top), not `stack` (base)
2. Is argv NULL-terminated? `char *argv[] = {"/bin/ls", NULL};`
3. Run with `valgrind` to find exact crash location

---

### Linker errors after phase transition

**Problem:** `undefined reference to 'container_exec'` / `undefined reference to 'build_container_env'`

**Solution:** Update the Makefile to compile and link the new source files. Phase 7a centralized this via a `HELPER_OBJS` variable that every test target reuses; if your link rule predates Phase 7a, add `$(BUILD_DIR)/core.o $(BUILD_DIR)/env.o` to its prerequisite list. See decisions.md Decision #33 for the rationale.

---

### Linker errors after deleting per-phase types from a header

**Problem:** After slimming a helper header (e.g., removing `net_config_t` and `net_result_t` from `net.h`), the build fails with `error: unknown type name 'net_config_t'` inside `net.c` — even though those types are no longer used externally.

**Cause:** A per-phase `*_exec()` body in the corresponding `.c` file still references the type via its signature or local variable. Phase 7a consolidates `*_exec()` into `core.c::container_exec()`, so the per-phase exec bodies are no longer reachable — but the compiler still tries to compile them as long as they sit in the helper's `.c` file.

**Solution:** When you remove a `*_config_t` / `*_result_t` / `*_exec()` / `*_cleanup()` declaration from a helper header, delete its function body from the matching `.c` file in the same commit. The two have to move together. The file-local `child_args_t`, `static child_func()`, and `static close_inherited_fds()` in the same `.c` file go with them — they only existed to serve the now-deleted exec. See decisions.md Error #18 for the full case and the audit grep that catches stragglers.

---

## Contributing

This is an educational project, but contributions are welcome!

**Before submitting PR:**
1. Run `make test` - all tests must pass
2. Run `make valgrind` - no memory leaks
3. Follow existing code style
4. Update documentation if adding features

---

## License

MIT License - See LICENSE file for details

---

## Acknowledgments

- Inspired by Docker, runc, and Kubernetes
- Thanks to the Linux kernel developers for excellent documentation

---

## Contact

For questions or issues, please open a GitHub issue.

---

**Happy containerizing! 🐳**
