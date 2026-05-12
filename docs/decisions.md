# Design Decisions and Error Log

**Project:** minicontainer - Minimal Container Runtime
**Phase:** 5 - cgroups v2 (Resource Limits)
**Last Updated:** 2026-05-11

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

### 13. Superseding mount.c with overlay.c (Phase 3)

**Decision:** Replace `mount.c` (`clone(CLONE_NEWPID | CLONE_NEWNS)`) with `overlay.c` as the core execution module, continuing the superseding pattern from Phases 1 and 2.

**Rationale:**
- **New responsibilities:** Phase 3 adds OverlayFS setup/teardown, environment isolation, and inherited fd cleanup — filesystem-layer concerns that don't belong in `mount.c`
- **Superset behavior:** `overlay_exec()` handles overlay, non-overlay, namespaced, and non-namespaced execution, fully replacing `mount_exec()`
- **Function reuse:** `setup_rootfs()` and `mount_proc()` are imported from `mount.h` and called unchanged — the child receives `merged/` as its rootfs path instead of the raw rootfs, transparently
- **Parent-managed lifecycle:** Overlay is created before `clone()` and torn down after `waitpid()`, because the child's mount namespace is isolated from the parent

**What changed in main.c:**
- `#include "mount.h"` → `#include "overlay.h"`
- `mount_config_t` → `overlay_config_t` (adds `enable_overlay`, `container_dir`)
- `mount_exec()` → `overlay_exec()`
- `mount_cleanup()` → `overlay_cleanup()`
- Added `--overlay` flag, `--env` support, misplaced-flag detection
- Added `build_container_env()` to construct minimal container environment

---

### 14. OverlayFS over Alternatives (Phase 3)

**Decision:** Use OverlayFS for copy-on-write filesystem isolation instead of full rootfs copies, read-only bind mounts, or tmpfs.

**Rationale:**
- **Full rootfs copies (rejected):** 50–200 MB per container; wasteful when most files are read-only
- **Read-only bind mounts (rejected):** Prevents all writes, breaking temp files, logs, and config — containers need a writable view
- **tmpfs for writable layer (rejected):** Memory-backed; large writes consume RAM with no disk backing
- **OverlayFS:** Shares a read-only base image (lowerdir) across containers; each container gets its own writable upper layer on disk; writes are copy-on-write; changes are discarded on teardown

**Security hardening:** Overlay is mounted with `MS_NODEV | MS_NOSUID` — `MS_NOSUID` prevents setuid escalation inside the container, `MS_NODEV` prevents device node creation. This matches Docker/Podman/runc defaults.

---

### 15. Environment Isolation and FD Cleanup (Phase 3)

**Decision:** Construct a minimal container environment instead of inheriting the host's `environ`, and close all inherited file descriptors before `execve()`.

**Environment rationale:**
- Phases 0–2 inherited the full host environment (`PATH`, `HOME`, `SHELL`, etc.), which referenced host paths that don't exist inside the container rootfs
- `build_container_env()` constructs a minimal baseline (`PATH=/usr/bin:/bin`, `HOME=/root`, `TERM`) — matches Docker's approach
- User `--env` overrides replace on duplicate key rather than appending a second entry

**FD cleanup rationale:**
- After `clone()`, the child inherits every open fd from the parent. Any fd referencing the host filesystem survives `pivot_root()` + `MNT_DETACH` and enables container escape (CVE-2024-21626, CVE-2016-9962)
- `close_inherited_fds()` iterates `/proc/self/fd` (O(open fds), not O(max fds)) and closes everything above `STDERR_FILENO`
- Called after `mount_proc()` so `/proc/self/fd` is available, with a brute-force fallback if it isn't

---

### 16. Superseding overlay.c with uts.c (Phase 4)

**Decision:** Replace `overlay.c` with `uts.c` as the core execution module, continuing the superseding pattern.

**Rationale:**
- **New responsibilities:** Phase 4 adds UTS namespace isolation (`CLONE_NEWUTS`) and hostname configuration — identity concerns that don't belong in `overlay.c`
- **Superset behavior:** `uts_exec()` handles everything `overlay_exec()` did plus UTS namespace creation and hostname setting
- **Function reuse:** `setup_overlay()`, `teardown_overlay()`, `setup_rootfs()`, and `mount_proc()` are all imported and called unchanged
- **Acknowledged technical debt:** By Phase 4, the `*_exec()` functions are ~90% identical boilerplate — documented as a future refactoring trigger (Phase 7 CLI refactor)

