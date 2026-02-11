# minicontainer

> A minimal container runtime built from scratch to understand the internals of Docker and Kubernetes

[![Phase](https://img.shields.io/badge/Phase-0%20Foundation-blue)]()
[![License](https://img.shields.io/badge/License-MIT-green)]()
[![C Standard](https://img.shields.io/badge/C-C11-orange)]()

---

## Overview

**minicontainer** is an educational project that implements a container runtime from first principles. Instead of using Docker or other high-level tools, this project builds process isolation step-by-step using low-level Linux system calls.

**Current Phase:** Phase 0 - Foundation (fork/execve baseline)

This phase implements the fundamental pattern underlying all container runtimes: spawning and managing isolated processes using `fork()` and `execve()`.

---

## Features (Phase 0)

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
# Basic usage
./minicontainer /bin/ls -la

# With debug output
./minicontainer --debug /bin/echo "Hello, containers!"

# Custom environment variables
./minicontainer --env FOO=bar /bin/sh -c 'echo $FOO'

# Get help
./minicontainer --help
```

### Test

```bash
# Run unit tests
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
│   └── spawn.h              # Public API for process spawning
├── src/
│   ├── main.c               # CLI entry point and argument parsing
│   └── spawn.c              # Core fork/execve implementation
├── tests/
│   └── test_spawn.c         # Unit tests
├── docs/
│   ├── decisions.md         # Design decisions and error log
│   └── (phase guides)       # Implementation guides for each phase
├── Makefile                 # Build system
└── README.md                # This file
```

---

## Architecture

### Module Separation

```
┌─────────────────────────────────────┐
│         main.c (CLI Layer)          │
│  • Argument parsing (getopt)        │
│  • Environment variable merging     │
│  • User-facing error messages       │
└───────────────┬─────────────────────┘
                │
                ▼
┌─────────────────────────────────────┐
│       spawn.c (Core Logic)          │
│  • fork() child process             │
│  • execve() new program             │
│  • waitpid() and reaping            │
│  • Signal handling (SIGCHLD)        │
└─────────────────────────────────────┘
```

**Why separate?**
- `spawn.c` is reusable across all phases (will switch to `clone()` in Phase 1)
- `main.c` handles only user interaction
- Easier to unit test in isolation

### API Design

**Configuration structure:**
```c
spawn_config_t config = {
    .program = "/bin/ls",
    .argv = argv,           // NULL-terminated array
    .envp = NULL,           // NULL = inherit parent environment
    .enable_debug = false
};
```

**Result structure:**
```c
spawn_result_t result = spawn_process(&config);

if (result.exited_normally) {
    printf("Exit code: %d\n", result.exit_status);
} else {
    printf("Killed by signal %d\n", result.signal);
}
```

---

## How It Works

### The fork/execve Pattern

```
Parent Process                   Child Process
     │
     ├─────── fork() ───────────────► (Copy of parent)
     │                                      │
     │                                execve("/bin/ls")
     │                                      │
     │                                (Becomes /bin/ls)
     │                                      │
     │                                (Executes)
     │                                      │
     │                                 exit(status)
     │                                      │
     │◄──── waitpid() ──────────────── [Zombie]
     │                                      │
     │ (Reaps zombie)                       X
     │ (Gets exit status)
     ▼
```

### Key System Calls

| System Call | Purpose |
|-------------|---------|
| `fork()` | Creates a new process (duplicate of parent) |
| `execve()` | Replaces process image with new program |
| `waitpid()` | Waits for child to exit and reaps zombie |
| `sigaction()` | Installs SIGCHLD handler to prevent zombies |

---

## Usage Examples

### Example 1: Basic Command Execution

```bash
$ ./minicontainer /bin/echo "Hello from minicontainer"
Hello from minicontainer
```

### Example 2: Debug Mode

```bash
$ ./minicontainer --debug /bin/ls -la
[spawn] Executing: /bin/ls -la
[spawn] Child PID: 12345
total 64
drwxr-xr-x  6 user user  4096 Feb 10 12:00 .
...
[spawn] Child exited with status 0
```

### Example 3: Exit Code Handling

```bash
$ ./minicontainer /bin/false
$ echo $?
1

$ ./minicontainer /nonexistent/command
execve: No such file or directory
$ echo $?
127
```

### Example 4: Environment Variables

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

Tests cover:
- ✓ Basic execution (`/bin/true`)
- ✓ Exit code propagation (exit 42)
- ✓ Signal death (killed by SIGTERM)
- ✓ execve failure (command not found)

### Manual Testing

```bash
# Test normal exit
./minicontainer /bin/true
echo $?  # Should be 0

# Test failure
./minicontainer /bin/false
echo $?  # Should be 1

# Test missing command
./minicontainer /nonexistent
echo $?  # Should be 127

# Test signal death
./minicontainer /bin/sh -c 'kill -9 $$'
echo $?  # Should be 137 (128 + 9)
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
- Why fork() instead of vfork() or clone()?
- Why blocking waitpid()?
- Why signal handlers are async-safe?
- Exit code conventions
- Error handling strategy

---

## Known Limitations (Phase 0)

1. **No PATH search** - Must use absolute paths (`/bin/ls`, not `ls`)
2. **Environment merging** - Custom vars appended, not deduplicated
3. **Fixed env array size** - Max 255 custom environment variables
4. **No FD management** - Child inherits all open file descriptors
5. **Single command** - Runs one command then exits (no daemon mode)

These are intentional for Phase 0. Future phases will address them.

---

## Roadmap

### ✅ Phase 0: Foundation (Current)
- [x] fork/execve pattern
- [x] Signal handling (SIGCHLD)
- [x] Exit status parsing
- [x] Environment variables
- [x] Unit tests

### 🔄 Phase 1: PID Namespace
- [ ] Replace fork() with clone(CLONE_NEWPID)
- [ ] Child sees itself as PID 1
- [ ] Process isolation

### 📋 Phase 2: Filesystem Isolation
- [ ] chroot to custom rootfs
- [ ] Mount /proc and /dev
- [ ] Filesystem isolation

### 📋 Phase 3: Mount Namespace
- [ ] CLONE_NEWNS for mount isolation
- [ ] Private mounts
- [ ] Overlay filesystem support

### 📋 Phase 4: Resource Limits (cgroups)
- [ ] Memory limits
- [ ] CPU shares
- [ ] Process limits
- [ ] cgroup v2 support

### 📋 Phase 5: Network Isolation
- [ ] CLONE_NEWNET for network namespace
- [ ] veth pairs and bridges
- [ ] Port forwarding

### 📋 Phase 6: User Namespace
- [ ] UID/GID mapping
- [ ] Rootless containers

### 📋 Phase 7: Full Container Runtime
- [ ] OCI runtime spec compliance
- [ ] Image management
- [ ] Container lifecycle API

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
man 2 fork        # Process creation
man 2 execve      # Execute program
man 2 waitpid     # Wait for process
man 2 sigaction   # Signal handling
man 7 signal-safety  # Async-safe functions
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

### "Command not found" error

**Problem:** `execve: No such file or directory`

**Solution:** Use absolute path: `/bin/ls` instead of `ls`

**Why?** `execve()` doesn't search PATH. Use `which ls` to find the full path.

---

### Zombie processes

**Problem:** `ps aux | grep defunct` shows zombie processes

**Solution:** Ensure `spawn_init_signals()` is called in main()

**Why?** Without SIGCHLD handler, zombies accumulate if parent doesn't wait.

---

### Compilation errors

**Problem:** `undefined reference to 'WEXITSTATUS'`

**Check:**
1. Ensure `#define _POSIX_C_SOURCE 199309L` is at top of spawn.c
2. Include `<sys/wait.h>`

---

### Segmentation fault

**Problem:** Crash when executing child

**Check:**
1. Is argv NULL-terminated? `char *argv[] = {"/bin/ls", NULL};`
2. Is program path valid?
3. Run with `valgrind` to find exact crash location

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
- Based on "Phase 0 Foundation Implementation Guide"
- Thanks to the Linux kernel developers for excellent documentation

---

## Contact

For questions or issues, please open a GitHub issue or refer to the [implementation guide](phase0_foundation_implementation_guide.md).

---

**Happy containerizing! 🐳**
