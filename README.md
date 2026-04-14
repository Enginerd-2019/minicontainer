# minicontainer

> A minimal container runtime built from scratch to understand the internals of Docker and Kubernetes

[![Phase](https://img.shields.io/badge/Phase-4%20UTS%20Namespace-blue)]()
[![License](https://img.shields.io/badge/License-MIT-green)]()
[![C Standard](https://img.shields.io/badge/C-C11-orange)]()

---

## Overview

**minicontainer** is an educational project that implements a container runtime from first principles. Instead of using Docker or other high-level tools, this project builds process isolation step-by-step using low-level Linux system calls.

**Current Phase:** Phase 4 - UTS Namespace (Hostname Isolation)

Building on Phase 3's OverlayFS and security corrections, this phase adds **UTS namespace isolation** so each container gets its own hostname via `CLONE_NEWUTS` and `sethostname()`. The host's hostname is never affected — each container's identity is fully scoped to its namespace.

---

## Features

### Phase 4 (Current)

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
but `--hostname` does not imply `--overlay` and vice versa — they are
independent isolation features. For full isolation, combine them explicitly.
Note that this auto-enable behavior is a `main.c` design choice; the underlying
library functions (`uts_exec`, `overlay_exec`, etc.) accept each flag
independently, allowing other entry points to compose isolation differently.

**Note:** Namespace features require root or `CAP_SYS_ADMIN`:
```bash
# Option 1: Run with sudo
sudo ./minicontainer --pid --rootfs ./rootfs /bin/sh

# Option 2: Grant capability (avoids sudo for subsequent runs)
sudo setcap cap_sys_admin+ep ./minicontainer
./minicontainer --pid /bin/sh -c 'echo $$'
```

### Test

```bash
# Run all tests (Phase 0 + Phase 1 + Phase 2 + Phase 3 + Phase 4)
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
│   ├── uts.h                # Phase 4: UTS namespace & hostname isolation API
│   ├── overlay.h            # Phase 3: OverlayFS, environment, fd cleanup API
│   ├── mount.h              # Phase 2: Mount namespace & rootfs API
│   ├── namespace.h          # Phase 1: PID namespace isolation API
│   └── spawn.h              # Phase 0: Process spawning API
├── src/
│   ├── main.c               # CLI entry point and argument parsing
│   ├── uts.c                # Phase 4: UTS namespace, clone/exec (calls overlay.c, mount.c)
│   ├── overlay.c            # Phase 3: OverlayFS, close_inherited_fds
│   ├── mount.c              # Phase 2: setup_rootfs, mount_proc
│   ├── namespace.c          # Phase 1: clone() + PID namespace logic
│   └── spawn.c              # Phase 0: fork/execve implementation
├── tests/
│   ├── test_uts.c           # Phase 4: UTS namespace tests (requires root + rootfs)
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
│                main.c (CLI Layer)                     │
│  • Parses --pid, --rootfs, --overlay, --hostname, etc │
│  • build_container_env() — clean environment          │
│  • Misplaced flag detection                           │
│  • Calls uts_exec()                                   │
└─────────────────────┬────────────────────────────────┘
                      │
                      ▼
┌──────────────────────────────────────────────────────┐
│           uts.c (Orchestration Layer)                 │
│  • setup_overlay() → mount OverlayFS (from overlay.c)│
│  • clone() with NEWPID | NEWNS | NEWUTS              │
│  • waitpid() and exit status parsing                 │
│  • teardown_overlay() → unmount and cleanup          │
└─────────────────────┬────────────────────────────────┘
                      │
                      ▼
┌──────────────────────────────────────────────────────┐
│            Child Process (child_func)                 │
│  • setup_uts(): sethostname() in new UTS namespace    │
│  • setup_rootfs(): pivot_root to new rootfs           │
│  • mount_proc(): mount /proc for PID namespace        │
│  • close_inherited_fds(): close leaked parent fds     │
│  • execve() with clean container environment          │
└──────────────────────────────────────────────────────┘
```

**Phase transitions:**
- Phase 0: `spawn.c` (`fork/execve`)
- Phase 1: `namespace.c` (`clone(CLONE_NEWPID)`)
- Phase 2: `mount.c` (`clone(CLONE_NEWPID | CLONE_NEWNS)` + `pivot_root`)
- Phase 3: `overlay.c` (OverlayFS + env isolation + fd cleanup), calls into `mount.c` for `setup_rootfs`/`mount_proc`
- Phase 4: `uts.c` (`CLONE_NEWUTS` + `sethostname`), calls into `overlay.c` and `mount.c`
- Earlier modules retained for their respective phase tests

**Note on code duplication:** Each phase's `*_exec()` function (e.g., `overlay_exec`, `uts_exec`) shares ~90% of its structure with the previous phase — `clone()`, `waitpid()`, stack allocation, and exit status parsing are repeated each time. This is a deliberate pedagogical choice: each module is self-contained and readable without cross-referencing earlier phases. A production runtime would factor this into a shared execution core, and this refactoring is planned for Phase 7.

### API Design

**Configuration structure:**
```c
uts_config_t config = {
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
    .hostname = "mycontainer"         // NULL = no hostname change
};
```

**Result structure:**
```c
uts_result_t result = uts_exec(&config);

if (result.exited_normally) {
    printf("Exit code: %d\n", result.exit_status);
} else {
    printf("Killed by signal %d\n", result.signal);
}

uts_cleanup(&result);  // Free clone stack
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
    │        containers/<id>/merged/ (unified view)    │
    │  Container sees: rootfs + writes merged together │
    │  Reads: lowerdir first, then upperdir            │
    │  Writes: always go to upperdir                   │
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
| `sethostname()` | Sets container hostname inside UTS namespace | 4 |
| `mount("overlay")` | Mounts OverlayFS with `MS_NODEV \| MS_NOSUID` | 3 |
| `close()` | Closes inherited parent fds before `execve()` | 3 |
| `getrlimit()` | Gets fd limit for brute-force fallback | 3 |
| `pivot_root()` | Swaps root filesystem for the container | 2 |
| `mount()` | Bind mounts rootfs, sets propagation, mounts `/proc` | 2 |
| `umount2()` | Lazily unmounts old root and overlay (`MNT_DETACH`) | 2, 3 |
| `clone()` | Creates child with namespace flags (`NEWPID`, `NEWNS`, `NEWUTS`) | 1–4 |
| `fork()` | Creates a new process (duplicate of parent) | 0 |
| `execve()` | Replaces process image with new program | 0–4 |
| `waitpid()` | Waits for child to exit and reaps zombie | 0–4 |
| `sigaction()` | Installs SIGCHLD handler to prevent zombies | 0 |

---

## Usage Examples

### Example 1: Hostname Isolation (Phase 4)

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

### Example 2: OverlayFS — Base Image Untouched (Phase 3)

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

### Example 3: Clean Container Environment (Phase 3)

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

### Example 4: Debug Mode with Overlay (Phase 3)

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

### Example 5: PID Namespace Isolation (Phase 1)

```bash
# Without --pid: shell reports its real PID
$ ./minicontainer /bin/sh -c 'echo $$'
5678

# With --pid: shell sees itself as PID 1
$ sudo ./minicontainer --pid /bin/sh -c 'echo $$'
1
```

### Example 6: Basic Command Execution

```bash
$ ./minicontainer /bin/echo "Hello from minicontainer"
Hello from minicontainer
```

### Example 7: Exit Code Handling

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

### Manual Testing

```bash
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
7. **Requires root for namespaces** — `CLONE_NEWPID`/`CLONE_NEWNS`/`CLONE_NEWUTS`/overlay need `CAP_SYS_ADMIN`
8. **Rootfs must be pre-built** — No image pull or layer support; rootfs directory must exist before running
9. **No tmpfs mounts** — Only `/proc` is mounted automatically; `/tmp`, `/dev`, etc. are not set up
10. ~~**No hostname isolation**~~ — **Fixed in Phase 4** via `CLONE_NEWUTS` and `--hostname`

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

### ✅ Phase 4: UTS Namespace — Hostname Isolation (Current)
- [x] `CLONE_NEWUTS` for hostname isolation
- [x] `--hostname <name>` flag (auto-enables UTS namespace)
- [x] `sethostname()` in child — host hostname unchanged
- [x] Unit tests (requires root)

### 📋 Phase 4b: User Namespace
- [ ] `CLONE_NEWUSER` for unprivileged containers
- [ ] UID/GID mapping

### 📋 Phase 5: Network Isolation
- [ ] CLONE_NEWNET for network namespace
- [ ] veth pairs and bridges
- [ ] Port forwarding

### 📋 Phase 6: CLI & Lifecycle
- [ ] Container lifecycle management
- [ ] Start/stop/exec commands
- [ ] Refactor shared `*_exec()` boilerplate into common execution core

### 📋 Phase 7: Inspector Integration
- [ ] OCI runtime spec compliance
- [ ] Image management
- [ ] Container inspection tools

---

## Development

### Build Targets

```bash
make              # Build minicontainer
make test         # Build and run all tests (Phase 0 + 1 + 2 + 3 + 4)
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
