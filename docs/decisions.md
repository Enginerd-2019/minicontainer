# Design Decisions and Error Log

**Project:** minicontainer - Minimal Container Runtime
**Phase:** 6 - Network Namespace (veth pair + optional NAT)
**Last Updated:** 2026-05-13

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

**Fix Applied:** 2026-05-11

---

### 22. Superseding cgroup.c with net.c (Phase 6)

**Decision:** Phase 6 follows the same supersede pattern established in
Decisions #7, #9, #13, #16, #21: create a new `net.c`/`net.h` module that
becomes the top-level `*_exec()` entry point, with `cgroup.c` (Phase 5)
retained and imported for its lifecycle helpers (`setup_cgroup`,
`add_pid_to_cgroup`, `remove_cgroup`).

**Rationale:**
- **Reader continuity:** A reader who learned `cgroup.c`'s structure should
  recognize `net.c` as "Phase 5 plus network." The duplication (its own
  `child_args_t`, `child_func()`, `close_inherited_fds()`, ~90% of
  `*_exec()` boilerplate) is deliberate and matches Decisions #7/#9/#13/#16.
- **Phase 5's `cgroup_exec()` stays valid.** `test_cgroup` and any future
  consumer of the Phase 5 API still compile and link unchanged.
- **No premature consolidation.** The repeated boilerplate is the explicit
  motivation for Phase 7's planned execution-core refactor (`core.c`).
  Phase 6 does NOT attempt that consolidation — adding a refactor on top
  of a new feature in the same phase would obscure both.

**Trade-offs:**
- Five copies of `child_func()` now exist (`namespace.c`, `mount.c`,
  `overlay.c`, `uts.c`, `cgroup.c`, `net.c`). Phase 7a will collapse them
  to one. Until then, the symmetry is the documentation.

**Files affected:**
- `src/net.c`, `include/net.h` — new
- `src/main.c` — top-level config/result/exec/cleanup switched from `cgroup_*`
  to `net_*`
- `tests/test_net.c` — new

**Fix Applied:** 2026-05-12

---

### 23. fork+exec ip(8) Instead of Raw Netlink (Phase 6)

**Decision:** Drive veth pair creation, IP assignment, route install, and
netns move via `fork+exec` of the `ip` binary (and `iptables` for NAT)
rather than emitting netlink messages directly with `AF_NETLINK` /
`NETLINK_ROUTE` sockets.

**Rationale:**
- **Pedagogy over performance.** Each `ip` invocation maps to a single,
  greppable line that a learner can run from a shell. A learner can stop
  minicontainer mid-setup and re-run any step manually — netlink has no
  such inspect-and-replay surface.
- **Correctness via the canonical tool.** `iproute2` already encodes the
  kernel's preferred syscall sequence (RTM_NEWLINK with IFLA_LINKINFO
  attributes, RTM_SETLINK with IFLA_NET_NS_PID, RTM_NEWADDR with
  IFA_LOCAL/IFA_ADDRESS, etc.). Re-implementing it is non-trivial and
  every bug is a CVE.
- **`fork+exec` is the cost we already pay.** `clone()` is the project's
  central abstraction; an additional 5–6 `fork+execve` pairs during setup
  is rounding error relative to the work each container does.

**Trade-offs:**
- **Latency:** ~50ms per `ip` invocation × ~5 calls = ~250ms of setup
  overhead per container. Acceptable for educational use; would matter for
  a production runtime spawning thousands of containers/sec.
- **Dependency:** the host needs `iproute2` installed, and the rootfs needs
  `/bin/ip` (see §0 Prerequisites and Decision #24).
- **Error handling:** parsing `ip`'s exit status is coarse-grained
  compared to netlink's per-message ACK/NACK semantics. We accept this.

**Files affected:**
- `src/net.c` — `run_ip_command()`, `run_iptables_command()` helpers wrap
  `fork+execve`

**Fix Applied:** 2026-05-12

---

### 24. Three-Path `find_ip_binary()` Search (Phase 6)

**Decision:** `find_ip_binary()` searches for the `ip` binary in three
locations, in order: `/sbin/ip` → `/usr/sbin/ip` → `/bin/ip`. The same
function is called from BOTH sides of `clone()`: parent-side BEFORE
`clone()` (against the host filesystem) and child-side AFTER `pivot_root`
(against the rootfs).

**Rationale:**
- **Two filesystems, one helper.** Pre-clone, the host is the filesystem
  in scope — and on Ubuntu/Debian/RHEL, `ip` lives at `/sbin/ip` or
  `/usr/sbin/ip`. Post-`pivot_root`, the rootfs is the filesystem in
  scope — and `scripts/build_rootfs.sh` puts every entry in `BINS` at
  `/bin/<name>`, so the container's `ip` is at `/bin/ip`.
- **One helper, not two.** Having a single function with a multi-path
  search keeps the parent and child code identical at the call site
  (`const char *ip = find_ip_binary(); if (!ip) return -1;`) and means
  there's no risk of the parent and child accidentally diverging on
  which paths they consider valid.

**Trade-offs:**
- **Implicit ordering:** if both `/sbin/ip` and `/bin/ip` exist
  simultaneously, the host's wins. This is the right call (the host's
  `iproute2` is what was tested against), but worth noting.

**Files affected:**
- `src/net.c` — `find_ip_binary()` (static)
- `scripts/build_rootfs.sh` — `BINS` array includes `ip`

**Related:** This was caught and corrected in Error #17. The initial draft
of `find_ip_binary()` only checked `/sbin/ip` and `/usr/sbin/ip`, which
broke the child-side lookup inside the rootfs.

**Fix Applied:** 2026-05-12

---

### 25. `generate_veth_names()` Runs BEFORE `clone()` (Phase 6)

**Decision:** Veth interface names (`veth_h_<6 hex>` for the host end,
`veth_c_<6 hex>` for the container end) are generated by the parent
BEFORE calling `clone()`, written into `child_args.net_ctx`, and read
unchanged by the child after the sync pipe unblocks.

**Rationale (the bug this prevents):**

`clone()` without `CLONE_VM` gives the child a COPY of the parent's
address space, not a shared view. Anything the parent writes to
`child_args` (or any other pointer-reachable object) AFTER `clone()`
returns is INVISIBLE to the child. A natural-looking refactor —
"generate the names inside `setup_net()`, which the parent calls after
`clone()`" — would silently break: the parent would configure veths
named `veth_h_abc123` while the child would see whatever `child_args`
contained at clone-time (uninitialized memory, or a stale value).

By generating the names BEFORE `clone()`, both copies of `child_args`
see the same names; the parent's later mutation of `ctx->veth_created`,
`ctx->nat_added`, etc. doesn't matter because the child never reads
those fields — it only reads `ctx->veth_host` and `ctx->veth_container`,
both populated pre-clone.

**Trade-offs:**
- **Slightly less locality:** the name generation is split from the rest
  of the setup. The header comment on `generate_veth_names()` explicitly
  flags this. `setup_net()` also defensively re-validates that names are
  populated.

**Files affected:**
- `include/net.h` — `generate_veth_names()` declaration with address-space note
- `src/net.c` — call site is `net_exec()` BEFORE `clone()`

**Fix Applied:** 2026-05-12

---

### 26. Sync Pipe Widened to `enable_user_namespace OR enable_network` (Phase 6)