**What changed in main.c:**
- `#include "overlay.h"` → `#include "uts.h"`
- `overlay_config_t` → `uts_config_t` (adds `enable_uts_namespace`, `hostname`)
- `overlay_exec()` → `uts_exec()`
- `overlay_cleanup()` → `uts_cleanup()`
- Added `--hostname <name>` flag (auto-enables UTS namespace)

---

### 17. sethostname() in Child After clone() (Phase 4)

**Decision:** Call `sethostname()` inside the child process after `clone(CLONE_NEWUTS)`, not in the parent.

**Rationale:**
- **Namespace scoping:** `CLONE_NEWUTS` creates the new UTS namespace for the *child*. The parent remains in the host's UTS namespace. Calling `sethostname()` in the parent would change the host's hostname globally
- **Minimal implementation:** One flag (`CLONE_NEWUTS`) added to the existing `clone()` call + one `sethostname()` call in the child — no /proc writes, no synchronization, no cleanup
- **Timing:** `setup_uts()` is called early in the child, before `setup_rootfs()`, so the hostname is set before the container's filesystem is configured

**Alternative considered:** `unshare(CLONE_NEWUTS)` — rejected because `clone()` is already used for PID/mount namespaces, and adding another flag to the same call is simpler and creates all namespaces atomically.

---

### 18. Extending uts.c for User Namespace Instead of New Module (Phase 4b)

**Decision:** Add `CLONE_NEWUSER`, sync pipe, and UID/GID mapping support directly to `uts.c` and `uts.h` rather than creating a new `user.c` module.

**Rationale:**
- **No new child-side logic:** Unlike UTS (which added `sethostname()`) or overlay (which added `setup_overlay()`/`teardown_overlay()`), user namespaces add no new function the child calls. The child just blocks on a pipe read — the real work is parent-side (`setup_user_namespace_mapping()`)
- **Changes are to `uts_exec()`:** Add `CLONE_NEWUSER` flag, create pipe, write maps, signal child. These are additions to the existing orchestration function, not a new execution path
- **Avoids compounding code smell:** Creating `user.c` would produce yet another near-identical `*_exec()` function (~90% duplicate), worsening the technical debt noted in Phase 4

**What changed in uts.h:**
- `uts_config_t` gains `enable_user_namespace` (bool) and 6 UID/GID mapping fields
- New `setup_user_namespace_mapping()` function signature

**What changed in uts.c:**
- `child_args_t` gains `sync_fd` (pipe read end) and `user_namespace_active` (bool)
- `child_func()` blocks on `read(sync_fd)` before doing privileged work
- `uts_exec()` creates pipe, passes `CLONE_NEWUSER` to `clone()`, writes UID/GID maps via `/proc/<pid>/{setgroups,uid_map,gid_map}`, signals child
- Error path closes pipe and kills child if mapping fails

**What changed in main.c:**
- Added `--user` flag and `enable_user_namespace` variable
- Default mapping: container UID 0 → `getuid()`, container GID 0 → `getgid()`
- Added `--user` to `known_flags[]` for misplaced-flag detection

---

### 19. Proc Mount Hardening with MS_NOSUID | MS_NODEV | MS_NOEXEC (Phase 4b)

**Decision:** Mount `/proc` with `MS_NOSUID | MS_NODEV | MS_NOEXEC` flags instead of `0`.

**Rationale:**
- **User namespace requirement:** When mounting proc inside a user namespace, the kernel rejects mounts that are less restrictive than the existing proc mount. Most distributions mount `/proc` with `MS_NOSUID | MS_NODEV | MS_NOEXEC`. A proc mount with flags `0` (the Phase 2 default) is rejected with `EPERM`
- **Good hardening regardless:** There is no legitimate reason to allow setuid binaries, device nodes, or direct execution from `/proc`. Docker, Podman, and runc all mount proc with these flags
- **Backward compatible:** Adding restrictive flags to the mount call does not change behavior for privileged (non-`--user`) containers — it just makes them more secure by default

**Change in mount.c:**
```c
// Before (Phase 2):
mount("proc", "/proc", "proc", 0, NULL)

// After (Phase 4b):
mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL)
```

---

### 20. Graceful /proc Degradation in User Namespace (Phase 4b)

**Decision:** When running in a user namespace (`--user`), downgrade `/proc` mount failure to a warning instead of aborting the container.

