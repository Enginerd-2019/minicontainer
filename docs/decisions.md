# Design Decisions and Error Log

**Project:** minicontainer - Minimal Container Runtime
**Phase:** 2 - Mount Namespace & Filesystem Isolation
**Last Updated:** 2026-02-27

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

### 7. Superseding spawn.c with namespace.c (Phase 1)

**Decision:** Replace `spawn.c` (`fork()`) with `namespace.c` (`clone()`) as the core execution module, rather than modifying `spawn.c` in place.

**Rationale:**
- **Clean separation:** `clone()` has a fundamentally different calling convention than `fork()` — it requires a child function pointer and explicit stack, making an in-place swap awkward
- **Superset behavior:** `namespace_exec()` handles both namespaced and non-namespaced execution via conditional flags (`SIGCHLD` alone vs `SIGCHLD | CLONE_NEWPID`), so it fully replaces `spawn_process()`
- **Testability:** Keeping `spawn.c`/`spawn.h` allows Phase 0 tests (`test_spawn.c`) to continue running unchanged as a regression baseline
- **New API surface:** `namespace_config_t` adds `enable_pid_namespace` field; `namespace_result_t` adds `stack_ptr` for cleanup — these don't fit cleanly into the Phase 0 types

**What changed in main.c:**
- `#include "spawn.h"` → `#include "namespace.h"`
- `spawn_config_t` → `namespace_config_t`
- `spawn_process()` → `namespace_exec()`
- `spawn_init_signals()` removed (namespace_exec handles wait internally; SIGCHLD handler could race with explicit waitpid)
- Added `namespace_cleanup()` call to free clone stack

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

### 8. Build Minimal rootfs Manually vs. Download Alpine minirootfs (Phase 2)

**Decision:** Build the rootfs by hand (Option 2) rather than downloading a pre-built Alpine minirootfs.

**Options considered:**
- **Option 1 — Alpine minirootfs:** Download a ~2.7 MB tarball containing a complete musl-based userland (busybox, apk, /etc skeleton)
- **Option 2 — Manual rootfs:** Create the directory tree manually, copy only the specific binaries needed (`/bin/sh`, `/bin/ls`, `/bin/echo`), and resolve shared library dependencies with `ldd`

**Rationale:**
- **Educational value:** Manually assembling a rootfs teaches exactly what a root filesystem requires — which directories the kernel expects, which shared libraries a dynamically-linked binary depends on, and how the ELF loader (`ld-linux-x86-64.so.2`) resolves them. Downloading a tarball hides all of this.
- **Minimal surface area:** The hand-built rootfs contains only the binaries explicitly copied. Alpine's minirootfs ships busybox (300+ applets), apk, and configuration files that are irrelevant to understanding mount namespace isolation.
- **No network dependency:** Option 2 works entirely offline using binaries already on the host. Option 1 requires downloading from `dl-cdn.alpinelinux.org`, adding a failure mode unrelated to the project.
- **Faster iteration:** Rebuilding the rootfs is a few `cp` commands. No re-downloading or re-extracting.

**Trade-offs:**
- The manual rootfs is host-libc-dependent (glibc binaries from the host won't work inside a musl-based container, but that's irrelevant here since the container uses the same host libraries)
- Adding new binaries requires manually resolving their library dependencies via `ldd`
- Alpine minirootfs would provide a more "realistic" container environment with a package manager

---

### 9. Superseding namespace.c with mount.c (Phase 2)

**Decision:** Replace `namespace.c` (`clone(CLONE_NEWPID)`) with `mount.c` (`clone(CLONE_NEWPID | CLONE_NEWNS)`) as the core execution module, following the same pattern used when superseding `spawn.c` in Phase 1.

**Rationale:**
- **New responsibilities:** Phase 2 adds rootfs pivot, `/proc` mounting, and mount propagation management — these are filesystem operations that don't belong in `namespace.c` which was scoped to PID isolation
- **Superset behavior:** `mount_exec()` handles PID namespace, mount namespace, and non-namespaced execution via conditional flags, fully replacing `namespace_exec()`
- **Clean API surface:** `mount_config_t` adds `enable_mount_namespace` and `rootfs_path` fields; `mount_result_t` is structurally identical to `namespace_result_t` but namespaced under the mount module
- **Testability:** Keeping `namespace.c`/`namespace.h` allows Phase 1 tests to continue running unchanged

