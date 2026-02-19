# minicontainer

> A minimal container runtime built from scratch to understand the internals of Docker and Kubernetes

[![Phase](https://img.shields.io/badge/Phase-1%20PID%20Namespace-blue)]()
[![License](https://img.shields.io/badge/License-MIT-green)]()
[![C Standard](https://img.shields.io/badge/C-C11-orange)]()

---

## Overview

**minicontainer** is an educational project that implements a container runtime from first principles. Instead of using Docker or other high-level tools, this project builds process isolation step-by-step using low-level Linux system calls.

**Current Phase:** Phase 1 - PID Namespace Isolation

Building on the Phase 0 foundation (`fork`/`execve`), this phase introduces **process isolation** using Linux PID namespaces. The containerized process sees itself as PID 1 in its own process tree, unable to see or interact with host processes.

---

## Features

### Phase 1 (Current)

✅ **PID Namespace Isolation** - Child process runs as PID 1 in its own namespace via `clone(CLONE_NEWPID)`
✅ **Manual Stack Allocation** - Allocates and manages stack for `clone()` child
✅ **Namespace Cleanup** - Proper deallocation of clone stack via `namespace_cleanup()`
✅ **Conditional Isolation** - `--pid` flag enables namespace; without it, runs like Phase 0

### Phase 0 (Foundation)

✅ **Process Spawning** - Fork child processes and execute arbitrary commands
✅ **Exit Status Handling** - Properly report exit codes and signal deaths
✅ **Zombie Prevention** - SIGCHLD handler prevents zombie processes
✅ **Environment Variables** - Pass custom environment to child processes
✅ **Error Handling** - Robust error checking on all system calls
✅ **Debug Mode** - Optional verbose output for troubleshooting

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

# PID namespace with debug output
sudo ./minicontainer --pid --debug /bin/echo "Hello, containers!"

# Custom environment variables
./minicontainer --env FOO=bar /bin/sh -c 'echo $FOO'

# Get help
./minicontainer --help
```

**Note:** PID namespace features require root or `CAP_SYS_ADMIN`:
```bash
# Option 1: Run with sudo
sudo ./minicontainer --pid /bin/sh

# Option 2: Grant capability (avoids sudo for subsequent runs)
sudo setcap cap_sys_admin+ep ./minicontainer
./minicontainer --pid /bin/sh -c 'echo $$'
```

### Test

```bash
# Run all tests (Phase 0 + Phase 1)
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
│   ├── namespace.h          # Phase 1: Namespace isolation API
│   └── spawn.h              # Phase 0: Process spawning API
├── src/
│   ├── main.c               # CLI entry point and argument parsing
│   ├── namespace.c          # Phase 1: clone() + PID namespace logic
│   └── spawn.c              # Phase 0: fork/execve implementation
├── tests/
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
│  • Parses --pid, --debug, --env flags            │
│  • Environment variable merging                  │
│  • Calls namespace_exec()                        │
└─────────────────────┬───────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────┐
│         namespace.c (Isolation Layer)            │
│  • clone() with CLONE_NEWPID                     │
│  • Stack allocation/deallocation                 │
│  • Child function wrapper                        │
│  • waitpid() and exit status parsing             │
└─────────────────────┬───────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────┐
│           Child Process (child_func)             │
│  • Runs in new PID namespace (PID 1)             │
│  • execve() to replace with target program       │
└─────────────────────────────────────────────────┘
```

**Phase 0 → Phase 1 transition:**
- `spawn.c` (`fork()`) superseded by `namespace.c` (`clone()`)
- `namespace_exec()` handles both namespaced and non-namespaced execution
- `spawn.c`/`spawn.h` retained for Phase 0 tests

### API Design

**Configuration structure:**
```c
namespace_config_t config = {
    .program = "/bin/sh",
    .argv = argv,                   // NULL-terminated array
    .envp = NULL,                   // NULL = inherit parent environment
    .enable_debug = false,
    .enable_pid_namespace = true    // Use CLONE_NEWPID
};
```

**Result structure:**
```c
namespace_result_t result = namespace_exec(&config);

if (result.exited_normally) {
    printf("Exit code: %d\n", result.exit_status);
} else {
    printf("Killed by signal %d\n", result.signal);
}

namespace_cleanup(&result);  // Free clone stack
```

---

## How It Works

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

### The clone/execve Pattern (Phase 1)

```
Parent Process                   Child Process (new PID namespace)
     │
     ├── clone(CLONE_NEWPID) ───────► child_func() [PID 1 inside ns]
     │                                      │
     │                                execve("/bin/sh")
     │                                      │
     │                                (Becomes /bin/sh)
     │                                      │
     │                                 exit(status)
     │                                      │
     │◄──── waitpid() ──────────────── [Reaped]
     │
     │ namespace_cleanup()
     │ (Frees clone stack)
     ▼
```

### Key System Calls

| System Call | Purpose | Phase |
|-------------|---------|-------|
| `clone()` | Creates child with namespace flags (`CLONE_NEWPID`) | 1 |
| `fork()` | Creates a new process (duplicate of parent) | 0 |
| `execve()` | Replaces process image with new program | 0, 1 |
| `waitpid()` | Waits for child to exit and reaps zombie | 0, 1 |
| `sigaction()` | Installs SIGCHLD handler to prevent zombies | 0 |

---

## Usage Examples

### Example 1: PID Namespace Isolation

```bash
# Without --pid: shell reports its real PID
$ ./minicontainer /bin/sh -c 'echo $$'
5678

# With --pid: shell sees itself as PID 1
$ sudo ./minicontainer --pid /bin/sh -c 'echo $$'
1
```

### Example 2: Debug Mode with PID Namespace

```bash
$ sudo ./minicontainer --pid --debug /bin/sh -c 'echo $$'
[parent] Executing: /bin/sh -c echo $$
[parent] Creating PID namespace
[parent] Child PID in parent namespace: 5678
[child] PID inside namespace: 1
[child] PPID inside namespace: 0
1
[parent] Child exited with status 0
```

### Example 3: Basic Command Execution

```bash
$ ./minicontainer /bin/echo "Hello from minicontainer"
Hello from minicontainer
```

### Example 4: Exit Code Handling

```bash
$ ./minicontainer /bin/false
$ echo $?
1

$ ./minicontainer /nonexistent/command
execve: No such file or directory
$ echo $?
127
```

### Example 5: Environment Variables

```bash
$ ./minicontainer --env CUSTOM_VAR=hello /bin/sh -c 'echo $CUSTOM_VAR'
hello
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

### Manual Testing

```bash
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
- Phase 0 → Phase 1 architecture transition (spawn → namespace API)
- Why clone() instead of fork() for namespace support
- Stack allocation choices (malloc vs mmap)
- Modular design (spawn.c/namespace.c vs main.c separation)
- Configuration and result structure patterns
- Why blocking waitpid()?
- Exit code conventions
- Errors found and fixed during implementation

---

## Known Limitations

1. **No PATH search** - Must use absolute paths (`/bin/ls`, not `ls`)
2. **Environment merging** - Custom vars appended, not deduplicated
3. **Fixed env array size** - Max 255 custom environment variables
4. **No FD management** - Child inherits all open file descriptors
5. **Single command** - Runs one command then exits (no daemon mode)
6. **Requires root for namespaces** - `CLONE_NEWPID` needs `CAP_SYS_ADMIN` (user namespaces in Phase 3 will allow unprivileged use)
7. **No /proc remount** - `ps` inside the namespace still sees host processes (fixed in Phase 2 with mount namespace)

---

## Roadmap

### ✅ Phase 0: Foundation
- [x] fork/execve pattern
- [x] Signal handling (SIGCHLD)
- [x] Exit status parsing
- [x] Environment variables
- [x] Unit tests

### ✅ Phase 1: PID Namespace (Current)
- [x] Replace fork() with clone(CLONE_NEWPID)
- [x] Child sees itself as PID 1
- [x] Manual stack allocation for clone()
- [x] Namespace cleanup (stack deallocation)
- [x] Unit tests (requires root)

### 📋 Phase 2: Mount Namespace & Filesystem Isolation
- [ ] CLONE_NEWNS for mount isolation
- [ ] pivot_root() to custom rootfs
- [ ] Mount /proc inside container
- [ ] --rootfs flag

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
make test         # Build and run tests
make clean        # Remove build artifacts
make debug        # Build with debug symbols (-g)
make valgrind     # Run memory leak detection
make examples     # Run demonstration commands
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
man 2 clone           # Process creation with namespaces
man 2 fork            # Process creation (Phase 0)
man 2 execve          # Execute program
man 2 waitpid         # Wait for process
man 7 namespaces      # Overview of Linux namespaces
man 7 pid_namespaces  # PID namespace details
man 7 signal-safety   # Async-safe functions
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

### "Operation not permitted" with --pid

**Problem:** `clone: Operation not permitted`

**Solution:** PID namespaces require `CAP_SYS_ADMIN`:
```bash
sudo ./minicontainer --pid /bin/sh
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

**Solution:** Use absolute path: `/bin/ls` instead of `ls`

**Why?** `execve()` doesn't search PATH. Use `which ls` to find the full path.

---

### Segmentation fault in child

**Problem:** Crash when clone'd child starts executing.

**Check:**
1. Is the stack pointer correct? Must pass `stack + STACK_SIZE` (top), not `stack` (base)
2. Is argv NULL-terminated? `char *argv[] = {"/bin/ls", NULL};`
3. Run with `valgrind` to find exact crash location

---

### Linker errors after phase transition

**Problem:** `undefined reference to 'namespace_exec'`

**Solution:** Update the Makefile to compile and link the new source files. See [Error #4 in decisions.md](docs/decisions.md) for details.

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
