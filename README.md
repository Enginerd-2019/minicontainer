# minicontainer

> A minimal container runtime built from scratch to understand the internals of Docker and Kubernetes

[![Phase](https://img.shields.io/badge/Phase-5%20cgroups%20v2-blue)]()
[![License](https://img.shields.io/badge/License-MIT-green)]()
[![C Standard](https://img.shields.io/badge/C-C11-orange)]()

---

## Overview

**minicontainer** is an educational project that implements a container runtime from first principles. Instead of using Docker or other high-level tools, this project builds process isolation step-by-step using low-level Linux system calls.

**Current Phase:** Phase 5 - cgroups v2 (Resource Limits)

Building on Phase 4c's complete namespace isolation, this phase adds **resource control** via cgroups v2. Containers can now be limited in memory (`memory.max`), CPU bandwidth (`cpu.max`), and process count (`pids.max`) — defending the host from runaway containers, fork bombs, and OOM-killer chaos. Namespaces answer "what can the container see?"; cgroups answer "how much can it use?"

---

## Features

### Phase 5 (Current)

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

### Run

```bash
# Basic usage (no namespace)
./minicontainer /bin/ls -la

# Rootless container (no sudo — user namespace maps UID 0 to host user)
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
# Run all tests (Phase 0 + 1 + 2 + 3 + 4 + 4b + 4c + 5)
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
│   ├── cgroup.h             # Phase 5: cgroups v2 resource limits API
│   ├── uts.h                # Phase 4/4b/4c: UTS + user + IPC namespace API
│   ├── overlay.h            # Phase 3: OverlayFS, environment, fd cleanup API
│   ├── mount.h              # Phase 2: Mount namespace & rootfs API
│   ├── namespace.h          # Phase 1: PID namespace isolation API
│   └── spawn.h              # Phase 0: Process spawning API
├── src/
│   ├── main.c               # CLI entry point and argument parsing
│   ├── cgroup.c             # Phase 5: cgroup setup/teardown, clone/exec (supersedes uts.c)
│   ├── uts.c                # Phase 4/4b/4c: UTS + user + IPC namespace, sync pipe
│   ├── overlay.c            # Phase 3: OverlayFS, close_inherited_fds
│   ├── mount.c              # Phase 2: setup_rootfs, mount_proc
│   ├── namespace.c          # Phase 1: clone() + PID namespace logic
│   └── spawn.c              # Phase 0: fork/execve implementation
├── tests/
│   ├── test_cgroup.c        # Phase 5: cgroup lifecycle, memory/CPU/PID limits (requires root)
│   ├── test_uts.c           # Phase 4/4b/4c: UTS + user + IPC namespace tests
│   ├── test_overlay.c       # Phase 3: Overlay/env/fd tests (requires root + rootfs)
│   ├── test_mount.c         # Phase 2: Mount/rootfs tests (requires root + rootfs)
│   ├── test_namespace.c     # Phase 1: Namespace tests (requires root)
│   └── test_spawn.c         # Phase 0: Spawn tests
├── scripts/
│   └── build_rootfs.sh      # Builds minimal rootfs from host binaries
├── docs/
│   └── decisions.md         # Design decisions and error log
├── Makefile                 # Build system
└── README.md                # This file
```

---

## Architecture

### Module Separation

```
┌──────────────────────────────────────────────────────┐
│                main.c (CLI Layer)                    │
│  • Parses --pid, --rootfs, --overlay, --hostname,    │
│    --memory, --cpus, --pids, etc.                    │
│  • build_container_env() — clean environment         │
│  • Misplaced flag detection                          │
│  • Calls cgroup_exec()                               │
└─────────────────────┬────────────────────────────────┘
                      │
                      ▼
┌──────────────────────────────────────────────────────┐
│          cgroup.c (Orchestration Layer)              │
│  • setup_cgroup() → mkdir /sys/fs/cgroup/...         │
│    write memory.max, cpu.max, pids.max               │
│  • setup_overlay() → mount OverlayFS (from overlay.c)│
│  • clone() with NEWPID|NEWNS|NEWUTS|NEWUSER|NEWIPC   │
│  • Sync pipe: write UID/GID maps, signal child       │
│  • add_pid_to_cgroup() → write cgroup.procs          │
│  • waitpid() and exit status parsing                 │
│  • teardown_overlay() → unmount and cleanup          │
│  • remove_cgroup() → rmdir /sys/fs/cgroup/...        │
└─────────────────────┬────────────────────────────────┘
                      │
                      ▼
┌──────────────────────────────────────────────────────┐
│            Child Process (child_func)                │
│  • Wait on sync pipe (if --user) for UID/GID maps    │
│  • setup_uts(): sethostname() in new UTS namespace   │
│  • setup_rootfs(): pivot_root to new rootfs          │
│  • mount_proc(): mount /proc (warning if user ns)    │
│  • close_inherited_fds(): close leaked parent fds    │
│  • execve() with clean container environment         │
└──────────────────────────────────────────────────────┘
```

**Phase transitions:**
- Phase 0: `spawn.c` (`fork/execve`)
- Phase 1: `namespace.c` (`clone(CLONE_NEWPID)`)
- Phase 2: `mount.c` (`clone(CLONE_NEWPID | CLONE_NEWNS)` + `pivot_root`)
- Phase 3: `overlay.c` (OverlayFS + env isolation + fd cleanup), calls into `mount.c` for `setup_rootfs`/`mount_proc`
- Phase 4: `uts.c` (`CLONE_NEWUTS` + `sethostname`), calls into `overlay.c` and `mount.c`
- Phase 4b: `uts.c` extended with `CLONE_NEWUSER`, sync pipe, UID/GID mapping (no new module — see code duplication note)
- Phase 4c: `uts.c` extended with `CLONE_NEWIPC` (one flag, zero new child-side code)
- Phase 5: `cgroup.c` (cgroups v2 resource limits), supersedes `uts.c` as exec module, imports `setup_uts`/`setup_user_namespace_mapping` from `uts.h` and overlay/mount helpers
- Earlier modules retained for their respective phase tests

**Note on code duplication:** Each phase's `*_exec()` function (e.g., `overlay_exec`, `uts_exec`) shares ~90% of its structure with the previous phase — `clone()`, `waitpid()`, stack allocation, and exit status parsing are repeated each time. This is a deliberate pedagogical choice: each module is self-contained and readable without cross-referencing earlier phases. A production runtime would factor this into a shared execution core, and this refactoring is planned for Phase 7 (CLI & Lifecycle).

### API Design

**Configuration structure:**
```c
cgroup_config_t config = {
    .program = "/bin/sh",
    .argv = argv,                     // NULL-terminated array
    .envp = env,                      // From build_container_env()
    .enable_debug = false,
    .enable_pid_namespace = true,     // Use CLONE_NEWPID
    .enable_mount_namespace = true,   // Use CLONE_NEWNS
    .rootfs_path = "./rootfs",        // NULL = no rootfs change
    .enable_overlay = true,           // Use OverlayFS on top of rootfs
    .container_dir = NULL,            // NULL = default "./containers"
    .enable_uts_namespace = true,     // Use CLONE_NEWUTS
    .hostname = "mycontainer",        // NULL = no hostname change
    .enable_user_namespace = true,    // Use CLONE_NEWUSER (rootless)
    .enable_ipc_namespace = true,     // Use CLONE_NEWIPC (IPC isolation)
    .uid_map_inside = 0,              // Container root...
    .uid_map_outside = getuid(),      // ...maps to host user
    .uid_map_range = 1,
    .gid_map_inside = 0,
    .gid_map_outside = getgid(),
    .gid_map_range = 1,
    // Phase 5: cgroup resource limits
    .enable_cgroup = true,
    .cgroup_limits = {
        .memory_limit = 100 * 1024 * 1024,  // 100 MB
        .cpu_quota = 50000,                  // 50% of one core
        .cpu_period = 100000,                // (over 100ms period)
        .pid_limit = 20                      // Max 20 processes
    }
};
```

**Result structure:**
```c
cgroup_result_t result = cgroup_exec(&config);

if (result.exited_normally) {
    printf("Exit code: %d\n", result.exit_status);
} else {
    printf("Killed by signal %d\n", result.signal);  // 137 = OOM-killed
}

cgroup_cleanup(&result);  // Remove cgroup dir + free clone stack
```

---

## How It Works

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

### The overlay_exec Pattern (Phase 3)

```
Parent Process                   Child Process (new PID + mount namespace)
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
     │ overlay_cleanup()
     │ (Frees clone stack)
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
| `clone()` | Creates child with namespace flags (`NEWPID`, `NEWNS`, `NEWUTS`, `NEWUSER`, `NEWIPC`) | 1–4c |
| `fork()` | Creates a new process (duplicate of parent) | 0 |
| `execve()` | Replaces process image with new program | 0–4c |
| `waitpid()` | Waits for child to exit and reaps zombie | 0–4c |
| `sigaction()` | Installs SIGCHLD handler to prevent zombies | 0 |

---

## Usage Examples

### Example 1: cgroup Resource Limits (Phase 5)

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

### Example 2: IPC Namespace Isolation (Phase 4c)

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

### Example 3: Hostname Isolation (Phase 4)

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

### Example 4: OverlayFS — Base Image Untouched (Phase 3)

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

### Example 5: Clean Container Environment (Phase 3)

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

### Example 6: Debug Mode with Overlay (Phase 3)

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

### Example 7: PID Namespace Isolation (Phase 1)

```bash
# Without --pid: shell reports its real PID
$ ./minicontainer /bin/sh -c 'echo $$'
5678

# With --pid: shell sees itself as PID 1
$ sudo ./minicontainer --pid /bin/sh -c 'echo $$'
1
```

### Example 8: Basic Command Execution

```bash
$ ./minicontainer /bin/echo "Hello from minicontainer"
Hello from minicontainer
```

### Example 9: Exit Code Handling

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
- Why `cgroup.c` is a separate module (vs. extending `uts.c` like Phase 4b/4c)
- `build_container_env()` defensive refactor for Phase 7 extraction (Decision #21)
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
- Modular design (spawn.c/namespace.c/mount.c/overlay.c/uts.c separation)
- All errors found and fixed during implementation (Errors #1–#16)

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

### ✅ Phase 5: cgroups v2 — Resource Limits (Current)
- [x] New `cgroup.c`/`cgroup.h` module supersedes `uts.c` as exec path
- [x] Three-phase cgroup lifecycle: create → add PID → remove
- [x] `memory.max` limit (`--memory <100M|1G>`); OOM kill at exit code 137
- [x] `cpu.max` limit (`--cpus 0.5` = 50% of one core)
- [x] `pids.max` limit (`--pids 20`) for fork bomb defense
- [x] Auto-enable `enable_cgroup` when any limit flag is set
- [x] `build_container_env()` defensive refactor (decisions.md #21)
- [x] Unit tests (`test_cgroup` requires root)

### 📋 Phase 6: Network Isolation
- [ ] CLONE_NEWNET for network namespace
- [ ] veth pairs and bridges
- [ ] Port forwarding

### 📋 Phase 7: CLI & Lifecycle
- [ ] Container lifecycle management
- [ ] Start/stop/exec commands
- [ ] Refactor shared `*_exec()` boilerplate into common execution core

### 📋 Phase 8: Inspector Integration
- [ ] OCI runtime spec compliance
- [ ] Image management
- [ ] Container inspection tools

---

## Development

### Build Targets

```bash
make              # Build minicontainer
make test         # Build and run all tests (Phase 0 + 1 + 2 + 3 + 4 + 4b + 4c + 5)
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

**Problem:** `sethostname` fails because the child is not in a new UTS namespace.

**Check:**
1. Are you using `--hostname`? This flag auto-enables `CLONE_NEWUTS`
2. Are you running with `sudo`? UTS namespace creation requires `CAP_SYS_ADMIN`

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

**Problem:** `undefined reference to 'mount_exec'`

**Solution:** Update the Makefile to compile and link the new source files. See [decisions.md](docs/decisions.md) for details.

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
