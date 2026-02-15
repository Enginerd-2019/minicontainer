# Design Decisions and Error Log

**Project:** minicontainer - Minimal Container Runtime
**Phase:** 1 - PID Namespace Isolation
**Last Updated:** 2026-02-15

---

## Table of Contents

1. [Architecture Decisions](#architecture-decisions)
2. [Implementation Choices](#implementation-choices)
3. [Errors Found and Fixed](#errors-found-and-fixed)
4. [Known Limitations](#known-limitations)
5. [Future Considerations](#future-considerations)

---

## Architecture Decisions

### 1. Modular Design: spawn.c vs main.c

**Decision:** Separate process spawning logic from CLI argument parsing.

**Rationale:**
- **Reusability:** `spawn.c` can be used as a library in future phases when we transition to `clone()` with namespaces
- **Testability:** Core spawning logic can be unit tested independently of CLI
- **Clean separation of concerns:** main.c handles user interaction, spawn.c handles Unix process management

**Trade-offs:**
- Slightly more complex than monolithic approach
- Requires careful API design (spawn_config_t structure)

---

### 2. Configuration Structure Pattern

**Decision:** Use `spawn_config_t` structure instead of multiple function parameters.

**Rationale:**
- **Forward compatibility:** Easy to add new fields (rootfs path, cgroup settings, etc.) in later phases without breaking API
- **Readability:** Named fields are clearer than positional parameters
- **Flexibility:** Caller can use designated initializers to set only needed fields

**Example:**
```c
spawn_config_t config = {
    .program = "/bin/ls",
    .argv = argv,
    .envp = NULL,        // Can omit to inherit
    .enable_debug = true
};
```

---

### 3. Result Structure Instead of Error Codes

**Decision:** Return `spawn_result_t` structure instead of simple integer exit code.

**Rationale:**
- **Rich information:** Contains PID, exit status, signal info, and success flag
- **Type safety:** Structured data prevents misinterpretation of exit codes
- **Extensibility:** Can add timing, resource usage stats in future

**Alternative considered:**
- Returning exit code and using output parameters for PID/signal (messier API)

---

### 4. Signal Handler Installation

**Decision:** Require explicit `spawn_init_signals()` call at program startup.

**Rationale:**
- **Explicit control:** User knows when signal handlers are installed
- **Testability:** Tests can choose whether to install handlers
- **Safety:** Prevents automatic global state modification

**Pattern:**
```c
int main() {
    spawn_init_signals();  // Install SIGCHLD handler
    // ... rest of program
}
```

---

### 5. Environment Variable Handling

**Decision:** Use `__environ` global when `envp` is NULL.

**Rationale:**
- **Convenience:** Default to inheriting parent environment (most common case)
- **POSIX compliance:** `__environ` is available on all POSIX systems
- **Flexibility:** User can still pass custom environment if needed

**Note:** `__environ` is declared in `<unistd.h>` on Linux.

---

### 6. Exit Code Conventions

**Decision:** Follow shell conventions for exit codes.

**Exit Code Mapping:**
- `0` = Success
- `1-125` = Program-specific errors
- `126` = Command found but not executable (not yet implemented)
- `127` = Command not found (execve failure)
- `128 + N` = Killed by signal N (e.g., 137 = SIGKILL)

**Rationale:**
- **Compatibility:** Matches bash/sh behavior
- **Familiarity:** Standard Unix convention
- **Debugging:** Easy to identify failure modes

---

## Implementation Choices

### 1. Why fork() Not vfork()?

**Choice:** Use `fork()` despite `vfork()` being faster.

**Rationale:**
- **Safety:** `vfork()` has strict requirements (child must only call `execve()` or `_exit()`)
- **Simplicity:** `fork()` is more forgiving and well-understood
- **Performance:** Modern kernels use copy-on-write (COW) for fork(), making it fast
- **Future-proofing:** Phase 1 will replace fork() with `clone()` anyway

---

### 2. Blocking vs Non-blocking Wait

**Choice:** Use blocking `waitpid(pid, &status, 0)` in spawn_process().

**Rationale:**
- **Simplicity:** Synchronous behavior is easier to reason about
- **Correctness:** Guarantees zombie reaping before function returns
- **Use case:** minicontainer is designed for running single commands, not managing multiple processes

**Future consideration:** Phase 2+ may need non-blocking wait for container lifecycle management.

---

### 3. Signal Handler: Async-Safe Functions Only

**Choice:** Only use `waitpid()` in SIGCHLD handler, no `printf()` or `malloc()`.

**Rationale:**
- **Safety:** Signal handlers can interrupt any code, including non-reentrant functions
- **Compliance:** POSIX async-signal-safety requirements (see `man 7 signal-safety`)
- **Reliability:** Prevents deadlocks and corruption

**Async-safe functions used:**
- `waitpid()` ✓
- Saving/restoring `errno` ✓

**Forbidden functions:**
- `printf()`, `malloc()`, `free()` ✗

---

### 4. Error Handling Strategy

**Choice:** Print errors with `perror()` and return sentinel values.

**Rationale:**
- **Diagnostics:** `perror()` automatically appends `strerror(errno)` message
- **Simplicity:** No need for custom error code enum
- **Unix idiom:** Standard practice in system programming

**Sentinel values:**
- `child_pid = -1` indicates fork/wait failure
- Exit status `127` indicates execve failure
- Exit status `128 + N` indicates signal death

---

## Errors Found and Fixed

### Error #1: Typo in WEXITSTATUS Macro

**Location:** `src/spawn.c:116`

**Original Code:**
```c
result.exit_status = WEXITESTATUS(status);  // TYPO: extra 'E'
```

**Fixed Code:**
```c
result.exit_status = WEXITSTATUS(status);
```

**Impact:**
- **Severity:** Critical - compilation failure
- **Symptom:** Compiler error: `implicit declaration of function 'WEXITESTATUS'`
- **Root Cause:** Typo when transcribing from implementation guide

**Fix Applied:** 2026-02-10

---

### Error #2: Wrong Debug Message in WIFEXITED Block

**Location:** `src/spawn.c:119`

**Original Code:**
```c
if (WIFEXITED(status)) {
    result.exited_normally = true;
    result.exit_status = WEXITSTATUS(status);

    if (config->enable_debug) {
        printf("[spawn] Child killed by signal %d\n", result.signal);  // WRONG!
    }
}
```

**Fixed Code:**
```c
if (WIFEXITED(status)) {
    result.exited_normally = true;
    result.exit_status = WEXITSTATUS(status);

    if (config->enable_debug) {
        printf("[spawn] Child exited with status %d\n", result.exit_status);
    }
}
```

**Impact:**
- **Severity:** Medium - misleading debug output
- **Symptom:** Debug message says "killed by signal" when child exited normally
- **Root Cause:** Copy-paste error from WIFSIGNALED block

**Fix Applied:** 2026-02-10

---

### Error #3: getopt_long() Parsing Child Command Arguments

**Location:** `src/main.c:38`

**Original Code:**
```c
while ((opt = getopt_long(argc, argv, "de:h", long_options, NULL)) != -1) {
```

**Problem:**
When running `./minicontainer /bin/ls -la`, getopt_long() continued parsing beyond the command path and tried to interpret `-la` as minicontainer options instead of passing them to the child command.

**Symptom:**
```bash
$ ./minicontainer /bin/ls -la
./minicontainer: invalid option -- 'l'
Usage: ./minicontainer [OPTIONS] <command> [args...]
```

**Fixed Code:**
```c
while ((opt = getopt_long(argc, argv, "+de:h", long_options, NULL)) != -1) {
```

**Explanation:**
The `+` prefix in the optstring enables POSIX mode, which stops parsing at the first non-option argument. This prevents getopt from permuting argv and ensures child command arguments are preserved.

**Behavior:**
- Without `+`: getopt permutes argv to move all options to the front, parsing `-la` as options
- With `+`: getopt stops at `/bin/ls`, leaving `-la` in argv for the child

**Impact:**
- **Severity:** Critical - completely prevented passing flags to child commands
- **Symptom:** Any child command with flags would fail with "invalid option" error
- **Root Cause:** Default getopt behavior permutes argv array
- **Alternative solution:** Use `--` separator: `./minicontainer --debug -- /bin/ls -la`

**Examples After Fix:**
```bash
./minicontainer /bin/ls -la          # Works: -la goes to ls
./minicontainer --debug /bin/echo -n # Works: -n goes to echo
./minicontainer /bin/date +%Y        # Works: +%Y goes to date
```

**Fix Applied:** 2026-02-10

---

### Error #4: Linker Failure — Undefined References to namespace_exec/namespace_cleanup

**Location:** `Makefile` (link step)

**Symptom:**
```
/usr/bin/ld: build/main.o: in function `main':
main.c:(.text+0x514): undefined reference to `namespace_exec'
main.c:(.text+0x523): undefined reference to `namespace_cleanup'
collect2: error: ld returned 1 exit status
```

**Root Cause:**
When transitioning `main.c` from Phase 0 (spawn API) to Phase 1 (namespace API), the Makefile was not updated to match. It still compiled and linked only `spawn.c`, but `main.c` now calls `namespace_exec()` and `namespace_cleanup()` from `namespace.c`.

**Changes required:**
1. Added `namespace.c` / `namespace.o` to source and object file variables
2. Changed `$(MINICONTAINER)` link target from `$(MAIN_OBJ) $(SPAWN_OBJ)` to `$(MAIN_OBJ) $(NAMESPACE_OBJ)`
3. Updated `main.o` dependency from `spawn.h` to `namespace.h`
4. Added `-D_GNU_SOURCE` to CFLAGS (required for `clone()` and `CLONE_NEWPID` in `<sched.h>`)

**Impact:**
- **Severity:** Critical - build failure, no binary produced
- **Phase transition note:** When moving between phases, the Makefile must be updated alongside source changes. The guide's compilation instructions (Phase 1, Section 10.1) show the correct gcc invocation but don't provide an updated Makefile.

**Fix Applied:** 2026-02-15

---

## Known Limitations

### 1. No PATH Search

**Limitation:** Program must be an absolute path (e.g., `/bin/ls`, not just `ls`).

**Rationale:**
- `execve()` does not search PATH (unlike `execvp()`)
- Explicit paths prevent security issues (PATH injection)

**Workaround:** Users must specify full paths.

**Future:** Consider adding `which` lookup or `execvp()` option.

---

### 2. Environment Variable Merging

**Limitation:** Custom environment variables are appended, not merged/overwritten.

**Example:**
```bash
# If PATH is already set in parent:
./minicontainer --env PATH=/custom /bin/sh
# Child will have TWO PATH entries!
```

**Impact:** Last value wins (shell behavior), but wasteful.

**Future:** Implement proper deduplication/override logic.

---

### 3. Fixed Environment Array Size

**Limitation:** Maximum 255 custom environment variables (hardcoded in main.c).

**Rationale:**
- Simple fixed-size array avoids dynamic allocation complexity
- 255 is far more than typical use case

**Future:** Use dynamic array (realloc) for unlimited size.

---

### 4. No File Descriptor Management

**Limitation:** Child inherits all open file descriptors from parent.

**Impact:**
- Could leak file descriptors (sockets, pipes, etc.)
- Security concern in production environments

**Future:** Set `FD_CLOEXEC` on all fds except stdin/stdout/stderr, or close all fds > 2.

---

### 5. Single Command Execution

**Limitation:** Can only run one command, then exits.

**Rationale:** Phase 0 is deliberately simple - just fork/execve baseline.

**Future:** Phase 2+ will add container lifecycle management.

---

## Future Considerations

### Phase 1: PID Namespace Isolation

**Changes needed:**
- Replace `fork()` with `clone(CLONE_NEWPID | SIGCHLD, ...)`
- Allocate stack for child (clone requires explicit stack)
- Move execve logic into callback function
- Child will see itself as PID 1

**API compatibility:**
- `spawn_config_t` structure will remain compatible
- `spawn_process()` signature unchanged (internal implementation differs)

---

### Phase 2: Filesystem Isolation (chroot)

**Changes needed:**
- Add `rootfs` field to `spawn_config_t`
- Call `chroot()` in child before execve
- Handle `/proc`, `/dev` mounting

**Security concerns:**
- chroot is not a security boundary (can be escaped)
- Requires root privileges

---

### Phase 3: Mount Namespace

**Changes needed:**
- Add `CLONE_NEWNS` to clone flags
- Implement proper mount propagation
- Create private `/proc` mount for child

---

### Phase 4: Resource Limits (cgroups)

**Changes needed:**
- Add cgroup configuration to spawn_config_t
- Create/configure cgroup before fork
- Move child into cgroup
- Cleanup cgroup on exit

**Example fields:**
```c
typedef struct {
    // ... existing fields
    const char *cgroup_path;
    size_t memory_limit_bytes;
    int cpu_shares;
} spawn_config_t;
```

---

### Phase 5: Network Isolation

**Changes needed:**
- Add `CLONE_NEWNET` to clone flags
- Set up veth pairs
- Configure network namespace

---

## Testing Strategy

### Unit Tests (tests/test_spawn.c)

- ✓ Basic execution (`/bin/true`)
- ✓ Exit code propagation
- ✓ Signal death handling
- ✓ execve failure (command not found)

### Integration Tests (Makefile: make examples)

- ✓ Real commands (`/bin/ls`, `/bin/echo`)
- ✓ Debug output
- ✓ Environment variables
- ✓ Exit code forwarding

### Stress Tests

- Zombie prevention (1000+ rapid spawns)
- Memory leak detection (valgrind)
- Error path coverage

---

## References

- POSIX fork(2): Creates child process
- POSIX execve(2): Replaces process image
- POSIX waitpid(2): Waits for child state change
- POSIX sigaction(2): Installs signal handlers
- POSIX signal-safety(7): Async-signal-safe functions
- [Phase 0 Implementation Guide](../phase0_foundation_implementation_guide.md)
- [Linux System Programming by Robert Love](https://www.oreilly.com/library/view/linux-system-programming/9781449341527/)

---

**End of Design Decisions Document**