**Rationale:**
- **Kernel/AppArmor restrictions:** Even with correct mount flags (Decision #19), the kernel or AppArmor (on Ubuntu 23.10+) may deny proc mounts from within unprivileged user namespaces. This is a platform restriction, not a minicontainer bug
- **Most commands don't need /proc:** `ls`, `echo`, `hostname`, `id`, `sh` all function without a mounted `/proc`. Only `/proc`-dependent tools like `ps` are affected
- **Existing fallback:** `close_inherited_fds()` (Phase 3 §3.5) already has a brute-force fallback that closes fds 3 through `RLIMIT_NOFILE` when `/proc/self/fd` is unavailable. This fallback activates automatically when proc is not mounted — there is no security gap
- **Fatal without --user:** When running without `--user` (privileged mode), the proc mount failure remains fatal. In that context, a failed proc mount indicates a real configuration problem that should not be silently ignored

**Implementation in uts.c:**
- `child_args_t` gains `user_namespace_active` field, set from `config->enable_user_namespace`
- `child_func()` checks `args->user_namespace_active` when `mount_proc()` fails: if true, prints warning and continues; if false, returns error (existing behavior)

---

### 21. build_container_env() Refactored for Future Extraction (Phase 5)

**Decision:** During Phase 5, refactor `build_container_env()` in `main.c` from
an exact-size `malloc`-based implementation that trusts caller invariants into
a fixed-ceiling `calloc`-based implementation with a bounds check at every
append. The algorithm is unchanged; the safety properties are stronger.

**Background — what was there:**

The Phase 3/4/4b/4c version of `build_container_env()` looked like this in
shape:

```c
int custom_count = 0;
if (custom_env) while (custom_env[custom_count]) custom_count++;

char **env = malloc((count + custom_count + 1) * sizeof(char *));
// ... copy defaults, set NULL terminator ...

for (int i = 0; i < custom_count; i++) {
    // ... replace-or-append ...
    if (!replaced) {
        env[count] = custom_env[i];  /* No bounds check — sizing was exact. */
        count++;
        env[count] = NULL;
    }
}
```

This version is **correct for the current codebase**. `main.c` caps
`custom_env` at `MAX_ENV_ENTRIES - 1` before calling, and `custom_env` is a
stable single-threaded array, so the upfront `custom_count` is always
accurate and the append never overflows.

**The Phase 5 refactor:**

```c
int max_entries = count + MAX_ENV_ENTRIES + 1;
char **env = calloc(max_entries, sizeof(char *));
// ... copy defaults (no terminator needed — calloc handles it) ...

if (custom_env) {
    for (int i = 0; custom_env[i]; i++) {
        // ... replace-or-append ...
        if (!replaced && count < max_entries - 1) {
            env[count++] = custom_env[i];
        }
    }
}

env[count] = NULL;  /* Explicit terminator at end (calloc placed one too). */
```

Three changes:
1. **`calloc` instead of `malloc`** — every slot is zero-initialized (implicit
   NULL terminator throughout the array).
2. **Fixed compile-time ceiling instead of exact size** — `MAX_ENV_ENTRIES` is
   a constant, so the size calculation cannot be wrong (no integer overflow,
   no reliance on a caller-supplied count).
3. **Bounds check at every append** — `if (!replaced && count < max_entries - 1)`.
   Excess entries are silently dropped rather than written past the
   allocation.

**Rationale:**

- **Phase 7 plans to extract `*_exec()` boilerplate into a shared execution
  core.** `build_container_env()` will likely be pulled along with it into a
  shared utility module (or `libprocfs`-style helper library) — at which point
  it can be called from contexts that don't pre-validate input. Making the
  function self-contained **now** means the Phase 7 extraction is mechanical
  (move the file) rather than a correctness refactor.

- **Defense-in-depth at the function level.** The architecture-level
  defense-in-depth pattern (independent isolation layers — `pivot_root`,
  `close_inherited_fds`, `MS_NODEV|MS_NOSUID`, etc.) is mirrored here at the
  function level: `calloc` defends against reads from
  between-write slots; the fixed ceiling defends against integer overflow and
  caller-size lies; the bounds check defends against buffer overflow if
  anything else fails. Each defense is redundant given the current `main.c`
  caller, but each guards a different failure mode the function might face
  after extraction.

- **Spend complexity budget when extraction is foreseeable.** The Phase 4c
  "minimal diff" pattern (Decision #16/#17) resisted abstraction until
  repetition proved it. This decision is the inverse: invest preemptively
  when the future use case is **known** (Phase 7 is scheduled, not
  speculative). The distinction is certainty: structural abstraction is
  speculative; a function known to be extracted has a foreseeable future
  caller environment to defend against.

- **No behavioral change for current users.** Both versions produce identical
  output for valid input. The bounds check can never trigger via the
  CLI-driven path because `main.c` caps `custom_env` at `MAX_ENV_ENTRIES - 1`.
  The cost is ~5 lines of code and a slight constant overhead per call
  (`calloc` zero-inits). Reverting if needed is trivial.

**What the principle illustrates:**

When code transitions from "tightly coupled internal helper" to "library
function" — even within the same codebase — it has to stop trusting its
caller. Long-lived software accumulates callers and use cases the original
author didn't anticipate; a function that's safe *only because of caller
invariants* is a latent bug waiting for the day those invariants stop
holding. The refactor cost is small now and prevents a future correctness
audit during the Phase 7 extraction.

**Tension with Decision #16 (minimal diff resists abstraction):**

This decision adds *local* complexity (a few defensive lines inside one
function). Decision #16 resisted *structural* complexity (modules,
interfaces, indirection). The reconciliation: structural abstraction is
expensive to revert and obscures the data flow; local defensive code is
cheap to revert and self-documenting. The project accepts the latter
preemptively; the former waits for proof of necessity.

**Files affected:**
- `src/main.c` — `build_container_env()` rewritten in-place
- `phase5_cgroups_implementation_guide.md` §3.7 — full rationale with worked
  comparison
- `SRE.md` §5.4 and §8.4 — SRE framing as "code that survives" and risk-budget
  decision

**Fix Applied:** 2026-05-11

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

### Error #7: Silent Option Swallowing with POSIX-Strict getopt

**Location:** `src/main.c` — getopt loop (all phases from Phase 1 onward)

**Symptom:**
```bash
./minicontainer --rootfs ./rootfs /bin/sh --pid --overlay
# Container runs without PID namespace or overlay — no error message
```

**Problem:**
The `+` prefix in the getopt optstring (introduced in Phase 1 to fix Error #3) enforces POSIX-strict parsing, which stops option processing at the first non-option argument. This correctly prevents child command flags like `-la` from being misinterpreted as minicontainer options. However, the tradeoff is that any minicontainer flag placed **after** the command path is silently absorbed into the child's `argv`. The container runs but without the isolation the user intended, and there is no diagnostic output.

The risk scales with each phase: Phase 3 adds `--overlay`, `--container-dir`, and `--env`, making it increasingly likely that a user places a flag after the command by mistake.

**Fixed Code:**
```c
// Detect whether the user wrote '--' to explicitly separate args
bool explicit_separator = false;
if (optind > 1 && strcmp(argv[optind - 1], "--") == 0) {
    explicit_separator = true;
}

// Catch minicontainer flags that landed after the command
if (!explicit_separator) {
    static const char *known_flags[] = {
        "--debug", "--pid", "--rootfs", "--overlay",
        "--container-dir", "--env", "--help", NULL
    };

    for (int i = optind + 1; i < argc; i++) {
        for (int j = 0; known_flags[j]; j++) {
            if (strcmp(argv[i], known_flags[j]) == 0) {
                fprintf(stderr,
                    "Error: '%s' appears after command '%s' (argv[%d])\n"
                    "All minicontainer options must precede the command.\n"
                    "Use '--' to pass flags to the child: "
                    "%s [options] -- %s %s\n",
                    argv[i], argv[optind], i,
                    argv[0], argv[optind], argv[i]);
                return 1;
            }
        }
    }
}
```

**Design decisions:**
- **Scan starts at `optind + 1`:** `argv[optind]` is the command itself — it shouldn't trigger the check even if it collides with a flag name
- **`--` suppresses the scan:** Standard POSIX convention for separating tool options from child arguments. Eliminates false positives when a child command legitimately uses `--help`, `--debug`, etc. — this same mechanism handles short flag collisions like `-e` or `-p`, so a separate short-flag check would add noise without adding safety
- **Long options only:** Short flags like `-d` or `-p` are not checked because they're commonly used by child programs (`echo -e`, `kill -p`). The `--` separator already covers any case where a short flag collision matters
- **`known_flags` must stay in sync with `long_options[]`:** When adding flags in future phases (e.g., `--hostname` in Phase 4), add them to both arrays

**Trade-offs:**
- **Misplaced short flags are undetected:** `./minicontainer --rootfs ./rootfs /bin/sh -p` silently passes `-p` to `/bin/sh` with no PID namespace. Accepted because short flags are pervasive in child arguments (`echo -e`, `grep -c`), and flagging them would produce false positives on common invocations. The `--` separator handles disambiguation when needed.
- **Maintenance burden:** `known_flags` must be manually kept in sync with `long_options[]`. A flag added to one but not the other silently bypasses the check. No compile-time enforcement — this is a discipline requirement mitigated by a cross-referencing comment in the code.
- **False positives on colliding long names:** A child program that accepts `--debug` or `--help` will trigger the error. The `--` separator resolves this, and the error message itself teaches the fix.

**Impact:**
- **Severity:** Medium — silent misconfiguration with no error message
- **Symptom:** Container runs without intended isolation; user receives no warning
- **Root Cause:** POSIX-strict getopt stops at the first non-option, silently absorbing subsequent options into the child's argv
- **Relationship to Error #3:** Error #3 introduced the `+` prefix to fix child flag parsing; Error #7 addresses the tradeoff introduced by that fix

**Fix Applied:** 2026-03-13

---

### Error #8: `_GNU_SOURCE` Redefined Warning (Makefile vs Source)

**Date Found:** 2026-03-30

**Problem:** The Makefile defines `_GNU_SOURCE` via CFLAGS (`-D_GNU_SOURCE`),
but source files (`main.c`, `overlay.c`, `test_overlay.c`) also `#define
_GNU_SOURCE` at the top. GCC emits a `-Wredefined` warning for each file.

**Symptom:** Build output contains warnings like:
```
src/overlay.c:1: warning: "_GNU_SOURCE" redefined
<command-line>: note: this is the location of the previous definition
```

**Root Cause:** The `#define _GNU_SOURCE` in source files predates the Makefile
adding `-D_GNU_SOURCE` to CFLAGS. Both are correct individually — the source
`#define` ensures the file compiles standalone, while the CFLAGS flag ensures
all files get it even if they forget. Together they produce a harmless but noisy
warning.

**Impact:**
- **Severity:** Low — warning only, no behavior change
- **Symptom:** Cluttered build output obscures real warnings

**Fix:** Remove `#define _GNU_SOURCE` from source files that are always built
via the Makefile. The CFLAGS definition is sufficient.

**Fix Applied:** 2026-03-30

---

### Error #9: `unmount2` Typo — Undefined Reference at Link Time

**Date Found:** 2026-03-30

**Problem:** `teardown_overlay()` in `overlay.c` called `unmount2()` instead of
`umount2()`. The compiler emitted an implicit declaration warning, and the
linker failed with an undefined reference.

**Symptom:**
```
src/overlay.c:258:12: warning: implicit declaration of function 'unmount2';
    did you mean 'umount2'?
/usr/bin/ld: overlay.c:(.text+0x7c2): undefined reference to `unmount2'
```

**Root Cause:** Typo — `unmount2` vs the correct `umount2`. The function name
follows the Unix convention of abbreviated names (`umount`, not `unmount`).

**Impact:**
- **Severity:** High — build fails, linker error
- **Symptom:** `make` exits with error, no binary produced

**Fix:** Changed `unmount2` to `umount2` and `perror("unmount2(overlay)")` to
`perror("umount2(overlay)")` in `teardown_overlay()`.

**Fix Applied:** 2026-03-30

---

### Error #10: Missing `<stdlib.h>` in test_overlay.c

**Date Found:** 2026-03-30

**Problem:** `test_overlay.c` calls `calloc()` and `free()` but did not include
`<stdlib.h>`. GCC emitted implicit declaration warnings for both functions.

**Symptom:**
```
tests/test_overlay.c:16:18: warning: implicit declaration of function 'calloc'
tests/test_overlay.c:48:5: warning: implicit declaration of function 'free'
```

**Root Cause:** The include list was missing `<stdlib.h>`. The functions still
linked because GCC's built-in declarations provided a fallback, but the
implicit declaration means the compiler assumed `int` return type instead of
`void *` for `calloc` — which can cause pointer truncation on 64-bit systems
if the compiler doesn't recognize the builtin.

**Impact:**
- **Severity:** Medium — compiles and links but with incorrect type assumptions
  that could cause runtime failures on some platforms
- **Symptom:** Warnings during build; potential pointer truncation on LP64

**Fix:** Added `#include <stdlib.h>` to `test_overlay.c` and to the
corresponding code listing in the Phase 3 implementation guide.

**Fix Applied:** 2026-03-30

---

### Error #11: `close_inherited_fds()` Opens Wrong Path — `/proc/self/fs` vs `/proc/self/fd`

**Date Found:** 2026-03-30

**Problem:** `close_inherited_fds()` called `opendir("/proc/self/fs")` instead
of `opendir("/proc/self/fd")`. The path `/proc/self/fs` does not exist, so
`opendir()` always returned `NULL`, and the function always fell through to
the brute-force fallback — even after `/proc` was successfully mounted.

**Symptom:** Debug output always showed:
```
[child] /proc/self/fd not available, closing fds 3-1024
```
This appeared even after `[child] Mounted /proc`, which should have made
`/proc/self/fd` available. The fallback still closed the fds correctly, so
the bug had no security impact — but it defeated the efficient `/proc/self/fd`
iteration path and produced misleading debug output.

**Root Cause:** Typo — `fs` instead of `fd`. A single-character error that
was difficult to spot in review because `/proc/self/fs` looks plausible (the
kernel does have various `/proc/self/fs*` paths in some configurations).

**Impact:**
- **Severity:** Low — the fallback path still closed all fds correctly
- **Symptom:** Always hit the brute-force path (1021 `close()` syscalls
  instead of only the handful that are actually open); misleading debug output

**Fix:** Changed `opendir("/proc/self/fs")` to `opendir("/proc/self/fd")`.

**Fix Applied:** 2026-03-30

---

### Error #12: `sudo` Masks File Descriptor Leak in Testing

**Date Found:** 2026-03-30

**Problem:** Phase 2 testing showed no leaked fds in the container, which
made the lack of `close_inherited_fds()` appear safe. The reason: `sudo`
spawns a new process that does not inherit file descriptors from the calling
shell. Fds opened in the user's terminal (editor buffers, shell redirections,
multiplexer sockets) never reached the minicontainer parent process.

**Symptom:** `ls /proc/self/fd` inside the container only showed fds 0, 1, 2
— even without any fd cleanup code. This created a false sense of security.

**Root Cause:** `sudo` creates a fresh process with a minimal fd table
(stdin/stdout/stderr only). It does not forward arbitrary fds from the
calling shell. This is a property of `sudo`, not of the container runtime.

**Impact:**
- **Severity:** Medium — no immediate vulnerability in Phase 2 (the parent
  was too simple to open extra fds), but a latent issue as the runtime grows
- **Not a code bug:** This is a testing gap, not a code defect. The code was
  missing fd cleanup, and the test methodology (running via `sudo`) could not
  detect it.

**What `sudo` does NOT protect against:** Fds opened by the minicontainer
parent *itself* — overlay bookkeeping, config files, log files, or future
network sockets. Both CVE-2024-21626 and CVE-2016-9962 involved fds opened
by the container runtime's own code, not inherited from an external shell.

**Verification:** Added a debug test block in `overlay_exec()` that opens
three fds to host files (`/etc/hostname`, `/etc/os-release`, `/etc/passwd`)
before `clone()`. With `--debug`, the output confirms `close_inherited_fds()`
closes them in the child:
```
[parent] TEST: Opened simulated leaked fds: 3, 4, 5
[child] Closing inherited fd 3
[child] Closing inherited fd 4
[child] Closing inherited fd 5
```
This test block should be removed once verification is complete.

**Fix:** `close_inherited_fds()` added in Phase 3 (§3.5). Updated Phase 2
and Phase 3 documentation to explain the `sudo` masking behavior.

**Fix Applied:** 2026-03-30

---

### Error #13: Duplicate Program Name in Debug Output

**Date Found:** 2026-03-30

**Problem:** The `[parent] Executing:` debug line printed the program name
twice. For example: `[parent] Executing: /bin/sh /bin/sh -c env`.

**Root Cause:** The debug code printed `config->program` followed by all
elements of `config->argv`. Since `argv[0]` is also the program name, it
appeared twice.

**Fix:** Changed the format string from `printf("[parent] Executing: %s", config->program)`
to `printf("[parent] Executing:")`, letting the `argv` loop handle the full
command line including `argv[0]`.

**Fix Applied:** 2026-03-30

---

### Error #14: Duplicate Work Directory Removal and Wrong Teardown Labels

**Date Found:** 2026-03-30

**Problem:** `teardown_overlay()` had two issues:
1. The `work_path` block was duplicated — `remove_directory(ctx->work_path)`
   was called twice
2. All debug labels said "Removing working layer" for `work/`, `merged/`, and
   `container_base`, making it impossible to tell which step was executing

**Root Cause:** Copy-paste error when adding debug output to each cleanup step.

**Impact:**
- **Severity:** Low — the duplicate `remove_directory()` call on an
  already-removed directory is harmless (nftw returns error but teardown
  continues), and the wrong labels only affect debug readability

**Fix:** Removed the duplicate `work_path` block. Changed labels to
`Removing upper layer`, `Removing work dir`, `Removing merged dir`, and
`Removing container base`.

**Fix Applied:** 2026-03-30

---

### Error #15: `-Wformat-truncation` Warnings in `init_overlay_paths()`

**Date Found:** 2026-04-13

**Problem:** GCC emitted four `-Wformat-truncation` warnings for the `snprintf`
calls in `init_overlay_paths()`. The warnings indicated that the formatted
output could exceed the `PATH_MAX` (4096) destination buffer when the combined
container directory path and container ID (or subdirectory suffix) approached
the limit.

**Symptom:**
```
src/overlay.c:110:49: warning: '%s' directive output may be truncated
  writing up to 12 bytes into a region of size between 0 and 4095
src/overlay.c:113:44: warning: '/upper' directive output may be truncated
  writing 6 bytes into a region of size between 1 and 4096
src/overlay.c:114:43: warning: '/work' directive output may be truncated
  writing 5 bytes into a region of size between 1 and 4096
src/overlay.c:115:45: warning: '/merged' directive output may be truncated
  writing 7 bytes into a region of size between 1 and 4096
```

**Root Cause:** The four `snprintf` calls wrote formatted paths into
`PATH_MAX`-sized buffers but never checked the return value. `snprintf` returns
the number of characters that *would have been written* if the buffer were large
enough — when this value is ≥ `PATH_MAX`, the output is silently truncated.
GCC's `-Wformat-truncation` (enabled by `-Wall`) flagged this because it could
statically prove truncation was possible.

**Impact:**
- **Severity:** Medium — while truncation is unlikely with typical path lengths,
  a silently truncated path passed to `mkdir` or `mount` would operate on the
  wrong directory, potentially outside the intended container tree. In a
  container runtime, this is a correctness and safety concern.

**Fix:** Wrapped each `snprintf` call in a return-value check:
```c
if (snprintf(ctx->container_base, PATH_MAX, "%s/%s",
             abs_container_dir, ctx->container_id) >= PATH_MAX) {
    fprintf(stderr, "init_overlay_paths: container_base path truncated\n");
    return -1;
}
```
The same pattern was applied to `upper_path`, `work_path`, and `merged_path`.
This converts silent truncation into a hard failure with a descriptive error
message. Both code listings in the Phase 3 implementation guide (§7 and §8.2.1)
were updated to match.

**Fix Applied:** 2026-04-13

---

### Error #16: Missing `<stdlib.h>` and Wrong Pointer Type in test_uts.c

**Date Found:** 2026-04-13

**Problem:** `test_uts.c` had two issues:
1. Missing `#include <stdlib.h>` — `calloc()` and `free()` were implicitly
   declared, causing `-Wimplicit-function-declaration` and
   `-Wbuiltin-declaration-mismatch` warnings
2. `test_env` was declared as `char *` instead of `char **` —
   `build_container_env()` returns `char **`, causing
   `-Wincompatible-pointer-types` warnings when assigned and when passed to
   `.envp` (which expects `char * const *`)

**Symptom:**
```
tests/test_uts.c:15:18: warning: implicit declaration of function 'calloc'
tests/test_uts.c:24:22: warning: initialization of 'char *' from incompatible
  pointer type 'char **'
tests/test_uts.c:46:5: warning: implicit declaration of function 'free'
```

**Root Cause:** Same class of error as Error #10 (missing `<stdlib.h>` in
`test_overlay.c`). The pointer type mismatch was a separate typo — `char *`
instead of `char **` — which compiled only because GCC's implicit conversion
rules allowed it with a warning.

