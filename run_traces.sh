#!/bin/bash
#
# run_traces.sh — Syscall tracing exercises for minicontainer Phase 1
#
# Run with: sudo bash run_traces.sh
# Outputs saved to: ./traces/
#
# Each exercise builds on the last. Read the output files in order.
#

set -euo pipefail

MINI="./minicontainer"
OUT="./traces"

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: Must run as root (sudo bash run_traces.sh)"
    exit 1
fi

if [ ! -x "$MINI" ]; then
    echo "ERROR: $MINI not found or not executable. Run 'make' first."
    exit 1
fi

mkdir -p "$OUT"
echo "=== Saving all trace outputs to $OUT/ ==="
echo ""

# ============================================================================
# EXERCISE 1: Baseline — full unfiltered trace without namespace
# Purpose: See EVERYTHING that happens during a simple exec. Establishes
#          a baseline to compare against namespace-enabled runs.
# ============================================================================
echo "[1/12] Full unfiltered trace (no namespace)..."
strace -f -o "$OUT/01_full_no_namespace.txt" \
    $MINI /bin/echo "hello from minicontainer"
echo "  -> $OUT/01_full_no_namespace.txt"

# ============================================================================
# EXERCISE 2: Baseline — full unfiltered trace WITH namespace
# Purpose: Compare against Exercise 1. Diff the two files to see exactly
#          what CLONE_NEWPID adds.
# ============================================================================
echo "[2/12] Full unfiltered trace (with --pid namespace)..."
strace -f -o "$OUT/02_full_with_namespace.txt" \
    $MINI --pid /bin/echo "hello from namespace"
echo "  -> $OUT/02_full_with_namespace.txt"

# ============================================================================
# EXERCISE 3: Filtered — only container-relevant syscalls, no namespace
# Purpose: Cut through the noise. Focus on clone/execve/wait4 to see
#          the fork-exec-wait pattern clearly.
# ============================================================================
echo "[3/12] Key syscalls only (no namespace)..."
strace -f -e trace=clone,execve,wait4,getpid,getppid,write \
    -o "$OUT/03_key_syscalls_no_namespace.txt" \
    $MINI /bin/echo "no namespace"
echo "  -> $OUT/03_key_syscalls_no_namespace.txt"

# ============================================================================
# EXERCISE 4: Filtered — only container-relevant syscalls, WITH namespace
# Purpose: Compare clone() flags against Exercise 3. Look for CLONE_NEWPID
#          in the flags and getpid() returning 1 in the child.
# ============================================================================
echo "[4/12] Key syscalls only (with --pid namespace)..."
strace -f -e trace=clone,execve,wait4,getpid,getppid,write \
    -o "$OUT/04_key_syscalls_with_namespace.txt" \
    $MINI --pid /bin/echo "in namespace"
echo "  -> $OUT/04_key_syscalls_with_namespace.txt"

# ============================================================================
# EXERCISE 5: Per-process split — separate parent and child traces
# Purpose: Read each process's syscall stream independently. The parent
#          file shows clone+wait4. The child file shows execve+write.
# ============================================================================
echo "[5/12] Per-process split traces (--pid namespace)..."
strace -ff -o "$OUT/05_split" \
    $MINI --pid /bin/echo "split trace"
echo "  -> $OUT/05_split.<pid> (one file per process)"

# ============================================================================
# EXERCISE 6: Memory operations — trace stack allocation for clone
# Purpose: See the malloc (via brk/mmap) that allocates the 1MB stack
#          before clone() is called, and the free (munmap) after cleanup.
# ============================================================================
echo "[6/12] Memory allocation trace (stack for clone)..."
strace -f -e trace=brk,mmap,munmap,mprotect,clone \
    -o "$OUT/06_memory_operations.txt" \
    $MINI --pid /bin/echo "memory trace"
echo "  -> $OUT/06_memory_operations.txt"

# ============================================================================
# EXERCISE 7: Timing — measure how long each syscall takes
# Purpose: See real-world latency of clone() with CLONE_NEWPID vs execve.
#          The -T flag appends elapsed time in <seconds> after each call.
# ============================================================================
echo "[7/12] Timing trace (with -T flag)..."
strace -f -T -e trace=clone,execve,wait4 \
    -o "$OUT/07_timing.txt" \
    $MINI --pid /bin/echo "timing test"
echo "  -> $OUT/07_timing.txt"

