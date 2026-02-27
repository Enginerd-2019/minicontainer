# minicontainer

> A minimal container runtime built from scratch to understand the internals of Docker and Kubernetes

[![Phase](https://img.shields.io/badge/Phase-2%20Mount%20Namespace-blue)]()
[![License](https://img.shields.io/badge/License-MIT-green)]()
[![C Standard](https://img.shields.io/badge/C-C11-orange)]()

---

## Overview

**minicontainer** is an educational project that implements a container runtime from first principles. Instead of using Docker or other high-level tools, this project builds process isolation step-by-step using low-level Linux system calls.

**Current Phase:** Phase 2 - Mount Namespace & Filesystem Isolation

Building on Phase 1's PID namespace isolation, this phase introduces **filesystem isolation** using Linux mount namespaces. The containerized process runs in its own root filesystem via `pivot_root()`, with a private `/proc` mount that reflects only the container's process tree.

---

## Features

### Phase 2 (Current)

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

# PID namespace isolation (requires root or CAP_SYS_ADMIN)
sudo ./minicontainer --pid /bin/sh -c 'echo $$'    # Prints: 1

# Rootfs isolation (auto-enables mount namespace, requires root)
sudo ./minicontainer --rootfs ./rootfs /bin/ls /

# Full isolation: PID + mount namespace with custom rootfs
sudo ./minicontainer --pid --rootfs ./rootfs /bin/sh -c 'echo PID: $$ && ls /'

# Debug mode with full isolation
sudo ./minicontainer --pid --rootfs ./rootfs --debug /bin/sh -c 'echo $$'

# Get help
./minicontainer --help
```

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
# Run all tests (Phase 0 + Phase 1 + Phase 2)
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
│   ├── mount.h              # Phase 2: Mount namespace & rootfs API
│   ├── namespace.h          # Phase 1: PID namespace isolation API
│   └── spawn.h              # Phase 0: Process spawning API
├── src/
│   ├── main.c               # CLI entry point and argument parsing
│   ├── mount.c              # Phase 2: Mount namespace, pivot_root, /proc
│   ├── namespace.c          # Phase 1: clone() + PID namespace logic
│   └── spawn.c              # Phase 0: fork/execve implementation
├── tests/
│   ├── test_mount.c         # Phase 2: Mount/rootfs tests (requires root + rootfs)
│   ├── test_namespace.c     # Phase 1: Namespace tests (requires root)
│   └── test_spawn.c         # Phase 0: Spawn tests
├── docs/
│   └── decisions.md         # Design decisions and error log
├── Makefile                 # Build system
└── README.md                # This file
```

---

## Architecture

### Module Separation

```
┌─────────────────────────────────────────────────┐
│              main.c (CLI Layer)                  │
│  • Parses --pid, --rootfs, --debug flags         │
│  • Auto-enables mount namespace when --rootfs    │
│  • Calls mount_exec()                            │
└─────────────────────┬───────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────┐
│          mount.c (Isolation Layer)               │
│  • clone() with CLONE_NEWPID | CLONE_NEWNS       │
│  • Stack allocation/deallocation                 │
│  • waitpid() and exit status parsing             │
└─────────────────────┬───────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────┐
│           Child Process (child_func)             │
│  • setup_rootfs(): pivot_root to new rootfs      │
│  • mount_proc(): mount /proc for PID namespace   │
│  • execve() to replace with target program       │
└─────────────────────────────────────────────────┘
```

**Phase 1 → Phase 2 transition:**
- `namespace.c` (`clone(CLONE_NEWPID)`) superseded by `mount.c` (`clone(CLONE_NEWPID | CLONE_NEWNS)`)
- `mount_exec()` handles PID namespace, mount namespace, and rootfs pivot
- `namespace.c`/`namespace.h` retained for Phase 1 tests
- `spawn.c`/`spawn.h` retained for Phase 0 tests

### API Design

**Configuration structure:**
```c
mount_config_t config = {
    .program = "/bin/sh",
    .argv = argv,                     // NULL-terminated array
    .envp = NULL,                     // NULL = inherit parent environment
    .enable_debug = false,
    .enable_pid_namespace = true,     // Use CLONE_NEWPID
    .enable_mount_namespace = true,   // Use CLONE_NEWNS
    .rootfs_path = "./rootfs"         // NULL = no rootfs change
};
```

**Result structure:**
```c
mount_result_t result = mount_exec(&config);

if (result.exited_normally) {
    printf("Exit code: %d\n", result.exit_status);
} else {
    printf("Killed by signal %d\n", result.signal);
}

mount_cleanup(&result);  // Free clone stack
```

---

## How It Works

### Mount Namespace & Filesystem Isolation (Phase 2)

```
Host Filesystem                      Container Filesystem
/                                    / (was ./rootfs on host)
├── bin/                             ├── bin/
├── etc/                             ├── etc/
├── home/                            ├── proc/   ← private mount (PID ns only)
├── proc/  ← host's full process     └── ...     ← only rootfs contents visible
├── tmp/
└── ...

Host mount table is NOT visible inside the container.
Old root is lazily unmounted via umount2(MNT_DETACH).
```

The `pivot_root()` syscall swaps the container's root to the provided rootfs directory. Combined with `CLONE_NEWNS`, mount events inside the container do not propagate to the host.

### The clone/pivot_root/execve Pattern (Phase 2)

```
Parent Process                   Child Process (new PID + mount namespace)
     │
     ├── clone(NEWPID|NEWNS) ───────► child_func() [PID 1 inside ns]
     │                                      │
     │                                setup_rootfs()
     │                                  ├── mount(MS_PRIVATE /)
     │                                  ├── bind mount rootfs
     │                                  ├── pivot_root(".", "old_root")
     │                                  ├── chdir("/")
     │                                  └── umount2("/old_root", MNT_DETACH)
     │                                      │
     │                                mount_proc()
     │                                  └── mount("proc", "/proc", "proc")
     │                                      │
     │                                execve("/bin/sh")
     │                                      │
     │                                 exit(status)
     │                                      │
     │◄──── waitpid() ──────────────── [Reaped]
     │
     │ mount_cleanup()
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
| `pivot_root()` | Swaps root filesystem for the container | 2 |
| `mount()` | Bind mounts rootfs, sets propagation, mounts `/proc` | 2 |
| `umount2()` | Lazily unmounts old root (`MNT_DETACH`) | 2 |
| `clone()` | Creates child with namespace flags (`CLONE_NEWPID`, `CLONE_NEWNS`) | 1, 2 |
| `fork()` | Creates a new process (duplicate of parent) | 0 |
| `execve()` | Replaces process image with new program | 0, 1, 2 |
| `waitpid()` | Waits for child to exit and reaps zombie | 0, 1, 2 |
| `sigaction()` | Installs SIGCHLD handler to prevent zombies | 0 |

---

## Usage Examples

### Example 1: Rootfs Isolation (Phase 2)

```bash
# Container sees only the rootfs contents
$ sudo ./minicontainer --rootfs ./rootfs /bin/ls /
bin  etc  proc  usr

# Host filesystem is not visible inside the container
$ sudo ./minicontainer --rootfs ./rootfs /bin/sh -c 'ls /home'
ls: /home: No such file or directory
```

### Example 2: Full Isolation with Debug (Phase 2)

```bash
$ sudo ./minicontainer --pid --rootfs ./rootfs --debug /bin/sh -c 'echo $$'
[parent] Executing: /bin/sh -c echo $$
[parent] Rootfs: ./rootfs
[parent] Creating mount namespace
[parent] Child PID: 5678
[child] PID: 1
[child] Setting up rootfs: ./rootfs
[child] Bind mounted /home/user/minicontainer/rootfs
[child] pivot_root successful
[child] Unmounted old root
[child] Mounted /proc
1
[parent] Child exited: 0
```

### Example 3: /proc Inside Container (Phase 2)

```bash
# ps only sees container processes, not host processes
$ sudo ./minicontainer --pid --rootfs ./rootfs /bin/sh -c 'cat /proc/1/status | head -1'
Name:   sh
```

### Example 4: PID Namespace Isolation (Phase 1)

```bash
# Without --pid: shell reports its real PID
$ ./minicontainer /bin/sh -c 'echo $$'
5678

# With --pid: shell sees itself as PID 1
$ sudo ./minicontainer --pid /bin/sh -c 'echo $$'
1
```

### Example 5: Basic Command Execution

```bash
$ ./minicontainer /bin/echo "Hello from minicontainer"
Hello from minicontainer
```

### Example 6: Exit Code Handling

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

### Manual Testing

```bash
# Rootfs isolation: should show only rootfs contents
sudo ./minicontainer --rootfs ./rootfs /bin/ls /

# Full isolation: PID 1 + rootfs
sudo ./minicontainer --pid --rootfs ./rootfs /bin/sh -c 'echo $$ && ls /'

# PID namespace: should print 1
sudo ./minicontainer --pid /bin/sh -c 'echo $$'

# Without namespace: should print real PID
./minicontainer /bin/sh -c 'echo $$'

# Exit codes work through namespace
sudo ./minicontainer --pid /bin/sh -c 'exit 42'
echo $?  # Should be 42

# Missing command
./minicontainer /nonexistent
echo $?  # Should be 127
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
- Phase 1 → Phase 2 architecture transition (namespace → mount API)
- Why pivot_root() instead of chroot()
- Mount propagation (MS_PRIVATE | MS_REC) before pivot_root
- Bind mount + pivot_root pattern for arbitrary rootfs paths
- Lazy unmount (MNT_DETACH) for old root cleanup
- Why clone() instead of fork() for namespace support
- Stack allocation choices (malloc vs mmap)
- Modular design (spawn.c/namespace.c/mount.c vs main.c separation)
- Configuration and result structure patterns
- Errors found and fixed during implementation

---

## Known Limitations

1. **No PATH search** - Must use absolute paths (`/bin/ls`, not `ls`)
2. **No FD management** - Child inherits all open file descriptors
3. **Single command** - Runs one command then exits (no daemon mode)
4. **Requires root for namespaces** - `CLONE_NEWPID`/`CLONE_NEWNS` need `CAP_SYS_ADMIN` (user namespaces in Phase 3 will allow unprivileged use)
5. **Rootfs must be pre-built** - No image pull or layer support; rootfs directory must exist before running
6. **No tmpfs mounts** - Only `/proc` is mounted automatically; `/tmp`, `/dev`, etc. are not set up

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

### ✅ Phase 2: Mount Namespace & Filesystem Isolation (Current)
- [x] CLONE_NEWNS for mount isolation
- [x] pivot_root() to custom rootfs
- [x] Mount /proc inside container
- [x] --rootfs flag
- [x] Unit tests (requires root + rootfs)

### 📋 Phase 3: UTS & User Namespace
- [ ] CLONE_NEWUTS for hostname isolation
- [ ] CLONE_NEWUSER for UID/GID mapping
- [ ] Rootless containers

### 📋 Phase 4: Resource Limits (cgroups)
- [ ] Memory limits
- [ ] CPU shares
- [ ] Process limits
- [ ] cgroup v2 support

### 📋 Phase 5: Network Isolation
- [ ] CLONE_NEWNET for network namespace
- [ ] veth pairs and bridges
- [ ] Port forwarding

### 📋 Phase 6: CLI & Lifecycle
- [ ] Container lifecycle management
- [ ] Start/stop/exec commands

### 📋 Phase 7: Inspector Integration
- [ ] OCI runtime spec compliance
- [ ] Image management
- [ ] Container inspection tools

---

## Development

### Build Targets

```bash
make              # Build minicontainer
make test         # Build and run all tests (Phase 0 + 1 + 2)
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
man 2 pivot_root      # Change the root filesystem (Phase 2)
man 2 mount           # Mount filesystem (Phase 2)
man 2 umount2         # Unmount filesystem with flags (Phase 2)
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