**Impact:**
- **Severity:** Medium — implicit `calloc` declaration assumes `int` return
  type, which truncates the pointer on LP64 platforms. The wrong pointer type
  for `envp` could cause the child process to receive a corrupted environment.

**Fix:** Added `#include <stdlib.h>` and changed `char *test_env` to
`char **test_env` in both test functions. The corresponding code listing in
the Phase 4 implementation guide was updated to match.

**Fix Applied:** 2026-04-13

---

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

### 2. No File Descriptor Management (Security — CVE-2024-21626, CVE-2016-9962)

**Limitation (Phase 2):** Child inherits all open file descriptors from parent.

**Impact:**
- After `clone()`, the child has copies of every parent fd. Any fd referencing the host filesystem survives `pivot_root()` + `umount2("/old_root", MNT_DETACH)` because lazy unmount detaches the *mount*, not the *file descriptions*.
- A container process that discovers a leaked fd can read/write host files outside the mount namespace — this is a container escape.
- This is the exact mechanism exploited by **CVE-2024-21626 (Leaky Vessels)**: a leaked fd during runc's `clone()`-to-`execve()` window allowed full container escape. **CVE-2016-9962** is the same root cause.

**Note:** This vulnerability appeared safe in Phase 2 testing because `sudo`
spawns a new process that does not inherit the calling shell's fds. The Phase 2
parent was also simple enough to not open any extra fds itself. This was
accidental protection — see Error #12 for full analysis.