**What changed in main.c:**
- `#include "namespace.h"` → `#include "mount.h"`
- `namespace_config_t` → `mount_config_t` (with new fields)
- `namespace_exec()` → `mount_exec()`
- `namespace_cleanup()` → `mount_cleanup()`
- Added `--rootfs <path>` option (auto-enables mount namespace)
- Removed `--env` option (simplification; environment inheritance via `envp = NULL`)

**What changed in Makefile:**
- `$(MINICONTAINER)` link target: `main.o + namespace.o` → `main.o + mount.o`
- Added `test_mount` target linked against `mount.o`

---

### 10. Why pivot_root() Instead of chroot() (Phase 2)

**Decision:** Use `pivot_root()` syscall via `syscall(SYS_pivot_root, ...)` instead of `chroot()`.

**Rationale:**
- **True isolation:** `chroot()` only changes the apparent root directory — processes can escape via `fchdir()` to an open fd, `..` traversal from a working directory outside the root, or a second `chroot()` call. `pivot_root()` actually swaps the root mount, leaving no reference to the old root once it's unmounted
- **Mount namespace integration:** `pivot_root()` is designed to work with `CLONE_NEWNS` — the old root becomes a submount that can be cleanly unmounted. `chroot()` has no mount semantics
- **Industry standard:** runc, crun, and Docker all use `pivot_root()` for container rootfs setup
- **Kernel enforcement:** `pivot_root()` requires the new root to be a mount point in a private subtree, which forces the caller to set up proper mount isolation — a correctness guardrail that `chroot()` lacks

**Trade-offs:**
- `pivot_root()` has no glibc wrapper — must use `syscall(SYS_pivot_root, new_root, put_old)` directly
- Requires the new root to be a mount point (handled by bind-mounting it to itself)
- Requires non-shared mount propagation (handled by `mount(MS_PRIVATE | MS_REC)`)
- More setup steps than `chroot(path); chdir("/");`

---

### 11. Mount Propagation Strategy (Phase 2)

**Decision:** Make the entire mount tree private (`MS_PRIVATE | MS_REC`) before performing any other mount operations in `setup_rootfs()`.