# ============================================================================
# EXERCISE 8: Summary statistics — syscall counts and time breakdown
# Purpose: High-level overview. Shows which syscalls are called most,
#          which take the most time. Great for spotting bottlenecks.
# ============================================================================
echo "[8/12] Summary statistics (no namespace)..."
strace -f -c -o "$OUT/08_summary_no_namespace.txt" \
    $MINI /bin/echo "summary baseline"
echo "  -> $OUT/08_summary_no_namespace.txt"

echo "[8b/12] Summary statistics (with namespace)..."
strace -f -c -o "$OUT/08b_summary_with_namespace.txt" \
    $MINI --pid /bin/echo "summary namespace"
echo "  -> $OUT/08b_summary_with_namespace.txt"

# ============================================================================
# EXERCISE 9: Detailed trace + summary combined
# Purpose: Full trace with the summary table appended at the end.
#          Best of both worlds for a single reference file.
# ============================================================================
echo "[9/12] Combined detailed trace + summary..."
strace -f -C -e trace=clone,execve,wait4,getpid,getppid,write,read,openat \
    -o "$OUT/09_combined_trace_and_summary.txt" \
    $MINI --pid --debug /bin/sh -c 'echo "PID=$$"'
echo "  -> $OUT/09_combined_trace_and_summary.txt"

# ============================================================================
# EXERCISE 10: Error path — trace a failed execve
# Purpose: See what happens when the child can't exec. Look for
#          execve returning ENOENT and the exit code 127 path.
# ============================================================================
echo "[10/12] Error path trace (bad command)..."
strace -f -e trace=clone,execve,wait4,exit_group,write \
    -o "$OUT/10_error_path.txt" \
    $MINI --pid /bin/this_does_not_exist 2>/dev/null || true
echo "  -> $OUT/10_error_path.txt"

# ============================================================================
# EXERCISE 11: ltrace — library call trace
# Purpose: See the C library layer: malloc/free for stack allocation,
#          fprintf for debug output, etc. Complements strace's kernel view.
# ============================================================================
echo "[11/12] ltrace — library calls..."
ltrace -f -e malloc+free+execve+fprintf+perror \
    -o "$OUT/11_ltrace_library_calls.txt" \
    $MINI --pid /bin/echo "ltrace test" 2>/dev/null || true
echo "  -> $OUT/11_ltrace_library_calls.txt"

# ============================================================================
# EXERCISE 12: /proc inspection from inside the namespace
# Purpose: Trace the file operations when reading /proc/self/status
#          from inside a PID namespace. Look for NSpid in the output.
# ============================================================================
echo "[12/12] /proc inspection inside namespace..."
strace -f -e trace=clone,execve,openat,read,write,wait4 \
    -o "$OUT/12_proc_inspection.txt" \
    $MINI --pid /bin/cat /proc/self/status \
    > "$OUT/12_proc_status_output.txt" 2>&1
echo "  -> $OUT/12_proc_inspection.txt"
echo "  -> $OUT/12_proc_status_output.txt"

# ============================================================================
# BONUS: Generate a diff of Exercise 3 vs 4 (namespace vs no namespace)
# ============================================================================
echo ""
echo "[bonus] Generating diff of key syscalls (namespace vs no namespace)..."
diff "$OUT/03_key_syscalls_no_namespace.txt" \
     "$OUT/04_key_syscalls_with_namespace.txt" \
     > "$OUT/bonus_namespace_diff.txt" 2>&1 || true
echo "  -> $OUT/bonus_namespace_diff.txt"

# ============================================================================
# Summary
# ============================================================================
echo ""
echo "=== All traces complete ==="
echo ""
echo "Files generated:"
ls -lhS "$OUT"/ | tail -n +2
echo ""
echo "Suggested reading order:"
echo "  1. Start with 03 and 04 — compare clone() flags"
echo "  2. Read 05_split.* — understand parent vs child perspectives"
echo "  3. Read 06 — see stack allocation before clone()"
echo "  4. Read 08 and 08b — compare syscall counts with/without namespace"
echo "  5. Read 10 — understand the error/failure path"
echo "  6. Read 11 — see the library-level view (malloc, free)"
echo "  7. Read 12 — see /proc from inside the namespace"
echo "  8. Read 01 and 02 for the full unfiltered picture"
echo "  9. Read bonus_namespace_diff.txt for a direct comparison"