**Phase 3 fix:** `close_inherited_fds()` iterates `/proc/self/fd` after `mount_proc()` and closes every fd above `STDERR_FILENO` before `execve()`. This handles both inherited fds and fds opened by the parent itself (overlay bookkeeping, config files, etc.). See Phase 3 guide §3.5 and Error #11 for a typo that initially prevented the `/proc/self/fd` path from working.

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

### 6. Environment Variable Leak After pivot_root (Security)

**Limitation:** After `pivot_root()` into the container rootfs, the child process still inherits the host's full `environ`. Variables like `PATH=/home/tcrumb/.local/bin:/usr/local/bin:/usr/bin`, `HOME=/home/tcrumb`, and `SHELL=/bin/bash` reference paths that do not exist inside the container rootfs.

**Impact:**
- Programs that read `HOME` to create `~/.config/` would write to a nonexistent path
- Programs that read `SHELL` to spawn a subshell would fail if `/bin/bash` isn't in the rootfs
- Basic commands work by accident because `/usr/bin` and `/bin` happen to appear in the host's `PATH`, and the rootfs has binaries there — but this is fundamentally broken
- The `--env KEY=VALUE` flag from Phase 0 was dropped in Phase 2's `main.c` (no `case 'e':` in the getopt switch)

**Phase 3 fix:** `build_container_env()` constructs a minimal container-appropriate environment (`PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`, `HOME=/root`, `TERM=xterm`). The `--env` flag is restored with proper key-replacement semantics. See Phase 3 guide §3.4.