**Rationale:**
- **Systemd compatibility:** Systemd sets `/` to shared propagation at boot. `CLONE_NEWNS` inherits this into the child. Without making mounts private, `pivot_root()` returns `EINVAL` on all systemd-based systems
- **Isolation guarantee:** Private propagation ensures mount/unmount events inside the container don't leak to the host namespace, and vice versa
- **Two-call pattern:** `mount(2)` requires propagation flags in a separate call from other flags. The bind mount and propagation change for the rootfs are therefore two calls, not one (see Error #5)

**Order of operations in `setup_rootfs()`:**
1. `mount("", "/", NULL, MS_PRIVATE | MS_REC, NULL)` — make inherited tree private
2. `mount(abs_path, abs_path, NULL, MS_BIND | MS_REC, NULL)` — bind mount rootfs to itself
3. `mount("", abs_path, NULL, MS_PRIVATE | MS_REC, NULL)` — make rootfs subtree private
4. `chdir(abs_path)` → `mkdir("old_root")` → `pivot_root(".", "old_root")`
5. `chdir("/")` → `umount2("/old_root", MNT_DETACH)` → `rmdir("/old_root")`

---

### 12. Lazy Unmount for Old Root (Phase 2)

**Decision:** Use `umount2("/old_root", MNT_DETACH)` (lazy unmount) instead of `umount("/old_root")`.

**Rationale:**
- **Robustness:** A regular `umount()` fails with `EBUSY` if any process has an open file descriptor or working directory under the mount point. `MNT_DETACH` detaches the mount from the namespace immediately, then cleans up resources as references are dropped
- **Simplicity:** No need to track and close all references to the old root before unmounting
- **Safety:** The container is already in its new rootfs at this point; the old root is unreachable from the new namespace tree after detach

**Trade-off:** Lazy unmount means the actual cleanup of kernel resources is deferred. In practice this is negligible since the old root references drain almost immediately after detach.

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

### 5. Why clone() Instead of fork() (Phase 1)

**Choice:** Use `clone()` with namespace flags instead of continuing with `fork()`.

**Rationale:**
- **Namespace support:** `fork()` cannot create new namespaces — `clone()` accepts `CLONE_NEWPID`, `CLONE_NEWNS`, etc. as flags
- **Extensibility:** The same `clone()` call will absorb additional namespace flags in Phase 2+ (`CLONE_NEWNS`, `CLONE_NEWUTS`, `CLONE_NEWNET`) without changing the calling pattern
- **Backward compatibility:** Without `CLONE_NEWPID`, `clone()` with just `SIGCHLD` behaves like `fork()`, so one codepath handles both modes
- **Trade-off:** `clone()` requires manual stack allocation (unlike `fork()`), but this is a one-time cost that enables all future namespace features

**Why not `unshare()` + `fork()`?**
- `unshare(CLONE_NEWPID)` affects the *calling* process's children, not the caller itself — the parent would need to fork after unshare, adding complexity
- `clone()` creates the child directly in the new namespace in a single syscall

---

### 6. malloc() for clone() Stack

**Choice:** Use `malloc()` to allocate the child's stack rather than `mmap()`.

**Rationale:**
- **Simplicity:** `malloc(STACK_SIZE)` is a single call vs `mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0)`
- **Portability:** `malloc()` handles alignment automatically on all platforms
- **Sufficient for our use case:** The stack is only used briefly before `execve()` replaces the process image entirely — the kernel allocates a new stack for the exec'd program
- **Easy cleanup:** `free()` in `namespace_cleanup()` vs `munmap()` with size tracking

**Stack size:** 1 MB (`1024 * 1024`), which is standard for most programs and sufficient for the brief period before `execve()`.

**Stack direction:** x86_64 stacks grow downward, so `clone()` receives `stack + STACK_SIZE` (top), not `stack` (base).

**Alternative considered:** `mmap()` with `MAP_STACK` and `MAP_GROWSDOWN` would provide guard pages for stack overflow detection, but adds complexity not needed at this phase.

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

### Error #5: MS_BIND | MS_PRIVATE Combined in Single mount() Call

**Location:** `src/mount.c:46`

**Original Code:**
```c
if(mount(abs_path, abs_path, NULL, MS_BIND | MS_PRIVATE, NULL) < 0){
```

**Problem:**
Per `mount(2)`, propagation type flags (`MS_SHARED`, `MS_PRIVATE`, `MS_SLAVE`, `MS_UNBINDABLE`) cannot be combined with other flags in a single `mount()` call. The kernel silently ignores `MS_PRIVATE` when combined with `MS_BIND`, leaving the mount with the parent's default propagation (typically shared). This means mount/unmount events could propagate between the container and host namespaces, defeating the purpose of namespace isolation. On stricter kernels, `pivot_root` may also return `EINVAL` if the new root is not in a private mount subtree.

**Fixed Code:**
```c
if(mount(abs_path, abs_path, NULL, MS_BIND | MS_REC, NULL) < 0){
    perror("mount(MS_BIND)");
    return -1;
}

if(mount("", abs_path, NULL, MS_PRIVATE | MS_REC, NULL) < 0){
    perror("mount(MS_PRIVATE)");
    return -1;
}
```

**What changed:**
- Split into two `mount()` calls: bind mount first, then set propagation
- Added `MS_REC` to both calls so submounts are included in the bind and also marked private

**Impact:**
- **Severity:** High - mount propagation was not actually being set to private
- **Symptom:** Silent; the call succeeded but `MS_PRIVATE` was ignored
- **Root Cause:** `mount(2)` man page states propagation flags require a separate call

**Fix Applied:** 2026-02-22

### Error #6: pivot_root EINVAL Due to Shared Root Propagation

**Location:** `src/mount.c` — `setup_rootfs()`

**Symptom:**
```
pivot_root: Invalid argument
[child] Failed to setup rootfs
```

**Problem:**
Systemd sets `/` to shared propagation at boot. `CLONE_NEWNS` inherits this propagation into the child's mount namespace. `pivot_root` requires the current root to not be shared — if it is, the kernel returns `EINVAL`. The original code made `abs_path` (the new rootfs) private but never addressed the current root's propagation.

**Fixed Code:**
```c
// Added as first mount operation in setup_rootfs, before bind mount
if(mount("", "/", NULL, MS_PRIVATE | MS_REC, NULL) < 0){
    perror("mount(MS_PRIVATE /)");
    return -1;
}
```

**What changed:**
- Make the entire inherited mount tree private before any other mount operations
- This is safe because we're in a new mount namespace (`CLONE_NEWNS`) — the parent's mounts are unaffected

**Impact:**
- **Severity:** Critical — `pivot_root` fails on all systemd-based systems without this
- **Symptom:** `pivot_root: Invalid argument` at runtime
- **Root Cause:** Inherited shared propagation from systemd
- **Prior art:** runc, Docker, and LXC all perform this step

**Fix Applied:** 2026-02-23

---

## Known Limitations

### 1. No PATH Search

**Limitation:** Program must be an absolute path (e.g., `/bin/ls`, not just `ls`).

**Rationale:**
- `execve()` does not search PATH (unlike `execvp()`)
- Explicit paths prevent security issues (PATH injection)

**Workaround:** Users must specify full paths. When using `--rootfs`, the path must resolve inside the rootfs.

**Future:** Consider adding `which` lookup or `execvp()` option.

---

### 2. No File Descriptor Management

**Limitation:** Child inherits all open file descriptors from parent.

**Impact:**
- Could leak file descriptors (sockets, pipes, etc.)
- Security concern in production environments

**Future:** Set `FD_CLOEXEC` on all fds except stdin/stdout/stderr, or close all fds > 2.

---

### 3. Single Command Execution

**Limitation:** Can only run one command, then exits.

**Rationale:** Deliberately simple — lifecycle management comes in Phase 6.

**Future:** Phase 6 will add start/stop/exec container lifecycle management.

---

### 4. Rootfs Must Be Pre-Built

**Limitation:** The `--rootfs` flag expects a pre-existing directory with a complete filesystem tree. There is no image pull, layer extraction, or rootfs generation.

**Rationale:** Educational focus — manually building a rootfs teaches what a root filesystem requires (see Architecture Decision #8).

**Workaround:** Build rootfs manually by copying binaries and their shared library dependencies via `ldd`.

---

### 5. No tmpfs or Device Mounts

**Limitation:** Only `/proc` is automatically mounted inside the container. `/tmp`, `/dev`, `/sys`, and other standard mounts are not set up.

**Impact:** Programs that expect `/dev/null`, `/dev/urandom`, or `/tmp` will fail inside the container unless the rootfs includes them as regular files/directories.

**Future:** Add `/dev` (minimal device nodes), `/tmp` (tmpfs), and `/sys` (sysfs) mounts in the child setup.

---

## Future Considerations

### ✅ Phase 1: PID Namespace Isolation (Complete)

**What was done:**
- Replaced `fork()` with `clone(CLONE_NEWPID | SIGCHLD, ...)`
- Allocated stack for child via `malloc()`
- Moved execve logic into `child_func()` callback
- Child sees itself as PID 1

**API outcome (differs from original plan):**
- Created new `namespace_config_t` and `namespace_result_t` types rather than extending `spawn_config_t`
- `spawn_process()` replaced by `namespace_exec()`, not modified
- Added `namespace_cleanup()` for stack deallocation
- See Architecture Decision #7 for rationale

---

### ✅ Phase 2: Mount Namespace & Filesystem Isolation (Complete)

**What was done:**
- Created new `mount.c`/`mount.h` module superseding `namespace.c` (see Architecture Decision #9)
- Added `CLONE_NEWNS` to clone flags alongside `CLONE_NEWPID`
- Implemented `setup_rootfs()`: bind mount → private propagation → `pivot_root()` → unmount old root
- Implemented `mount_proc()`: mounts `/proc` filesystem after pivot for PID namespace visibility
- Created `mount_config_t` with `enable_mount_namespace` and `rootfs_path` fields
- Added `--rootfs <path>` CLI flag that auto-enables mount namespace
- Removed `--env` flag from main.c (simplified; environment inherited via `envp = NULL`)

**API outcome (differs from original plan):**
- Created new `mount_config_t` and `mount_result_t` types rather than extending `namespace_config_t`
- `namespace_exec()` replaced by `mount_exec()`, not modified
- `mount_exec()` handles both PID and mount namespaces via conditional flag building
- See Architecture Decision #9 for rationale

**Key implementation details:**
- `pivot_root()` called via `syscall(SYS_pivot_root)` — no glibc wrapper exists (see Decision #10)
- Mount propagation set to private before any other mount ops — required for systemd compatibility (see Decision #11, Error #6)
- Bind mount and propagation change are separate `mount()` calls — kernel requirement (see Error #5)
- Old root lazily unmounted via `MNT_DETACH` (see Decision #12)

---

### Phase 3: UTS & User Namespace

**Changes needed:**
- Add `CLONE_NEWUTS` to clone flags in `mount_exec()` for hostname isolation
- Add `CLONE_NEWUSER` for UID/GID mapping
- Add `hostname` and `uid_map`/`gid_map` fields to `mount_config_t`
- Enable rootless (unprivileged) container creation

---

### Phase 4: Resource Limits (cgroups)

**Changes needed:**
- Add cgroup configuration to `mount_config_t`
- Create/configure cgroup before clone
- Move child into cgroup
- Cleanup cgroup on exit

---

### Phase 5: Network Isolation

**Changes needed:**
- Add `CLONE_NEWNET` to clone flags
- Set up veth pairs
- Configure network namespace

---

## Testing Strategy

### Phase 0 Unit Tests (tests/test_spawn.c)

- ✓ Basic execution (`/bin/true`)
- ✓ Exit code propagation
- ✓ Signal death handling
- ✓ execve failure (command not found)

### Phase 1 Unit Tests (tests/test_namespace.c — requires root)

- ✓ PID namespace isolation (child is PID 1)
- ✓ Non-namespaced execution (fallback path)
- ✓ Stack allocation and cleanup

### Phase 2 Unit Tests (tests/test_mount.c — requires root + rootfs)

- ✓ Rootfs isolation (container sees only rootfs contents)
- ✓ /proc mount (proc filesystem mounted and accessible inside container)

### Integration Tests (Makefile: make examples)

- ✓ Real commands (`/bin/ls`, `/bin/echo`)
- ✓ Debug output
- ✓ Exit code forwarding
- ✓ PID namespace with `--pid` flag
- ✓ Rootfs isolation with `--rootfs ./rootfs`
- ✓ Full isolation (`--pid --rootfs ./rootfs`)
- ✓ Debug with full isolation

### Stress Tests

- Memory leak detection (valgrind)
- Error path coverage

---

## References

- pivot_root(2): Change the root filesystem
- mount(2): Mount filesystem, set propagation flags
- umount2(2): Unmount filesystem with flags (MNT_DETACH)
- mount_namespaces(7): Mount namespace details and propagation
- clone(2): Creates child process with namespace flags
- fork(2): Creates child process
- execve(2): Replaces process image
- waitpid(2): Waits for child state change
- sigaction(2): Installs signal handlers
- namespaces(7): Overview of Linux namespaces
- pid_namespaces(7): PID namespace details
- signal-safety(7): Async-signal-safe functions
- [LWN: Namespaces in operation](https://lwn.net/Articles/531114/)
- [LWN: Mount namespaces and shared subtrees](https://lwn.net/Articles/689856/)
- [Linux System Programming by Robert Love](https://www.oreilly.com/library/view/linux-system-programming/9781449341527/)

---

**End of Design Decisions Document**