**Decision:** The sync pipe introduced in Phase 4b (Decision #18 / #20) for
UID/GID-mapping coordination is now created whenever the user namespace
OR the network namespace is enabled. The child blocks on the pipe in
both cases; the parent writes to it after both UID/GID maps (if any) AND
the veth move (if any) are complete.

**Rationale:**
- **The veth move has the same ordering constraint as UID/GID maps.** The
  parent must call `ip link set veth_c_<id> netns <pid>` AFTER `clone()`
  (it needs the child's PID) but BEFORE the child tries to use the
  network. Reusing the existing sync pipe is mechanically identical and
  avoids inventing a second synchronization channel.
- **Single point of "child unblocks."** Whether the parent gates on
  UID/GID maps, veth move, both, or neither, the child's wait pattern
  is the same: if `sync_fd >= 0`, read one byte and proceed. This keeps
  `child_func()` simple.

**Trade-offs:**
- **One byte of cross-namespace IPC for the common case.** When `--net`
  is set without `--user`, the cost of the sync pipe is one `pipe()`
  syscall plus one `read()` plus one `write()`. Negligible.
- **The condition is "OR", not "AND".** Either feature individually
  needs the sync; both together still need exactly one.

**Files affected:**
- `src/net.c` — `net_exec()`: `bool need_sync = config->enable_user_namespace || config->enable_network;`
- `src/net.c` — `child_func()`: existing "if `sync_fd >= 0` then read" logic
  unchanged

**Fix Applied:** 2026-05-12

---

### 27. `cleanup_net()` Stores `nat_source_cidr` in the Context (Phase 6)

**Decision:** When `setup_net()` appends the `iptables -t nat -A
POSTROUTING -s <CIDR> -j MASQUERADE` rule, it stores the exact CIDR string
it used into `ctx->nat_source_cidr`. `cleanup_net()` reads this back and
issues the matching `-D` (delete) command. Cleanup never re-references the
original `veth_config_t`.

**Rationale:**
- **Self-contained teardown.** The Phase 5 cleanup contract is "given the
  result struct, you can clean up." `cleanup_net()` honoring that contract
  means callers (and Phase 7a's planned consolidation) can free the
  config immediately after `*_exec()` returns without keeping it alive
  for cleanup.
- **Avoids "construct the CIDR twice" drift.** If setup formats the CIDR
  as `"10.0.0.0/24"` and cleanup re-derives it from `host_ip + netmask`,
  any change to the formatting (e.g., normalizing to network address vs.
  host address) silently breaks cleanup, leaving a stale iptables rule.
  Storing the exact string ensures append and delete always match.

**Trade-offs:**
- **Larger `net_context_t`:** adds `char nat_source_cidr[INET_ADDRSTRLEN + 8]`.
  ~22 bytes; trivial.
- **The CIDR is also a soft secret of cleanup state.** If `ctx` is
  corrupted between setup and cleanup, the rule leaks. This is no worse
  than any other cleanup-via-context pattern in the codebase.

**Files affected:**
- `include/net.h` — `nat_source_cidr` field
- `src/net.c` — `setup_net()` writes it, `cleanup_net()` reads it

**Fix Applied:** 2026-05-12

---

### 28. `--net` Sub-Flag Validation (Hard Error, Not Silent Ignore) (Phase 6)

**Decision:** `--net-host-ip`, `--net-container-ip`, `--net-netmask`, and
`--no-nat` all require `--net`. Passing any of them without `--net` is a
hard error in `main.c` (prints a diagnostic, exits 1) — not a silent
no-op.

**Rationale:**
- **Surfaces typos early.** A user who writes `--net-host-ip 10.42.0.1`
  without `--net` either forgot `--net` or has a typo. In either case,
  silently ignoring the IP and running without network isolation
  produces a subtle, hard-to-debug failure (the container has no
  network, but the user thought they configured one).
- **Same precedent as Error #7's misplaced-flag detection.** That
  error introduced `known_flags[]` to convert silent failure into a loud
  warning. This is the same philosophy applied to sub-flag dependencies.

**Trade-offs:**
- **Slightly more rigid CLI.** A user who passes the sub-flags habitually
  has to remember `--net`. Acceptable — the alternative is silent data
  loss.

**Files affected:**
- `src/main.c` — validation block after `getopt_long` loop:
  ```c
  if ((net_host_ip || net_container_ip || net_netmask || no_nat)
      && !enable_network) {
      fprintf(stderr, "Error: --net-host-ip / --net-container-ip / "
                      "--net-netmask / --no-nat require --net\n");
      return 1;
  }
  ```

**Fix Applied:** 2026-05-12

---

### 29. `build_rootfs.sh` Adds curl, CA Bundle, NSS Modules, and resolv.conf (Phase 6)

**Decision:** Following a user-requested verification capability —
running `curl` against external HTTPS endpoints from inside the
container's netns to confirm end-to-end Phase 6 networking —
`scripts/build_rootfs.sh` is extended to copy four additional pieces
into the rootfs so that HTTPS client tools function correctly:

1. `curl` appended to the `BINS` array. The existing `ldd` resolver
   discovers and copies `libcurl`, `libssl`, `libcrypto`,
   `libnghttp2`, and the rest of curl's linked dependencies into
   `rootfs/lib/` automatically.
2. The host's CA certificate bundle copied to
   `rootfs/etc/ssl/certs/ca-certificates.crt`. The script tries
   `/etc/ssl/certs/ca-certificates.crt` (Debian/Ubuntu) first, then
   `/etc/pki/tls/certs/ca-bundle.crt` (RHEL/Fedora); the first
   readable path wins.
3. The glibc NSS modules `libnss_dns.so.2`, `libnss_files.so.2`, and
   `libresolv.so.2` copied to `rootfs/lib/` from
   `/lib/x86_64-linux-gnu/` (Debian/Ubuntu) or `/usr/lib64/`
   (RHEL/Fedora).
4. `/etc/resolv.conf` copied from the host, with a fall-back that
   writes `nameserver 1.1.1.1` + `nameserver 8.8.8.8` whenever the
   host's `/etc/resolv.conf` references `127.0.0.53` (the
   systemd-resolved stub, which is unreachable from a container
   netns).

**Rationale:**

- **Curl alone is not enough.** Curl in the rootfs is dynamically
  linked to libcurl/libssl/libcrypto, all of which `ldd` discovers and
  the existing copy loop handles. But three additional dependencies
  are NOT picked up by `ldd` and have to be addressed separately:
  CA certificates (a data file, not a library), NSS modules
  (`dlopen`'d by glibc at runtime, not linked), and `/etc/resolv.conf`
  (a config file).
- **NSS modules are the non-obvious one.** Without
  `libnss_dns.so.2`/`libnss_files.so.2` in the rootfs,
  `getaddrinfo()` returns `EAI_AGAIN`/`EAI_NONAME` and curl reports
  `Could not resolve host` even when `/etc/resolv.conf` is valid. The
  failure mode is silent at link time because the modules are loaded
  by name at runtime.
- **The systemd-resolved fall-back is essential.** On modern
  Ubuntu/Debian desktops, `/etc/resolv.conf` is a symlink to a stub
  containing `nameserver 127.0.0.53` — that loopback address is only
  reachable on the host's `lo` interface, never from a container
  netns. Copying it verbatim makes DNS appear to be configured while
  actually being broken. The fall-back detects the `127.0.0.53`
  pattern and substitutes public resolvers.
- **CA-bundle path matches curl's compile-time default on
  Debian/Ubuntu.** Curl is built with
  `--with-ca-bundle=/etc/ssl/certs/ca-certificates.crt`, so placing
  the bundle at exactly that path requires no `--cacert` or
  `CURL_CA_BUNDLE` configuration inside the container.

**Trade-offs:**

- **Rootfs grows by ~5 MB.** Curl + the linked TLS libraries +
  certificates + NSS modules. Acceptable — the goal is a runtime that
  can usefully execute network programs, not the smallest possible
  image.
- **NSS-module path hardcoding** (`/lib/x86_64-linux-gnu/` and
  `/usr/lib64/`) is Linux-distro-specific. The script tolerates
  missing paths gracefully via `[ -r "$so" ] && cp ...`, so it
  degrades to "curl works by IP but not by hostname" on unsupported
  layouts.
- **No `/dev/null` in the rootfs.** Curl's `-o /dev/null` therefore
  fails with `curl: (23) Failure writing output to destination` even
  though the HTTP exchange succeeds. Sample commands write to
  `/tmp/curl_out` instead (the rootfs has `/tmp`). Adding `/dev/null`
  would require `mknod c 1 3`, which needs root — the build script
  intentionally does not require sudo. See also Known Limitation #5.

**Operational prerequisite:** Curl traffic from inside the container
also requires the host's iptables `FORWARD` chain to permit packets
to and from `10.0.0.0/24`. The MASQUERADE rule that `net.c` installs
operates on `nat/POSTROUTING` only; `filter/FORWARD` is host policy
that minicontainer deliberately does not modify. See Known Limitation
#8 for the symptoms, the diagnostic checklist, and the two iptables
commands required.

**Files affected:**

- `scripts/build_rootfs.sh`:
  - `BINS` array — `curl` appended.
  - `mkdir -p` extended to include `etc/ssl/certs`.
  - New copy loop for `libnss_dns.so.2` / `libnss_files.so.2` /
    `libresolv.so.2` (paths tried for Debian/Ubuntu then RHEL/Fedora).
  - New CA-bundle-path search loop, first-readable wins.
  - New systemd-resolved-aware `/etc/resolv.conf` step (host copy with
    fall-back).

**Fix Applied:** 2026-05-13

---

### 30. Execution-Core Consolidation: Five `*_exec()` Functions Become One (Phase 7a)

**Decision:** Replace the five per-phase orchestrators (`mount_exec`,
`overlay_exec`, `uts_exec`, `cgroup_exec`, `net_exec` — plus the
already-deleted `spawn_process` / `namespace_exec`) with a single
`container_exec(const container_config_t *)` in a new `core.c`
module. The new function takes one unified `container_config_t`
covering every flag and parameter; behavior in each phase is recovered
by setting the corresponding flag.

The five duplicate `child_func` definitions, five duplicate
`close_inherited_fds` definitions, and five file-local `child_args_t`
struct definitions collapse to one canonical copy each, also in
`core.c`. `build_container_env()` (decisions.md #21 prepared this
specifically) moves out of `main.c` into its own `env.c` / `env.h`
module so tests and future subcommands can use it without dragging in
the CLI layer.

**Background — what was there:**

By Phase 6 the superseding module pattern (decisions.md #7, #9, #13,
#16, #21, #22) had produced five `.c` files that each contained:

1. A file-local `child_args_t` typedef carrying the same fields
   (`program`, `argv`, `envp`, `enable_debug`, `rootfs_path`,
   `hostname`, `sync_fd`, `user_namespace_active`, plus phase-specific
   additions).
2. A `static close_inherited_fds(bool enable_debug)` — the
   CVE-2024-21626 / CVE-2016-9962 mitigation, defined `static` in
   `uts.c` first, then copy-pasted into `cgroup.c`, `net.c`, and
   `overlay.c` because cross-module `static` symbols aren't reachable.
3. A `static child_func(void *arg)` — the child-side lifecycle (sync
   wait → setup_uts → setup_rootfs → mount_proc → close_inherited_fds
   → execve), extended in each phase with one new step
   (configure_container_net at Phase 6, etc.).
4. A `*_exec(const *_config_t *)` parent-side orchestrator with
   ~15 steps: cgroup-setup, sync-pipe, overlay-setup, stack-alloc,
   veth-name generation (Phase 6+), `clone()`, sync-pipe write,
   `setup_user_namespace_mapping()`, `add_pid_to_cgroup()`,
   `waitpid()`, exit-status parse, overlay teardown, error-path
   cleanup.
5. A `*_cleanup()` doing the tail half — `free(stack_ptr)` plus
   whatever cleanup the phase introduced (`remove_cgroup`,
   `cleanup_net`).

Phase 6's `net.c` reached ~900 lines, of which ~700 were a near-exact
copy of `cgroup.c`'s plumbing. Bug fixes had to propagate through up to
five copies. Phase 5's first cut dropped 8+ carry-forward invariants
from its copy and required a retro audit; Phase 6 dropped its own
subset before catching them.

**The Phase 7a refactor:**

A unified `container_config_t` subsumes every prior `*_config_t`:

```c
typedef struct {
    /* Process */
    const char *program;
    char *const *argv;
    char *const *envp;
    bool enable_debug;

    /* Namespace flags (Phases 1/2/4/4b/4c/6) */
    bool enable_pid_namespace;
    bool enable_mount_namespace;
    bool enable_uts_namespace;
    bool enable_user_namespace;
    bool enable_ipc_namespace;
    bool enable_network;

    /* Filesystem (Phases 2/3) */
    const char *rootfs_path;
    bool enable_overlay;
    const char *container_dir;

    /* UTS (Phase 4) */
    const char *hostname;

    /* User namespace mapping (Phase 4b — flat, packed into
     * user_ns_mapping_t inside core.c before the helper call) */
    uid_t uid_map_inside;
    uid_t uid_map_outside;
    size_t uid_map_range;
    gid_t gid_map_inside;
    gid_t gid_map_outside;
    size_t gid_map_range;

    /* Cgroup limits (Phase 5) */
    cgroup_limits_t cgroup_limits;
    bool enable_cgroup;

    /* Network (Phase 6) */
    veth_config_t veth;
} container_config_t;
```

A `container_context_t` aggregates the runtime state each helper
produces — `overlay_ctx`, `cgroup_ctx`, `net_ctx`, `stack_ptr` — so
`container_cleanup()` walks all of it in one call. A
`container_result_t` wraps the exit status plus the context.

The unified `container_exec()` is a single 250-line function that
covers every flag combination — exactly the lifecycle Phase 6's
`net_exec` had, with the difference that conditionals on
`config->enable_*` now gate the optional steps instead of presence
in the per-phase `*_config_t`.

The unified `child_func()` is the canonical child-side lifecycle:
sync-wait → `setup_uts` → `setup_rootfs` (calls `mount_proc`
internally) → `configure_container_net` → `close_inherited_fds` →
`execve`. One static `close_inherited_fds()` next to it, called from
the canonical step 6 — no more copy-paste.

**Rationale:**

- **Bug fixes propagate once.** The ~90% boilerplate that was
  identical across five phases is now identical because there's only
  one copy. The kind of carry-forward drift that bit Phase 5 and
  Phase 6 cannot recur — there is nothing to forward.
- **The helpers stay where they belong.** `setup_uts`,
  `setup_rootfs`, `mount_proc`, `setup_overlay`, `teardown_overlay`,
  `setup_cgroup`, `add_pid_to_cgroup`, `remove_cgroup`, `setup_net`,
  `configure_container_net`, `cleanup_net`, `generate_veth_names`,
  `find_ip_binary` — all remain in their respective `.c` files and
  are called from `core.c`'s parent or child half. Phase 7a is not a
  "rewrite as one giant file" refactor; it removes the *orchestration
  duplication* without touching the actual primitive implementations.
- **Test coverage is preserved by design.** Every Phase 2-6 test now
  calls `container_exec()` with the equivalent flag set. `test_core.c`
  adds two regressions (bare exec, PID-only) covering what the
  deleted `test_spawn.c` / `test_namespace.c` tested. The success
  criterion is "every Phase 0-6 test passes behaviorally identical."
- **`build_container_env()` extraction enables Phase 7b/8 reuse.** A
  future `cli.c` subcommand dispatcher (Phase 7b) and bundle loader
  (Phase 8c) both need a clean container environment without dragging
  in `main.c`. Putting it in `env.c` makes the future extraction
  mechanical.

**Trade-offs:**

- **`core.c` is harder to read in one pass than any single `*_exec()`
  was.** A reader has to hold the full lifecycle in their head — 15
  parent-side steps, 7 child-side steps, every flag combination —
  rather than tracing through (say) `mount_exec` to learn just Phase
  2's slice. Mitigation: comments inside `container_exec` mark each
  step with the phase that introduced it.
- **Reading order is reversed.** Pre-7a, a learner traced Phase 0 →
  Phase 1 → Phase 2 chronologically through ever-larger `*_exec`
  functions. Post-7a, they read `container_exec` once and then learn
  what each helper (`setup_uts`, `setup_overlay`, etc.) does in
  isolation. The chronological story now lives in the phase guides
  and this decisions.md, not in the source layout.
- **Two `child_args_t` snapshot semantics still matter.**
  `clone(2)` without `CLONE_VM` copies the parent's address space at
  clone-time, so any field the parent writes after `clone()` is
  invisible to the child. `core.c`'s `child_args_t` is populated
  before clone and never touched again — same invariant Phase 6
  established (decisions.md #25), just with one consolidated struct.

**What the principle illustrates:**

Pedagogical duplication has a half-life. Phases 0-6 used the
superseding module pattern (decisions.md #7, #9, #13, #16) because
**learning** the same idea twice cements it. By Phase 6 the readers
who would learn from the duplication already had, and the readers who
came in fresh to Phase 6 had to wade through five identical
prologues before seeing one line of network code. The optimal time to
consolidate is when the cost of repetition exceeds the teaching
benefit — which the Phase 5 first-cut audit and the Phase 6
carry-forward checklist together demonstrated had arrived.

**Files affected:**

- `src/core.c` — NEW, 438 lines: `container_exec`, `container_cleanup`,
  one canonical `child_func`, one canonical `close_inherited_fds`,
  the file-local `child_args_t`.
- `include/core.h` — NEW: `container_config_t`, `container_context_t`,
  `container_result_t`, public signatures.
- `src/env.c` — NEW, 67 lines: `build_container_env` moved from
  `main.c` verbatim (decisions.md #21 already made it self-contained).
- `include/env.h` — NEW: `build_container_env` declaration +
  `MAX_ENV_ENTRIES`.
- `src/mount.c` — SLIMMED: dropped `child_args_t`, `static
  child_func`, `mount_exec`, `mount_cleanup`. Kept `setup_rootfs`,
  `mount_proc`. Includes pruned to what's actually used.
- `src/overlay.c` — SLIMMED: dropped `child_args_t`, `static
  close_inherited_fds`, `static child_func`, `overlay_exec`,
  `overlay_cleanup`, the §3.5 leaked-fd debug-test block. Kept
  `setup_overlay`, `teardown_overlay`, the static path/dir helpers
  they use.
- `src/uts.c` — SLIMMED: dropped `child_args_t`, `static
  close_inherited_fds`, `static child_func`, `uts_exec`, `uts_cleanup`.
  Kept `setup_uts`, `setup_user_namespace_mapping` (signature change
  per Decision #31).
- `src/cgroup.c` — SLIMMED: dropped `child_args_t`, `static
  close_inherited_fds`, `static child_func`, `cgroup_exec`,
  `cgroup_cleanup`, and the synthetic `uts_config_t uts_cfg = {...}`
  workaround (Phase 5 §3.7). Kept `setup_cgroup`,
  `add_pid_to_cgroup`, `remove_cgroup`, `generate_cgroup_name`.
- `src/net.c` — SLIMMED: dropped `child_args_t`, `static
  close_inherited_fds`, `static child_func`, `net_exec`,
  `net_cleanup`. Kept `setup_net`, `configure_container_net`,
  `cleanup_net`, `generate_veth_names`, the `run_ip_command*` /
  `run_iptables_command` static helpers. `find_ip_binary` promoted to
  public (Decision #32).
- `include/mount.h` / `overlay.h` / `uts.h` / `cgroup.h` / `net.h` —
  per-phase `*_config_t` / `*_result_t` / `*_exec` / `*_cleanup`
  declarations removed. `uts.h` gains `user_ns_mapping_t` (Decision
  #31). `net.h` gains `find_ip_binary` declaration (Decision #32).
- `src/spawn.c`, `src/namespace.c`, `tests/test_spawn.c`,
  `tests/test_namespace.c`, `include/spawn.h`, `include/namespace.h` —
  DELETED entirely. Subsumed by `container_exec` with appropriate
  flags; `test_core.c` covers the regression cases.
- `src/main.c` — `container_config_t` literal replaces the prior
  `net_config_t`; `container_exec()` + `container_cleanup()` replace
  `net_exec()` + `net_cleanup()`. The CLI parsing and validation
  logic is unchanged.
- `Makefile` — `HELPER_OBJS` variable factored out (Decision #33);
  `TEST_SPAWN` / `TEST_NAMESPACE` removed; `TEST_CORE` added; every
  test rule now links against `$(HELPER_OBJS)`.

**Verification:** `make clean && make` builds with zero warnings.
`sudo make test` runs all six test binaries (`test_core`,
`test_mount`, `test_overlay`, `test_uts`, `test_cgroup`, `test_net`)
plus the unprivileged subset of `test_uts`.

**Fix Applied:** 2026-05-13

---

### 31. `user_ns_mapping_t` Replaces the Synthetic `uts_config_t` Workaround (Phase 7a)

**Decision:** Introduce a five-field struct `user_ns_mapping_t` in
`uts.h` and change `setup_user_namespace_mapping()`'s signature from
`(pid_t child_pid, const uts_config_t *config)` to `(pid_t child_pid,
const user_ns_mapping_t *config)`. The new struct contains only the
fields the mapping helper actually reads.

**Background:**

Phase 4b introduced `setup_user_namespace_mapping()` as a parent-side
helper. Its signature took a `const uts_config_t *` because the only
caller at the time was `uts_exec`, which already had a `uts_config_t`
in hand. The function reads six fields out of that struct
(`uid_map_inside`, `uid_map_outside`, `uid_map_range`,
`gid_map_inside`, `gid_map_outside`, `gid_map_range`) plus
`enable_debug`. It does not touch the other ~15 fields.

Phase 5's `cgroup_exec` superseded `uts_exec` but still needed to
call `setup_user_namespace_mapping()`. The cleanest call site would
have changed the helper's signature to accept just the mapping data —
but at the time, doing so would have required editing `uts.h`,
`uts.c`, the test files, and every prior caller. The faster path was
to **construct a partial `uts_config_t` inline** and pass that:

```c
/* Phase 5 / 6 cgroup_exec — synthetic config workaround */
uts_config_t uts_cfg = {
    .uid_map_inside  = config->uid_map_inside,
    .uid_map_outside = config->uid_map_outside,
    .uid_map_range   = config->uid_map_range,
    .gid_map_inside  = config->gid_map_inside,
    .gid_map_outside = config->gid_map_outside,
    .gid_map_range   = config->gid_map_range,
    .enable_debug    = config->enable_debug,
    /* Other fields zero-init'd — never read. */
};
setup_user_namespace_mapping(pid, &uts_cfg);
```

That code shipped in Phase 5 and Phase 6 and worked correctly — the
zero-init'd fields were never read. But it was a structural lie: the
function's signature claimed it needed a `uts_config_t`, when in fact
it needed seven fields packaged in any container.

**The Phase 7a fix:**

A new struct dedicated to what the helper actually consumes:

```c
/* uts.h */
typedef struct {
    uid_t  uid_map_inside;
    uid_t  uid_map_outside;
    size_t uid_map_range;
    gid_t  gid_map_inside;
    gid_t  gid_map_outside;
    size_t gid_map_range;
    bool   enable_debug;
} user_ns_mapping_t;

int setup_user_namespace_mapping(pid_t child_pid,
                                 const user_ns_mapping_t *config);
```

`core.c`'s `container_exec()` packs the flat `uid_map_*` /
`gid_map_*` fields from `container_config_t` into a
`user_ns_mapping_t` immediately before the call:

```c
if (config->enable_user_namespace) {
    user_ns_mapping_t mapping = {
        .uid_map_inside  = config->uid_map_inside,
        .uid_map_outside = config->uid_map_outside,
        .uid_map_range   = config->uid_map_range,
        .gid_map_inside  = config->gid_map_inside,
        .gid_map_outside = config->gid_map_outside,
        .gid_map_range   = config->gid_map_range,
        .enable_debug    = config->enable_debug
    };
    if (setup_user_namespace_mapping(pid, &mapping) < 0) { ... }
}
```

**Rationale:**

- **Signature truthfulness.** The function now advertises exactly
  what it needs. A future caller reading the declaration can tell
  immediately that no other fields are consulted — no need to inspect
  the implementation.
- **The synthetic-workaround pattern doesn't propagate.** Phase 5
  added one synthetic config; Phase 6 inherited it and added nothing
  new because `cgroup_config_t` already had the mapping fields. But
  any further superseding phase would have inherited the same lie.
  Stopping it at Phase 7a (the refactor that consolidates everything)
  costs one small struct + one signature change.
- **The struct is reusable.** Phase 7b's `cmd_exec` subcommand,
  Phase 8a's inspector, and Phase 8c's OCI bundle parser will all
  need user-namespace mapping data; they can now take a
  `user_ns_mapping_t` directly without going near the (now-deleted)
  `uts_config_t`.

**Trade-offs:**

- **Eight call-site lines to construct the struct.** The verbose form
  in `container_exec()` is more visible than the prior synthetic
  workaround was. Acceptable — the visibility is the point; readers
  see exactly which fields flow into the helper.
- **`user_ns_mapping_t` lives in `uts.h`, not `core.h`.** A reader
  might expect a "container-wide" struct to live with
  `container_config_t`. Putting it in `uts.h` keeps it next to the
  helper that consumes it (the principle: types live next to their
  primary consumer, not next to their primary producer). The
  alternative would split the helper's interface across two headers.

**Files affected:**

- `include/uts.h` — `user_ns_mapping_t` added; `setup_user_namespace_mapping`
  signature changed to take `const user_ns_mapping_t *`.
- `src/uts.c` — function body unchanged; only the parameter type
  changes. The body reads the same six fields off the new struct.
- `src/core.c` — `container_exec()` constructs the
  `user_ns_mapping_t` immediately before the call.
- `src/cgroup.c` — the synthetic `uts_config_t uts_cfg = {...}`
  workaround block (Phase 5 §3.7) deleted along with the rest of the
  per-phase exec body (Decision #30).

**Fix Applied:** 2026-05-13

---

### 32. `find_ip_binary()` Promoted from `static` to Public (Phase 7a)

**Decision:** Drop the `static` qualifier on `find_ip_binary()`'s
definition in `src/net.c` and add a matching `extern` declaration to
`include/net.h`. The function is now callable from any module that
includes `net.h` — specifically `core.c`, which needs the parent-side
early-failure check (Phase 6 invariant: fail loudly before `clone()`
if no `ip` binary is reachable on the host) without duplicating the
three-path search logic.

**Background:**

Phase 6 introduced `find_ip_binary()` to resolve `/sbin/ip` →
`/usr/sbin/ip` → `/bin/ip` (Decision #24). The function was defined
`static` because Phase 6's only callers were both inside `net.c` —
`net_exec`'s parent-side pre-clone check and `run_ip_command`'s
helper.

Phase 7a's `container_exec()` (in `core.c`) carries forward the
parent-side early-failure check:

```c
if (config->enable_network && !find_ip_binary()) {
    fprintf(stderr, "[parent] --net requires /sbin/ip, /usr/sbin/ip, "
                    "or /bin/ip; install the `iproute2` package\n");
    result.child_pid = -1;
    return result;
}
```

That call lives in `core.c`. With `find_ip_binary()` still `static`
inside `net.c`, the linker rejects the cross-module reference.

**The fix is two halves:**

1. **`include/net.h`** — add the declaration:
   ```c
   const char *find_ip_binary(void);
   ```
2. **`src/net.c`** — change the definition from
   `static const char *find_ip_binary(void)` to
   `const char *find_ip_binary(void)`.

Both halves are required. Adding the declaration alone leaves the
function file-scope, and the linker still rejects the reference.
Removing the `static` alone gives external linkage but no declaration
that other translation units can see; the cross-module caller would
fall back to an implicit-function-declaration warning under `-Wall`,
and the project explicitly builds clean.

**Rationale:**

- **Single source of truth for the path search.** The three-path
  search (`/sbin/ip` → `/usr/sbin/ip` → `/bin/ip`, Decision #24,
  Error #17) is a non-trivial invariant — host paths first, rootfs
  path last. Duplicating that logic in `core.c` would create a
  second copy that has to track every future change. Promotion to
  public is the minimum change that lets `core.c` reuse the existing
  helper.
- **No new attack surface.** The function is read-only over the
  filesystem (`access(path, X_OK)`); promoting it to public exposes
  no state and grants no new capability to any caller.
- **The internal callers (`run_ip_command`, `run_ip_command_ignore`)
  inside `net.c` are unchanged.** They continue to call
  `find_ip_binary()` directly; the only difference is that the
  function is now also linkable from outside the translation unit.

**Trade-offs:**

- **Both halves must move together.** A common error mode is to add
  the declaration and forget to drop the `static`, or vice versa. The
  Phase 7a guide §6.4 / §6.5 audit grep
  (`grep -rnE '\b(mount|overlay|uts|cgroup|net)_(exec|cleanup|...)'`)
  catches the more general "stale dead-code" case but does not
  specifically catch this one — a `static` left in place produces a
  linker error rather than a grep-able token. The Makefile build
  surfaces it on the first `make`. Documented here so future
  promotions (e.g., a helper that needs to be reachable from `cli.c`
  in Phase 7b) remember both halves.
- **Slightly weaker information hiding.** Pre-7a, the function was
  file-scope and could be inlined / removed / renamed at will.
  Post-7a, renaming it requires updating every external caller. The
  loss is small (`find_ip_binary` is an obvious name) and the gain
  (single search-path source) is real.

**Files affected:**

- `include/net.h` — `const char *find_ip_binary(void);` declaration
  added next to `generate_veth_names` / `setup_net` /
  `configure_container_net` / `cleanup_net`.
- `src/net.c` — `static` qualifier dropped from the definition.
  Function body unchanged.
- `src/core.c` — `container_exec()` calls `find_ip_binary()` as part
  of the parent-side validation that runs before `clone()` when
  `enable_network` is set.

**Fix Applied:** 2026-05-13

---

### 33. Makefile `HELPER_OBJS` Unified Link Chain (Phase 7a)

**Decision:** Factor the per-phase test link rules into a single
`HELPER_OBJS` Make variable containing every helper module's `.o`
file. Every test target now links against `$(HELPER_OBJS)` plus its
own test object; the main `minicontainer` binary links against
`$(BUILD_DIR)/main.o $(HELPER_OBJS)`. Six identical rules instead of
six near-identical ones.

**Background — what was there:**

Through Phase 6 the Makefile had one link rule per test, each listing
the helper chain explicitly:

```makefile
$(TEST_MOUNT): $(BUILD_DIR)/test_mount.o $(BUILD_DIR)/mount.o
	...
$(TEST_OVERLAY): $(BUILD_DIR)/test_overlay.o $(BUILD_DIR)/overlay.o $(BUILD_DIR)/mount.o
	...
$(TEST_UTS): $(BUILD_DIR)/test_uts.o $(BUILD_DIR)/uts.o $(BUILD_DIR)/overlay.o $(BUILD_DIR)/mount.o
	...
$(TEST_CGROUP): $(BUILD_DIR)/test_cgroup.o $(BUILD_DIR)/cgroup.o $(BUILD_DIR)/uts.o $(BUILD_DIR)/overlay.o $(BUILD_DIR)/mount.o
	...
$(TEST_NET): $(BUILD_DIR)/test_net.o $(BUILD_DIR)/net.o $(BUILD_DIR)/cgroup.o $(BUILD_DIR)/uts.o $(BUILD_DIR)/overlay.o $(BUILD_DIR)/mount.o
	...
```

Each test pulled in just enough modules to satisfy its own per-phase
`*_exec()` call. The prerequisite lists grew monotonically — Phase 2
needed `mount.o`; Phase 3 needed Phase 2's + `overlay.o`; Phase 4
needed all that + `uts.o`; and so on.

**The Phase 7a refactor:**

Phase 7a's consolidation means every test now calls `container_exec()`
from `core.c`, which in turn calls into every helper. The minimum
required prerequisite set for any test is therefore the **full helper
chain** — there is no longer a per-phase subset. The Makefile mirrors
that:

```makefile
HELPER_OBJS = $(BUILD_DIR)/core.o $(BUILD_DIR)/env.o \
              $(BUILD_DIR)/net.o $(BUILD_DIR)/cgroup.o \
              $(BUILD_DIR)/uts.o $(BUILD_DIR)/overlay.o \
              $(BUILD_DIR)/mount.o

$(MINICONTAINER): $(BUILD_DIR)/main.o $(HELPER_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_CORE):    $(BUILD_DIR)/test_core.o    $(HELPER_OBJS) ; ...
$(TEST_MOUNT):   $(BUILD_DIR)/test_mount.o   $(HELPER_OBJS) ; ...
$(TEST_OVERLAY): $(BUILD_DIR)/test_overlay.o $(HELPER_OBJS) ; ...
$(TEST_UTS):     $(BUILD_DIR)/test_uts.o     $(HELPER_OBJS) ; ...
$(TEST_CGROUP):  $(BUILD_DIR)/test_cgroup.o  $(HELPER_OBJS) ; ...
$(TEST_NET):     $(BUILD_DIR)/test_net.o     $(HELPER_OBJS) ; ...
```

**Rationale:**

- **The link set is one fact, not six.** Phase 7a's consolidation
  removed the per-phase orchestrators that justified per-phase link
  rules. With one orchestrator, one link rule shape is sufficient;
  defining the helper set once means future module additions
  (Phase 7b's `cli.o` / `state.o`, Phase 8a's `inspector.o`,
  Phase 8b's `hardening.o`) extend a single list rather than six.
- **No more "Phase N test forgot to link Phase N-1's helper" class
  of error.** Pre-7a, a test that needed (say) `setup_overlay` had
  to remember to include `overlay.o` in its prerequisites. Now every
  test pulls every helper; the linker drops unused symbols. The
  build is correct by construction.
- **Build-time cost is negligible.** Each helper `.o` is a few KB;
  six tests times one extra `.o` adds < 100 KB to link work. The
  helper compilations themselves happen once thanks to the
  `wildcard`-based source discovery and `-MMD -MP` dependency
  generation.

**Trade-offs:**

- **Tests no longer encode their dependency surface in the
  Makefile.** Pre-7a, reading `$(TEST_OVERLAY): ...` told you what
  helpers Phase 3's test actually used. Post-7a, every test links
  everything; the dependency information lives in the `#include`
  directives of the `.c` file instead. Acceptable — `#include` is
  where new readers look anyway.
- **Single point of update for module additions.** Adding a new
  module means editing one line of `HELPER_OBJS` instead of
  remembering to add it to whichever subset of link rules needed it.
  Reduces churn but concentrates the failure mode: forget the one
  line, every test fails to link.

**Removed concerns:**

The pre-7a Makefile also had explicit `$(TEST_SPAWN)` and
`$(TEST_NAMESPACE)` rules; both were deleted along with their source
files (`test_spawn.c`, `test_namespace.c`) per Decision #30. The
`clean:` target's `rm -f` list and the `test:` target's prerequisite
list lost those entries in the same edit.

**Files affected:**

- `Makefile`:
  - `HELPER_OBJS` variable introduced near the top.
  - `$(MINICONTAINER)` rule rewritten to use `$(HELPER_OBJS)`.
  - `$(TEST_SPAWN)` / `$(TEST_NAMESPACE)` rules + variables
    deleted; `$(TEST_CORE)` rule + variable added.
  - All six surviving test rules rewritten to use `$(HELPER_OBJS)`.
  - `test:` target's prerequisite list updated.
  - `clean:` target's `rm -f` list updated.
  - Top-of-file phase comment updated to "Phase 7a: Execution-Core
    Consolidation".

**Fix Applied:** 2026-05-13

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
- **Root Cause:** Typo in macro name (extra `E`)

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
- **Phase transition note:** When moving between phases, the Makefile must be updated alongside source changes. New `.c` files in `src/` need a matching object in the link rule for the top-level binary.

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

**Fix:** Added `#include <stdlib.h>` to `test_overlay.c`.

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
message.

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
`char **test_env` in both test functions.

**Fix Applied:** 2026-04-13

---

### Error #17: `find_ip_binary()` Missed `/bin/ip` Inside the Rootfs (Phase 6)

**Date Found:** 2026-05-12

**Problem:** The initial Phase 6 implementation of `find_ip_binary()`
checked only `/sbin/ip` and `/usr/sbin/ip` — the canonical host install
locations on Ubuntu/Debian/RHEL. The parent-side call (before `clone()`,
against the host filesystem) worked correctly. The child-side call (after
`pivot_root` into the rootfs, immediately before
`configure_container_net()`) failed because `scripts/build_rootfs.sh`
places every binary in `BINS` at `/bin/<name>`, so the rootfs contains
`/bin/ip` — not `/sbin/ip` or `/usr/sbin/ip`.

**Symptom (first Phase 6 smoke test):**
```
$ sudo ./minicontainer --pid --rootfs ./rootfs --net /bin/sh -c 'ip addr show'
[network] veth pair created: veth_h_a1b2 / veth_c_a1b2
[network] Moving veth_c_a1b2 into child netns (pid 12345)
[network] Host side configured: 10.0.0.1/24
[child] Sync complete, proceeding
[child] Error: ip binary not found at /sbin/ip, /usr/sbin/ip
[child] Failed to configure container network
```

The container's veth was successfully moved into its netns by the parent,
but the child couldn't run `ip addr add ... dev veth_c_<id>` because it
couldn't find the `ip` binary at any of the paths the function checked.

**Root Cause:** A pedagogical-vs-reality mismatch in the original
`find_ip_binary()`. The function was designed against the host's
typical install paths; the author didn't think through the fact that
the same function would be called from inside the rootfs after
`pivot_root`, where the path layout is governed by
`build_rootfs.sh`'s `BINS` array convention (everything under `/bin/`).

**Impact:**
- **Severity:** High for Phase 6 smoke test — every `--net` container
  failed during child-side network configuration. The veth was created
  (resource leak risk) but the container never came up.
- **Cleanup behavior was correct:** `cleanup_net()` still ran on
  parent-side after `waitpid()`, so the host veth and iptables rule
  were torn down. No persistent state was leaked.

**Fix:** Added `/bin/ip` as a third path in `find_ip_binary()`:

```c
#define IP_BIN_SBIN     "/sbin/ip"
#define IP_BIN_USR_SBIN "/usr/sbin/ip"
#define IP_BIN_BIN      "/bin/ip"

static const char *find_ip_binary(void) {
    if (access(IP_BIN_SBIN,     X_OK) == 0) return IP_BIN_SBIN;
    if (access(IP_BIN_USR_SBIN, X_OK) == 0) return IP_BIN_USR_SBIN;
    if (access(IP_BIN_BIN,      X_OK) == 0) return IP_BIN_BIN;
    return NULL;
}
```

Architecture Decision #24 captures the design rationale (host paths
first, rootfs path last).

**Lesson:** When the same helper is called from two different
filesystem contexts (parent against host, child against rootfs), the
search paths have to cover BOTH contexts. The structural prevention is
that callers must update `BINS` in `build_rootfs.sh` before the
phase's examples will work.

**Fix Applied:** 2026-05-12

---

### Error #18: Helper `.c` Cleanup Lockstep with Helper `.h` Cleanup (Phase 7a)

**Date Found:** 2026-05-13

**Problem:** During the Phase 7a refactor, the helper headers
(`mount.h`, `overlay.h`, `uts.h`, `cgroup.h`, `net.h`) had their
per-phase `*_config_t` / `*_result_t` / `*_exec()` / `*_cleanup()`
declarations removed (per Decision #30). The matching function
bodies in the corresponding `.c` files were *not* removed in the same
edit. The result: every helper `.c` file still contained an
`*_exec()` body that referenced its now-deleted types, plus a
file-local `child_args_t`, a `static child_func()`, and a `static
close_inherited_fds()` that only existed to serve the
now-unreachable orchestrator.

**Symptom (first `make` after the header changes):**

```
$ make clean && make
gcc ... -c src/net.c -o build/net.o
src/net.c:597: error: unknown type name 'net_config_t'
src/net.c:597: error: unknown type name 'net_result_t'
src/net.c:706: error: unknown type name 'child_args_t'
...
src/cgroup.c:363: error: unknown type name 'cgroup_config_t'
src/cgroup.c:363: error: unknown type name 'cgroup_result_t'
...
src/uts.c:265: error: unknown type name 'uts_config_t'
src/uts.c:265: error: unknown type name 'uts_result_t'
...
make: *** [Makefile:64: build/net.o] Error 1
```

The compile failed before the link step. None of the dead `*_exec`
bodies were called from anywhere (the new `core.c` provides
`container_exec` and `main.c` calls that instead), but the compiler
still tries to compile every `.c` listed in the source set.

**Root Cause:** The Phase 7a refactor was specified in two halves:
header changes (delete the per-phase types and declarations) and
source changes (delete the matching function bodies). The first cut
applied only the header half. The compiler then refused to compile
the function bodies whose parameter and return types had been
deleted out from under them.

**Impact:**

- **Severity:** Build-blocking. `make` produces compile errors on
  every helper `.c` file at once. No partial mitigation — the
  refactor either applies in both halves or breaks completely.
- **No runtime risk.** This is a build-time failure; no broken
  binary ships. The compiler does its job.
- **No silent corruption.** Compile failures are loud; the error
  messages name the exact undefined types. Easier to diagnose than
  the failure modes Errors #4, #9, or #11 produced.

**Fix:**

For each of `mount.c`, `overlay.c`, `uts.c`, `cgroup.c`, `net.c`,
delete the dead block in the same edit that touched the matching
header:

1. The file-local `typedef struct { ... } child_args_t;` near the
   top of the file.
2. The `static void close_inherited_fds(bool enable_debug) { ... }`
   helper.
3. The `static int child_func(void *arg) { ... }` child entry point.
4. The `*_result_t *_exec(const *_config_t *config) { ... }`
   parent-side orchestrator.
5. The `void *_cleanup(*_result_t *result) { ... }` teardown.
6. In `cgroup.c` specifically: the synthetic `uts_config_t uts_cfg
   = {...}` workaround inside the (now-deleted) `cgroup_exec` body —
   it goes away with the surrounding function. Decision #31 covers
   the matching change in `uts.h` / `uts.c`.

Plus, for unrelated-but-clarifying hygiene, prune the now-unused
`#include` directives at the top of each file (the deleted code
needed `<sys/wait.h>` for `waitpid`, `<sys/mount.h>` for the rootfs
sequence, `<dirent.h>` and `<sys/resource.h>` for
`close_inherited_fds`, etc.; the surviving helpers don't).

**Lesson:**

When a header change deletes a type, the matching definition (or any
declaration that uses that type) must move in the same commit. The
compiler will reject the inconsistent state immediately, so the
failure is loud — but the work to fix it is per-file, and it's easy
to apply the header half first and then run `make` expecting success.
The structural prevention is to treat header changes and source
changes as a single edit unit when they reference the same types.

A canary audit catches stragglers:

```bash
grep -rnE '\b(mount|overlay|uts|cgroup|net)_(exec|cleanup|config_t|result_t)\b' \
    minicontainer/include/ minicontainer/src/ minicontainer/tests/ \
    | grep -vE '^[^:]+:[0-9]+:\s*(//|/?\*)'
```

Run after applying both halves. The `grep -vE` filters out comment
lines (historical documentation referencing the deleted types is
acceptable; live code references are not). Empty output means the
cleanup is complete.

**Why the word boundaries matter:** A naïve pattern without `\b`
catches `test_overlay_cleanup` (a test function name) as a substring
of `overlay_cleanup`. The `\b` anchors prevent that false positive.

**Files affected (fix):**

- `src/mount.c` — function bodies and prologue deleted; build size
  dropped from 6904 to 3199 bytes.
- `src/overlay.c` — same plus the §3.5 leaked-fd debug-test block;
  17732 → 8341 bytes.
- `src/uts.c` — same; 15114 → 2565 bytes.
- `src/cgroup.c` — same plus synthetic-`uts_config_t` workaround;
  20456 → 5595 bytes.
- `src/net.c` — same; 33161 → 14518 bytes. `find_ip_binary` also
  promoted from `static` to public in the same edit (Decision #32).

**Fix Applied:** 2026-05-13

---

### Error #19: Rootless `--hostname` Fails Under Ubuntu 24.04 AppArmor

**Date Found:** 2026-05-14

**Problem:** Running the documented rootless example

```
./minicontainer --user --pid --hostname mycontainer /bin/sh -c 'id && hostname'
```

from an *unconfined* shell (a plain `gnome-terminal` bash, an SSH session,
a tmux pane) on Ubuntu 24.04 fails immediately with:

```
sethostname: Operation not permitted
[child] Failed to setup UTS
```

The same binary, same flags, and same user run from the VS Code
integrated terminal succeeds. Nothing in the minicontainer source
differs between the two invocations.

**Root Cause:** Ubuntu 24.04 ships with
`kernel.apparmor_restrict_unprivileged_userns=1`. When an *unconfined*
process calls `clone(CLONE_NEWUSER)`, the kernel auto-attaches a
restrictive default AppArmor profile (`unprivileged_userns`) to the new
namespace. That profile masks `CAP_SYS_ADMIN` *within* the new userns,
so `sethostname(2)` returns `EPERM` even though the child is "root"
inside the namespace and the `CLONE_NEWUTS` flag was set correctly.

VS Code's integrated terminal runs under the `snap.code.code` AppArmor
profile in **complain** mode (`cat /proc/self/attr/current` →
`snap.code.code (complain)`). Complain-mode profiles log policy
violations but do not enforce them, which is why the same invocation
succeeds there. The mediation only kicks in for processes that the
kernel treats as unconfined at the point of namespace creation.

**Verification:**

```
$ cat /etc/os-release | grep PRETTY_NAME
PRETTY_NAME="Ubuntu 24.04.4 LTS"

$ cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns
1

$ cat /proc/self/attr/current        # from gnome-terminal bash
unconfined

$ cat /proc/self/attr/current        # from VS Code integrated terminal
snap.code.code (complain)
```

**Impact:**

- **Severity:** Medium for the rootless quickstart. The `--user --pid`
  baseline still works (no hostname change is attempted). The failure
  surfaces the moment a user adds `--hostname` to a rootless invocation
  — every advertised rootless command except the first one trips it.
- **Not a code defect.** The kernel + AppArmor stack is doing exactly
  what `apparmor_restrict_unprivileged_userns=1` asks. The code's
  step ordering (sync wait → setup_uts) is correct; there is no
  reordering that would help.
- **Trust impact for the rootless story.** The README advertises
  "rootless containers, no sudo" as the Phase 4b headline. A user who
  runs the documented commands and sees `sethostname: Operation not
  permitted` will reasonably conclude minicontainer is broken.

**Fix (documentation, not code):** The "sethostname: Operation not
permitted" troubleshooting entry in `README.md` was rewritten to cover
the Ubuntu 24.04 mediation explicitly, list the four workarounds
(disable the sysctl, install a per-binary AppArmor profile declaring
`userns,`, run from a complain-mode-confined parent, fall back to
`sudo`), and call out that the existing rootless quickstart commands
require one of those workarounds on a stock 24.04 install.

**Why no code fix:** Three options were considered.

1. **Detect the mediation and degrade gracefully** (skip `sethostname`
   with a warning when `--user --hostname` is combined and we're on a
   24.04-style system). Rejected because the "right" behavior is
   ambiguous — silently skipping a flag the user passed is worse than
   surfacing the kernel's verdict.
2. **Ship a per-binary AppArmor profile that declares `userns,`.**
   Rejected (for now) as too invasive for an educational project: an
   `/etc/apparmor.d/minicontainer` install step pulls the project out
   of "drop-in, no-config" territory. Reconsider in Phase 8b alongside
   the hardening work.
3. **Document.** Chosen. The kernel's mediation is the right
   behavior at the system level (it raises the unprivileged-userns
   security baseline by exactly the right amount); the right response
   is to teach users where the boundary is and how to opt in.

**Workarounds (in the documentation):**

```bash
# 1. Host-wide opt-out (lowers system security baseline)
sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0

# 2. Per-binary AppArmor profile (recommended, scoped to minicontainer)
sudo tee /etc/apparmor.d/minicontainer >/dev/null <<'EOF'
abi <abi/4.0>,
include <tunables/global>

profile minicontainer /path/to/minicontainer flags=(unconfined) {
    userns,
}
EOF
sudo apparmor_parser -r /etc/apparmor.d/minicontainer

# 3. Run from a complain-mode-confined parent (VS Code integrated
#    terminal, lxc-attach session, etc.)

# 4. Fall back to sudo (defeats the rootless goal)
```

**Lesson:** "rootless" is a property of the *runtime*, not of the host.
The kernel + LSM stack can mediate what a "rootless" process is
allowed to do inside its own user namespace, and that mediation is
opaque to the program itself (`sethostname` just sees `EPERM`).
Documentation for any rootless feature has to cover the LSM
interaction explicitly, because the user's first encounter with the
feature is the most likely place for a wrong impression to form
("rootless doesn't work on my distro" sticks; "rootless works once
you allow `userns,` for this binary" doesn't form unless you're
told). This generalizes beyond AppArmor (SELinux, Landlock, Lockdown
mode) — each LSM has its own knobs that can produce identical
`EPERM` surface from inside a "successfully created" namespace.

**Fix Applied:** 2026-05-14 (documentation)

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

**Phase 3 fix:** `close_inherited_fds()` iterates `/proc/self/fd` after `mount_proc()` and closes every fd above `STDERR_FILENO` before `execve()`. This handles both inherited fds and fds opened by the parent itself (overlay bookkeeping, config files, etc.). See Error #11 for a typo that initially prevented the `/proc/self/fd` path from working.

---

### 3. Single Command Execution

**Limitation:** Can only run one command, then exits.

**Rationale:** Deliberately simple — lifecycle management comes in Phase 7
(split into 7a: execution-core refactor, and 7b: CLI / `start` / `stop` /
`exec` / `inspect` / bind mounts / PTY).

**Future:** Phase 7b will add start/stop/exec container lifecycle
management via subcommand dispatch and a `/run/minicontainer/<id>/`
state-file convention.

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

**Phase 3 fix:** `build_container_env()` constructs a minimal container-appropriate environment (`PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`, `HOME=/root`, `TERM=xterm`). The `--env` flag is restored with proper key-replacement semantics.

---

### 7. No Copy-on-Write Filesystem

**Limitation:** Container writes modify the base rootfs directory directly on the host. There is no isolation between container runs.

**Impact:**
- Running `rm /bin/ls` in one container permanently deletes it for all future containers
- No way to share a single base image across multiple concurrent containers
- No clean-slate guarantee between container runs

**Phase 3 fix:** OverlayFS layers a writable `upperdir` on top of a read-only `lowerdir` (the rootfs). The container sees a merged view; writes go to the upper layer only. On container exit, the upper layer is discarded and the base image is untouched.

---

### 8. Host iptables FORWARD Chain Must Permit the Container Subnet

**Limitation:** Phase 6's `--net` flag enables IPv4 forwarding
(`/proc/sys/net/ipv4/ip_forward = 1`) and installs a MASQUERADE rule in
`nat/POSTROUTING` so the container's veth-routed traffic can reach
external networks via the host's default route. It does **not** modify
the host's `filter/FORWARD` chain. On any host where Docker, UFW, or
firewalld is installed, the default `filter/FORWARD` policy is `DROP`
and the standard rule list does not contain an ACCEPT for
`10.0.0.0/24` (the default container subnet). Packets from the
container's veth peer therefore reach the host kernel, traverse
`nat/PREROUTING`/`POSTROUTING` correctly, but get silently dropped at
the `filter/FORWARD` step.

**Impact:**

- TCP connections from inside the container hang indefinitely. With no
  client-side timeout flag, the calling process appears frozen — curl
  waits its default ~2-minute connect timeout, ping shows 100% loss,
  and nothing in minicontainer's own debug output indicates the host
  firewall is responsible.
- DNS (UDP/53) from inside the container also fails for the same
  reason; `getaddrinfo()` returns `EAI_AGAIN`.
- ICMP echo to anything outside the `10.0.0.0/24` subnet returns 100%
  loss.
- All Phase 6 `--net` example commands (e.g. `curl https://...`,
  `ping 8.8.8.8`) appear broken when the actual failure is on the
  host side.

**Diagnostic checklist** (run these on the host while the symptom is
reproducing; when all four are true, the FORWARD chain is the cause):

```bash
# 1. Verify forwarding is enabled.
cat /proc/sys/net/ipv4/ip_forward
# Expected: 1

# 2. Verify minicontainer's MASQUERADE rule is in place.
sudo iptables -t nat -L POSTROUTING -n -v | grep MASQUERADE
# Expected: a line referencing 10.0.0.0/24 (or your --net-* override).

# 3. Verify the FORWARD chain's default policy.
sudo iptables -L FORWARD -n -v | head -2
# Looking for: "Chain FORWARD (policy DROP ...)"
# Hosts without Docker/UFW/firewalld typically show "policy ACCEPT".

# 4. Verify Docker / UFW presence on the host.
ip link show | grep -E "^[0-9]+: (docker0|br-)" ; sudo ufw status 2>&1 | head -1
# docker0 present OR ufw enabled → FORWARD DROP is highly likely.
```

**Workaround.** Insert two ACCEPT rules at the top of the FORWARD
chain — one for traffic originating from the container subnet, one
for traffic returning to it:

```bash
sudo iptables -I FORWARD 1 -s 10.0.0.0/24 -j ACCEPT
sudo iptables -I FORWARD 1 -d 10.0.0.0/24 -j ACCEPT
```

Verify they landed:

```bash
sudo iptables -L FORWARD -n -v | head -4
# Expected: two ACCEPT rules at positions 1 and 2, both for 10.0.0.0/24.
```

If the default subnet has been overridden via `--net-host-ip` /
`--net-container-ip` / `--net-netmask`, substitute the actual subnet
into both rules.

After the rules are in place, re-run the failing command:

```bash
sudo ./minicontainer --pid --rootfs ./rootfs --net /bin/sh -c \
    'curl -sS --connect-timeout 5 --max-time 20 \
        -o /tmp/curl_out -w "%{http_code}\n" https://example.com'
# Expected: 200
```

The short `--connect-timeout 5 --max-time 20` is important — without
it, a still-broken FORWARD chain (or any other silent packet drop)
will hang for ~2 minutes before curl's own default timeout fires,
making the failure indistinguishable from a frozen process.

**Persistence.** The two `iptables -I FORWARD` rules above do not
survive a reboot or a manual `iptables -F`. Persistent installation is
distribution-specific:

- **Debian/Ubuntu:** `sudo apt install iptables-persistent` then
  `sudo netfilter-persistent save`.
- **RHEL/Fedora:** `sudo firewall-cmd --permanent --add-source=10.0.0.0/24`
  + `sudo firewall-cmd --permanent --direct --add-rule ipv4 filter
  FORWARD 0 -s 10.0.0.0/24 -j ACCEPT` then `sudo firewall-cmd
  --reload`.

minicontainer does not install or modify firewall persistence. By
design, the runtime touches host firewall state only through its own
MASQUERADE rule, which it creates on container start and removes on
container exit.

**Why minicontainer does not auto-install the FORWARD ACCEPT rules.**
Inserting a wildcard `FORWARD ACCEPT` for `10.0.0.0/24` is a
host-policy decision with broad blast radius:

- Docker manages its own `DOCKER-USER` and `DOCKER-FORWARD` chains;
  an unconditional ACCEPT at the top of FORWARD interacts with them
  in ways the host operator may not want.
- UFW and firewalld both treat their generated chains as the source
  of truth; rules inserted directly via `iptables -I` are invisible to
  their tooling and survive UFW/firewalld restarts in ways that
  produce confusing state.
- Reverting on container exit (the way the MASQUERADE rule is
  reverted) is not safe: a long-running second container would lose
  its FORWARD permission when an unrelated short-running container
  cleaned up.

The right place for a wildcard FORWARD policy decision is the host
operator, not a short-lived container runtime. minicontainer's
responsibility ends at the per-container NAT rule it owns.

**Future:** A Phase 7+ `setup` or `doctor` subcommand could probe the
FORWARD policy, warn loudly when DROP is in effect and no
container-subnet ACCEPT exists, and offer to install the two rules
above with explicit user confirmation. That is distinct from
unconditional auto-insertion.

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

### ✅ Phase 5: Resource Limits (cgroups) (Complete)

**What was done:**
- New `cgroup.c`/`cgroup.h` module superseding `uts.c` (Decision #21 context;
  decision body lives at #21 for `build_container_env` defensive refactor)
- Three-phase lifecycle: `setup_cgroup()` before `clone()`, `add_pid_to_cgroup()`
  after sync, `remove_cgroup()` after `waitpid()`
- `memory.max` (`--memory`), `cpu.max` (`--cpus`), `pids.max` (`--pids`)
  written to `/sys/fs/cgroup/minicontainer_<id>/`
- Auto-enable: any of `--memory`/`--cpus`/`--pids` sets `enable_cgroup`
- OOM kill at exit code 137 when `memory.max` exceeded
- `build_container_env()` defensively refactored (`calloc` + bounds check) —
  see Decision #21

---

### ✅ Phase 6: Network Namespace (Complete)

**What was done:**
- New `net.c`/`net.h` module superseding `cgroup.c` (Decision #22)
- `CLONE_NEWNET` for the container's own loopback, interfaces, routing table
- veth pair created with `ip link add`, container end moved via
  `ip link set <name> netns <pid>` (Decisions #23, #24)
- `--net` flag plus `--net-host-ip` / `--net-container-ip` /
  `--net-netmask` / `--no-nat` sub-flags (Decision #28)
- Default subnet `10.0.0.0/24` (host `10.0.0.1`, container `10.0.0.2`)
- Optional `iptables -t nat -A POSTROUTING -j MASQUERADE` for outbound
  internet; default ON, disabled by `--no-nat`
- `find_ip_binary()` three-path search: `/sbin/ip` → `/usr/sbin/ip` →
  `/bin/ip` (Decision #24, Error #17)
- `generate_veth_names()` runs BEFORE `clone()` (Decision #25)
- Sync pipe widened to `enable_user_namespace OR enable_network`
  (Decision #26)
- `cleanup_net()` stores `nat_source_cidr` in context for self-contained
  teardown (Decision #27)
- `scripts/build_rootfs.sh`: `BINS` array extended to include `ip`,
  plus curl + CA bundle + NSS modules + resolv.conf for HTTPS client
  testing from inside the container netns (Decision #29)
- Unit tests (`test_net`) require root + iproute2
- Operational prerequisite for `--net` curl/DNS traffic on hosts with
  Docker, UFW, or firewalld installed: two `iptables -I FORWARD 1 ...
  ACCEPT` rules permitting the container subnet — see Known Limitation
  #8 for the diagnostic checklist and commands

---

### Phase 7: CLI & Lifecycle (Split into 7a + 7b — Designed, Not Yet Implemented)

**Phase 7a — Execution-Core Consolidation (refactor, no new features):**
- New `core.c`/`core.h` with unified `container_exec()` collapsing five
  duplicate `child_args_t` / `child_func()` / `close_inherited_fds()`
  copies into one
- New `env.c`/`env.h` extracting `build_container_env()` (Decision #21
  was designed for this extraction)
- Unified `container_config_t` and `container_context_t`
- New `user_ns_mapping_t` in `uts.h` replaces the synthetic-`uts_config_t`
  workaround introduced for Phase 5's `cgroup_exec`
- `spawn.c`, `namespace.c` and their tests deleted; coverage replaced by
  `test_core.c`
- Success criterion: every Phase 0-6 test still passes byte-identical

**Phase 7b — CLI / Lifecycle / Bind Mounts / PTY:**
- Subcommand dispatch: `run` / `start` / `stop` / `exec` / `inspect` /
  `list` / `cleanup`
- `container_exec()` splits into `container_start()` + `container_wait()`
  so the CLI can write the state file BETWEEN the two halves
- State at `/run/minicontainer/<id>/state.json` (rootless under
  `$XDG_RUNTIME_DIR`)
- 12-hex container IDs from `/dev/urandom` (not timestamp-based)
- Bind mounts (`--volume host:container[:ro]`) — note the `MS_RDONLY`
  remount quirk: first `mount(MS_BIND)` silently ignores `MS_RDONLY`,
  a second `mount(MS_BIND|MS_REMOUNT|MS_RDONLY)` is required
- PTY allocation (`--interactive`) via `posix_openpt` → `grantpt` →
  `unlockpt` → `ptsname`
- `cmd_exec` uses `setns(2)` then `clone()` WITHOUT `CLONE_NEW*` flags
  (joining existing namespaces vs. creating fresh ones)
- `cmd_start` runs `container_start()` and exits — child orphaned to init,
  `cleanup` reaps stale state files

---

### Phase 8: OCI / Inspector (Designed, Not Yet Implemented — Split into 8a/8b/8c)

- **8a:** `libprocfs` extracted into a sibling shared library — read-side
  of cgroups (`memory.current`, `cpu.stat`, `pids.current`) and
  `/proc/<pid>/...` parsing. `minicontainer inspect <id>` consumes it.
- **8b:** Hardening — capability set (`capset`), seccomp BPF filter,
  no-new-privileges.
- **8c:** OCI runtime spec compliance (`config.json`, bundle format),
  minimal image extraction from tarballs.

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

### Phase 3 Unit Tests (tests/test_overlay.c — requires root + rootfs)

- ✓ Base image untouched after container writes
- ✓ Overlay cleanup (upper / work / merged removed on exit)
- ✓ Backward compatibility (no overlay — Phase 2 behavior preserved)

### Phase 4 / 4b / 4c Unit Tests (tests/test_uts.c — mixed privileges)

- ✓ Hostname isolation (`sethostname` in new UTS namespace)
- ✓ Backward compatibility (no UTS — Phase 3 behavior preserved)
- ✓ User namespace unprivileged (container root maps to host user)
- ✓ User namespace + hostname (rootless with UTS)
- ✓ IPC isolation (System V IPC tables empty in new IPC namespace)
- ✓ IPC + user namespace (rootless with IPC)

### Phase 5 Unit Tests (tests/test_cgroup.c — requires root)

- ✓ Cgroup creation and cleanup lifecycle (mkdir/rmdir)
- ✓ Memory limit enforcement (process exits within budget)
- ✓ PID limit (fork-bomb defense: kernel returns EAGAIN at cap)
- ✓ Backward compatibility (no cgroup — Phase 4c behavior preserved)
- ✓ Cgroup + IPC namespace (all isolation layers active together)

### Phase 6 Unit Tests (tests/test_net.c — requires root + iproute2)

- ✓ Network namespace creation (`CLONE_NEWNET` + veth pair, container
  sees `veth_c_<id>`)
- ✓ Backward compatibility (no `--net` — Phase 5 behavior preserved)
- ✓ Network namespace combined with cgroup (veth + memory/cpu/pids
  active simultaneously)

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
- network_namespaces(7): Network namespace semantics (Phase 6)
- ip(8): iproute2 driver — link/addr/route/netns subcommands (Phase 6)
- iptables(8) / iptables-extensions(8): NAT (MASQUERADE) and filter rules (Phase 6)
- veth(4): Virtual Ethernet pair device (Phase 6)
- [LWN: Namespaces in operation](https://lwn.net/Articles/531114/)
- [LWN: Mount namespaces and shared subtrees](https://lwn.net/Articles/689856/)
- [LWN: Network namespaces](https://lwn.net/Articles/580893/)
- [Linux System Programming by Robert Love](https://www.oreilly.com/library/view/linux-system-programming/9781449341527/)

---

**End of Design Decisions Document**