---

### 7. No Copy-on-Write Filesystem

**Limitation:** Container writes modify the base rootfs directory directly on the host. There is no isolation between container runs.

**Impact:**
- Running `rm /bin/ls` in one container permanently deletes it for all future containers
- No way to share a single base image across multiple concurrent containers
- No clean-slate guarantee between container runs

**Phase 3 fix:** OverlayFS layers a writable `upperdir` on top of a read-only `lowerdir` (the rootfs). The container sees a merged view; writes go to the upper layer only. On container exit, the upper layer is discarded and the base image is untouched. See Phase 3 guide §1.3.

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

### Phase 3: OverlayFS & Security Corrections

**Changes needed:**
- Create new `overlay.c`/`overlay.h` module superseding `mount.c`
- Mount OverlayFS over rootfs: read-only base image (lowerdir) + writable upper layer (upperdir)
- Generate unique container ID for per-container overlay directories
- Create `setup_overlay()` and `teardown_overlay()` for overlay lifecycle
- `overlay_exec()` replaces `mount_exec()` as the top-level execution function
- Add `--overlay` and `--container-dir` CLI flags
- Automatic cleanup of writable layer on container exit
- Backward compatible: without `--overlay`, behavior is identical to Phase 2

**Security corrections (addressing Known Limitations #2, #6, #7):**
- **Environment isolation (KL #6):** `build_container_env()` constructs a minimal container environment (`PATH`, `HOME`, `TERM`). Restores `--env KEY=VALUE` with key-replacement semantics. Fixes the host `environ` leak that has existed since Phase 0.
- **File descriptor audit (KL #2):** `close_inherited_fds()` closes all fds above stderr before `execve()`. Fixes the fd leak vulnerability (CVE-2024-21626, CVE-2016-9962) that has existed since Phase 0.
- **Mount hardening:** Overlay mounted with `MS_NODEV | MS_NOSUID` to block device node access and SUID escalation inside the container.

---

### Phase 4: UTS & User Namespace

**Changes needed:**
- Add `CLONE_NEWUTS` to clone flags for hostname isolation
- Add `CLONE_NEWUSER` for UID/GID mapping
- Add `hostname` and `uid_map`/`gid_map` fields to config
- Enable rootless (unprivileged) container creation

---

### Phase 5: Resource Limits (cgroups)

**Changes needed:**
- Add cgroup configuration to config
- Create/configure cgroup before clone
- Move child into cgroup
- Cleanup cgroup on exit

---

### Phase 6: Network Isolation

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
