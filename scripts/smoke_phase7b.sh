#!/usr/bin/env bash
#
# smoke_phase7b.sh — extensive smoke test of Phase 7b capabilities.
#
# Phase 7b shipped the CLI / lifecycle layer on top of the Phase 7a
# execution core.  This script exercises every user-visible claim:
#
#   * Subcommand dispatch        run / start / stop / exec / inspect /
#                                list / cleanup
#   * Implicit-run backwards compat (`minicontainer [flags] <cmd>`)
#   * container_start + container_wait split (state visible mid-run,
#     auto-removed on clean exit)
#   * State files                <root>/<id>/{state.json,pidfile,logs/}
#   * 12-hex container IDs from /dev/urandom
#   * Bind mounts                --volume host:cont[:ro]  (rw + ro)
#   * PTY allocation             --interactive (+ mutual exclusion)
#   * Detached logging           start → logs/{stdout,stderr}.log
#   * Namespaces                 --pid / --hostname(uts) / mount(rootfs)
#   * cgroup limits              --memory / --pids reflected host-side
#   * cgroup ENFORCEMENT         child placed before it runs; pids.events
#                                records denials; memory.current accounts
#                                (Error #25 regression)
#   * overlay --overlay          end-to-end COW write (Error #24 regression)
#   * Network state serialization --net veth block in state.json
#   * cleanup sweeper            dead-PID state dirs (dry-run + real)
#   * exit-code propagation      normal exit + signal death
#   * negative / parse-error paths
#
# Regression coverage for the post-test corrections (decisions.md):
#   #22 bind-after-pivot   §11 (host-only source path)
#   #23 exec self-userns   §14 (exec into a plain non-user container)
#   #24 overlay id         §16 (run --overlay --rootfs end-to-end)
#   #25 cgroup placement   §12 + §15 (placement, accounting, fork denial)
#
# Tests that need real namespaces require root; run with sudo for the
# full suite.  Without root, the privileged tests SKIP (not FAIL) and
# the unprivileged subset (CLI surface, host-binary lifecycle, PTY,
# state files, cleanup) still runs.
#
# SAFETY: the script runs from a scratch CWD so `cleanup`'s step-5
# `rm -rf ./containers/*` sweep cannot touch the real project tree.
#
# Usage:  ./scripts/smoke_phase7b.sh [ROOTFS_PATH]
#         sudo ./scripts/smoke_phase7b.sh                 # full suite
#         ROOTFS=/path/to/rootfs sudo ./scripts/smoke_phase7b.sh

set -u

# ---------------------------------------------------------------------------
# Locations
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="$PROJECT_DIR/minicontainer"

# Rootfs: arg > $ROOTFS > ubuntu-root > rootfs.  Resolved to absolute
# because we cd into a scratch dir below.
pick_rootfs() {
    if [ "${1:-}" ]; then echo "$1"; return; fi
    if [ "${ROOTFS:-}" ]; then echo "$ROOTFS"; return; fi
    if [ -d "$PROJECT_DIR/ubuntu-root" ]; then echo "$PROJECT_DIR/ubuntu-root"; return; fi
    echo "$PROJECT_DIR/rootfs"
}
ROOTFS="$(pick_rootfs "${1:-}")"
[ -d "$ROOTFS" ] && ROOTFS="$(cd "$ROOTFS" && pwd)"

EUID_VAL="$(id -u)"
IS_ROOT=0; [ "$EUID_VAL" = "0" ] && IS_ROOT=1

if [ "$IS_ROOT" = "1" ]; then
    STATE_ROOT="/run/minicontainer"
else
    STATE_ROOT="${XDG_RUNTIME_DIR:-/run/user/$EUID_VAL}/minicontainer"
fi

# In-container binary paths (same layout for rootfs/ and ubuntu-root/).
C_SH=/bin/sh
C_HOSTNAME=/bin/hostname
C_SLEEP=/bin/sleep
C_CAT=/bin/cat
C_IP=/bin/ip

# Host binaries for unprivileged (no-namespace) runs.
H_SH=/bin/sh
H_SLEEP=/bin/sleep
H_ECHO=/bin/echo

WORKDIR="$(mktemp -d)"
ERRFILE="$WORKDIR/.stderr"
declare -a TRACKED_IDS=()

# ---------------------------------------------------------------------------
# Output / counters
# ---------------------------------------------------------------------------
if [ -t 1 ]; then
    R='\033[31m'; G='\033[32m'; Y='\033[33m'; B='\033[34m'; D='\033[2m'; Z='\033[0m'
else
    R=''; G=''; Y=''; B=''; D=''; Z=''
fi
TESTS=0; PASS=0; FAIL=0; SKIP=0
declare -a FAILED_NAMES=()

section() { printf "\n${B}== %s ==${Z}\n" "$1"; }
pass()    { TESTS=$((TESTS+1)); PASS=$((PASS+1)); printf "  ${G}PASS${Z} %s\n" "$1"; }
fail()    { TESTS=$((TESTS+1)); FAIL=$((FAIL+1)); FAILED_NAMES+=("$1");
            printf "  ${R}FAIL${Z} %s\n" "$1"; [ "${2:-}" ] && printf "       ${D}%s${Z}\n" "$2"; }
skip()    { TESTS=$((TESTS+1)); SKIP=$((SKIP+1)); printf "  ${Y}SKIP${Z} %s ${D}(%s)${Z}\n" "$1" "${2:-}"; }

# _run <argv...> : capture OUT (stdout), ERR (stderr), RC (exit code)
_run() {
    OUT="$("$@" 2>"$ERRFILE")"; RC=$?; ERR="$(cat "$ERRFILE")"
}
# _run_to <seconds> <argv...> : same, wrapped in `timeout`
_run_to() {
    local t="$1"; shift
    OUT="$(timeout "$t" "$@" 2>"$ERRFILE")"; RC=$?; ERR="$(cat "$ERRFILE")"
}

# Assertion helpers (each is one test line)
ck_eq()       { if [ "$2" = "$3" ]; then pass "$1"; else fail "$1" "expected [$3] got [$2]"; fi; }
ck_ne()       { if [ "$2" != "$3" ]; then pass "$1"; else fail "$1" "expected != [$3]"; fi; }
ck_contains() { case "$2" in *"$3"*) pass "$1";; *) fail "$1" "missing [$3] in: $(printf '%s' "$2" | head -c 200)";; esac; }
ck_lacks()    { case "$2" in *"$3"*) fail "$1" "unexpected [$3] present";; *) pass "$1";; esac; }
ck_match()    { if printf '%s' "$2" | grep -Eq "$3"; then pass "$1"; else fail "$1" "no /$3/ in: $(printf '%s' "$2" | head -c 200)"; fi; }
ck_file()     { if [ -e "$2" ]; then pass "$1"; else fail "$1" "missing file: $2"; fi; }
ck_nofile()   { if [ ! -e "$2" ]; then pass "$1"; else fail "$1" "file still present: $2"; fi; }

track()   { TRACKED_IDS+=("$1"); }

cleanup_all() {
    for id in "${TRACKED_IDS[@]:-}"; do
        [ "$id" ] || continue
        "$BIN" stop "$id" >/dev/null 2>&1
        rm -rf "$STATE_ROOT/$id" 2>/dev/null
    done
    # Reap resources `stop` doesn't (veth interfaces, iptables NAT
    # rules, cgroup dirs) left behind by our now-dead test containers.
    # cleanup only touches dead-owner resources, so live containers are
    # untouched.  We're still cd'd into the scratch dir, so cleanup's
    # ./containers sweep can't reach the real project tree.
    "$BIN" cleanup >/dev/null 2>&1
    rm -rf "$WORKDIR" 2>/dev/null
}
trap cleanup_all EXIT

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
section "Preflight"
if [ ! -x "$BIN" ]; then
    printf "${R}error:${Z} binary not found/executable at %s\n" "$BIN"
    printf "       build it first:  (cd %s && make)\n" "$PROJECT_DIR"
    exit 2
fi
printf "  binary    : %s\n" "$BIN"
printf "  rootfs    : %s%s\n" "$ROOTFS" "$([ -d "$ROOTFS" ] || echo '  (MISSING — rootfs tests will SKIP)')"
printf "  euid      : %s (%s)\n" "$EUID_VAL" "$([ "$IS_ROOT" = 1 ] && echo root || echo unprivileged)"
printf "  state root: %s\n" "$STATE_ROOT"
printf "  scratch   : %s\n" "$WORKDIR"

ROOTFS_OK=0
if [ -d "$ROOTFS" ] && [ -x "$ROOTFS$C_SH" ]; then ROOTFS_OK=1; fi

cd "$WORKDIR" || { echo "cannot cd to scratch dir"; exit 2; }

# ===========================================================================
section "1. CLI dispatch & usage"
# ===========================================================================
_run "$BIN"
ck_eq      "no args exits non-zero"            "$RC" "1"
ck_contains "no args prints usage"             "$ERR$OUT" "Usage:"

_run "$BIN" --help
ck_eq      "--help exits 0"                    "$RC" "0"
ck_contains "--help lists subcommands"         "$ERR$OUT" "Subcommands:"
ck_contains "--help documents --volume"        "$ERR$OUT" "--volume"
ck_contains "--help documents --interactive"   "$ERR$OUT" "--interactive"

_run "$BIN" bogus-subcommand
ck_eq      "unknown subcommand exits 1"        "$RC" "1"
ck_contains "unknown subcommand message"       "$ERR" "unknown subcommand"

# ===========================================================================
section "2. Implicit-run backwards compat (no namespaces, host binary)"
# ===========================================================================
_run "$BIN" "$H_ECHO" hello-implicit
ck_eq      "implicit run exit 0"               "$RC" "0"
ck_contains "implicit run output"              "$OUT" "hello-implicit"

_run "$BIN" run "$H_ECHO" hello-explicit
ck_eq      "explicit run exit 0"               "$RC" "0"
ck_contains "explicit run output"              "$OUT" "hello-explicit"

# ===========================================================================
section "3. Exit-code propagation"
# ===========================================================================
_run "$BIN" run "$H_SH" -c "exit 42"
ck_eq      "normal exit code propagates"       "$RC" "42"

_run "$BIN" run "$H_SH" -c 'kill -9 $$'
ck_eq      "signal death → 128+SIGKILL"        "$RC" "137"
ck_contains "signal death reported on stderr"  "$ERR" "killed by signal"

# ===========================================================================
section "4. Environment passthrough (--env)"
# ===========================================================================
_run "$BIN" run --env SMOKE_KEY=smoke_val "$H_SH" -c 'echo $SMOKE_KEY'
ck_eq      "--env exit 0"                       "$RC" "0"
ck_contains "--env value visible in container"  "$OUT" "smoke_val"

# ===========================================================================
section "5. Detached lifecycle: start / list / inspect / stop"
# ===========================================================================
# Uses host binary + no namespaces so it runs unprivileged too.
_run "$BIN" start "$H_SLEEP" 60
ID="$OUT"
if printf '%s' "$ID" | grep -Eq '^[0-9a-f]{12}$'; then
    pass "start prints 12-hex container ID"
    track "$ID"
else
    fail "start prints 12-hex container ID" "got [$ID] / stderr: $ERR"
    ID=""
fi

if [ "$ID" ]; then
    SDIR="$STATE_ROOT/$ID"
    ck_file   "state dir created"                "$SDIR"
    ck_file   "state.json written"              "$SDIR/state.json"
    ck_file   "pidfile written"                 "$SDIR/pidfile"
    ck_file   "logs/stdout.log created"         "$SDIR/logs/stdout.log"
    ck_file   "logs/stderr.log created"         "$SDIR/logs/stderr.log"

    # pidfile content matches state.json pid
    PF="$(cat "$SDIR/pidfile" 2>/dev/null)"
    SJPID="$(grep -oE '"pid":[[:space:]]*[0-9]+' "$SDIR/state.json" | grep -oE '[0-9]+')"
    ck_eq    "pidfile matches state.json pid"   "$PF" "$SJPID"
    # the recorded pid is actually alive
    if kill -0 "$PF" 2>/dev/null; then pass "recorded PID is alive"; else fail "recorded PID is alive" "pid $PF"; fi

    # state.json uses the _ns-suffixed namespace keys (Phase 7b fix)
    ck_contains "state.json uses pid_ns key"    "$(cat "$SDIR/state.json")" '"pid_ns"'

    _run "$BIN" list
    ck_eq       "list exit 0"                    "$RC" "0"
    ck_contains "list shows container ID"        "$OUT" "$ID"
    ck_contains "list header present"            "$OUT" "CONTAINER ID"

    _run "$BIN" inspect "$ID"
    ck_eq       "inspect running exit 0"         "$RC" "0"
    ck_contains "inspect status running"         "$OUT" '"status":      "running"'
    ck_contains "inspect echoes ID"             "$OUT" "$ID"

    _run "$BIN" stop "$ID"
    ck_eq       "stop exit 0"                    "$RC" "0"
    ck_contains "stop confirmation"             "$OUT" "Stopped $ID"
    ck_nofile   "state dir removed after stop"  "$SDIR"

    _run "$BIN" inspect "$ID"
    ck_eq       "inspect after stop exits 1"     "$RC" "1"
    ck_contains "inspect after stop: no such"    "$ERR" "No such container"

    _run "$BIN" stop "$ID"
    ck_eq       "stop unknown id exits 1"        "$RC" "1"
else
    for t in "state dir" "state.json" "pidfile" "logs" "list" "inspect" "stop"; do
        skip "lifecycle: $t" "no container id"
    done
fi

# ===========================================================================
section "6. start/wait split: state visible mid-run, gone after clean exit"
# ===========================================================================
# A FOREGROUND run must publish its state file while running and remove
# it on clean exit (cli.c: container_start → state_save → container_wait
# → state_remove).
"$BIN" run "$H_SLEEP" 3 >/dev/null 2>&1 &
BGPID=$!
sleep 1
_run "$BIN" list
MIDRUN_OUT="$OUT"
wait "$BGPID"; FG_RC=$?
_run "$BIN" list
AFTER_OUT="$OUT"

# Count sleep-bearing rows mid-run vs after.
MID_N="$(printf '%s\n' "$MIDRUN_OUT" | grep -c "sleep")"
AFT_N="$(printf '%s\n' "$AFTER_OUT" | grep -c "sleep")"
if [ "$MID_N" -ge 1 ]; then pass "foreground container visible in list mid-run"
else fail "foreground container visible in list mid-run" "no 'sleep' row mid-run"; fi
ck_eq "foreground run clean exit 0"            "$FG_RC" "0"
if [ "$AFT_N" -lt "$MID_N" ]; then pass "state auto-removed after clean exit"
else fail "state auto-removed after clean exit" "mid=$MID_N after=$AFT_N"; fi

# ===========================================================================
section "7. PTY allocation (--interactive)"
# ===========================================================================
# Mutual exclusion is a deterministic parse-level check.
_run "$BIN" run --interactive --detach "$H_SH" -c true
ck_ne       "interactive+detach rejected (rc!=0)" "$RC" "0"
ck_contains "interactive+detach error message"    "$ERR" "mutually exclusive"

# Live PTY: drive the interactive shell and ask it for its tty.  The
# slave is a real pts, so the inner shell goes interactive and `tty`
# resolves to /dev/pts/N.  We MUST hold stdin open with small delays:
# closing it immediately EOFs the forwarder, which tears down the PTY
# master and SIGHUPs the shell before it can read the command (that's
# a harness race, not a runtime bug — --interactive targets a real
# terminal).
PTY_OUT="$({ printf 'tty\n'; sleep 2; printf 'exit\n'; sleep 1; } \
            | timeout 12 "$BIN" run --interactive "$H_SH" 2>/dev/null)"
ck_contains "interactive shell gets a pts tty"    "$PTY_OUT" "/dev/pts/"

# ===========================================================================
section "8. Negative / parse-error paths"
# ===========================================================================
_run "$BIN" run --overlay "$H_ECHO" hi
ck_ne       "--overlay without --rootfs rejected" "$RC" "0"
ck_contains "--overlay requires --rootfs msg"     "$ERR" "--overlay requires --rootfs"

_run "$BIN" run --net-host-ip 10.0.0.1 "$H_ECHO" hi
ck_ne       "net-* without --net rejected"        "$RC" "0"
ck_contains "net-* requires --net msg"            "$ERR" "require --net"

_run "$BIN" run --volume /tmp "$H_ECHO" hi
ck_ne       "malformed --volume rejected"         "$RC" "0"
ck_contains "malformed --volume msg"              "$ERR" "Invalid --volume"

_run "$BIN" run --rootfs "$ROOTFS"
ck_ne       "run with no command rejected"        "$RC" "0"
ck_contains "no command msg"                       "$ERR" "No command specified"

_run "$BIN" exec
ck_ne       "exec with no args rejected"          "$RC" "0"

_run "$BIN" inspect no-such-id-xyz
ck_eq       "inspect unknown id exits 1"          "$RC" "1"
ck_contains "inspect unknown id msg"              "$ERR" "No such container"

# ===========================================================================
section "9. cleanup sweeper (orphaned state dir)"
# ===========================================================================
# Create an orphan: start detached, then kill -9 the PID directly so the
# state dir is left behind with a dead PID.
_run "$BIN" start "$H_SLEEP" 120
OID="$OUT"
if printf '%s' "$OID" | grep -Eq '^[0-9a-f]{12}$'; then
    track "$OID"
    OPID="$(cat "$STATE_ROOT/$OID/pidfile" 2>/dev/null)"
    kill -9 "$OPID" 2>/dev/null
    # reap if it's our child / give it a moment to die
    wait "$OPID" 2>/dev/null
    for _ in 1 2 3 4 5; do kill -0 "$OPID" 2>/dev/null || break; sleep 0.2; done

    _run "$BIN" cleanup --dry-run
    ck_eq       "cleanup --dry-run exit 0"          "$RC" "0"
    ck_contains "dry-run reports the orphan"        "$OUT$ERR" "$OID"
    ck_file     "dry-run leaves state dir intact"   "$STATE_ROOT/$OID/state.json"

    _run "$BIN" cleanup
    ck_eq       "cleanup exit 0"                     "$RC" "0"
    ck_nofile   "cleanup removed orphan state dir"   "$STATE_ROOT/$OID"
else
    skip "cleanup sweeper" "could not create orphan (stderr: $ERR)"
fi

# ===========================================================================
section "10. Namespaces (root + rootfs required)"
# ===========================================================================
if [ "$IS_ROOT" != "1" ]; then
    skip "PID namespace"       "needs root"
    skip "UTS namespace"       "needs root"
    skip "mount namespace"     "needs root"
elif [ "$ROOTFS_OK" != "1" ]; then
    skip "PID namespace"       "rootfs missing $ROOTFS$C_SH"
    skip "UTS namespace"       "rootfs missing"
    skip "mount namespace"     "rootfs missing"
else
    # The cloned child IS pid 1 of the new pid namespace, so $$ == 1.
    _run_to 30 "$BIN" run --pid --rootfs "$ROOTFS" "$C_SH" -c 'echo $$'
    ck_eq       "PID ns: container cmd is PID 1"     "$OUT" "1"

    _run_to 30 "$BIN" run --hostname smokebox --rootfs "$ROOTFS" "$C_HOSTNAME"
    ck_contains "UTS ns: hostname applied"           "$OUT" "smokebox"

    # mount ns / pivot_root: the container root is the rootfs tree, so a
    # marker file in the host scratch CWD is NOT visible, and the
    # rootfs's own /etc IS.  (`ls /` only — keeps this rootfs-agnostic;
    # the minimal rootfs has no coreutils head / os-release.)
    echo HOSTMARKER > "$WORKDIR/host_only_marker"
    _run_to 30 "$BIN" run --rootfs "$ROOTFS" "$C_SH" -c 'ls /'
    ck_lacks    "mount ns: host scratch file not visible" "$OUT" "host_only_marker"
    ck_contains "mount ns: rootfs /etc present"            "$OUT" "etc"
fi

# ===========================================================================
section "11. Bind mounts (--volume, root + rootfs)"
# ===========================================================================
if [ "$IS_ROOT" != "1" ] || [ "$ROOTFS_OK" != "1" ]; then
    skip "bind mount rw"  "$([ "$IS_ROOT" = 1 ] && echo 'rootfs missing' || echo 'needs root')"
    skip "bind mount ro"  "$([ "$IS_ROOT" = 1 ] && echo 'rootfs missing' || echo 'needs root')"
else
    VOLDIR="$WORKDIR/vol"; mkdir -p "$VOLDIR"
    echo "volume-payload" > "$VOLDIR/payload.txt"

    _run_to 30 "$BIN" run --rootfs "$ROOTFS" --volume "$VOLDIR:/mnt" \
        "$C_CAT" /mnt/payload.txt
    ck_contains "bind mount rw: payload readable" "$OUT" "volume-payload"

    # if/else form: shell-agnostic (dash returns 2, bash 1, on a
    # redirection failure — so don't pin the exact code; assert the
    # branch instead).
    _run_to 30 "$BIN" run --rootfs "$ROOTFS" --volume "$VOLDIR:/mnt:ro" \
        "$C_SH" -c 'if echo blocked > /mnt/payload.txt 2>/dev/null; then echo WROTE; else echo BLOCKED; fi'
    ck_contains "bind mount ro: write rejected"      "$OUT" "BLOCKED"
    HOSTVAL="$(cat "$VOLDIR/payload.txt")"
    ck_eq       "bind mount ro: host file unchanged" "$HOSTVAL" "volume-payload"
fi

# ===========================================================================
section "12. cgroup limits reflected host-side (root + rootfs)"
# ===========================================================================
if [ "$IS_ROOT" != "1" ] || [ "$ROOTFS_OK" != "1" ]; then
    skip "cgroup --memory" "$([ "$IS_ROOT" = 1 ] && echo 'rootfs missing' || echo 'needs root')"
    skip "cgroup --pids"   "$([ "$IS_ROOT" = 1 ] && echo 'rootfs missing' || echo 'needs root')"
elif [ ! -d /sys/fs/cgroup ] || [ ! -e /sys/fs/cgroup/cgroup.controllers ]; then
    skip "cgroup --memory" "no cgroup v2"
    skip "cgroup --pids"   "no cgroup v2"
else
    _run "$BIN" start --memory 64M --pids 20 --rootfs "$ROOTFS" "$C_SLEEP" 60
    CID="$OUT"
    if printf '%s' "$CID" | grep -Eq '^[0-9a-f]{12}$'; then
        track "$CID"
        CGP="$(grep -oE '"cgroup_path":[[:space:]]*"[^"]*"' "$STATE_ROOT/$CID/state.json" \
               | sed -E 's/.*"([^"]*)"$/\1/')"
        if [ "$CGP" ] && [ -d "$CGP" ]; then
            pass "cgroup_path recorded and exists"
            MMAX="$(cat "$CGP/memory.max" 2>/dev/null)"
            ck_eq "memory.max == 64M (67108864)"      "$MMAX" "67108864"
            PMAX="$(cat "$CGP/pids.max" 2>/dev/null)"
            ck_eq "pids.max == 20"                      "$PMAX" "20"

            # ENFORCEMENT, not just file values (Error #25 regression).
            # The child must be placed in its cgroup BEFORE it runs, so:
            #  (a) /proc/<pid>/cgroup names minicontainer_<id>, not the
            #      launcher's cgroup, and
            #  (b) memory.current accounts the process (was 0 pre-fix,
            #      because pages faulted in the parent cgroup).
            sleep 1
            CPID="$(cat "$STATE_ROOT/$CID/pidfile" 2>/dev/null)"
            PCG="$(cat /proc/$CPID/cgroup 2>/dev/null)"
            ck_contains "child placed in its own cgroup (Error #25)" "$PCG" "minicontainer_"
            MCUR="$(cat "$CGP/memory.current" 2>/dev/null)"
            if [ "${MCUR:-0}" -gt 0 ]; then
                pass "memory.current accounts the placed process (Error #25): ${MCUR}B"
            else
                fail "memory.current accounts the placed process (Error #25)" "memory.current=${MCUR:-<unset>} (0 ⇒ child ran outside its cgroup)"
            fi
        else
            fail "cgroup_path recorded and exists" "path=[$CGP]"
            skip "memory.max check" "no cgroup path"
            skip "pids.max check"   "no cgroup path"
            skip "cgroup placement (Error #25)" "no cgroup path"
            skip "memory.current accounting (Error #25)" "no cgroup path"
        fi
        "$BIN" stop "$CID" >/dev/null 2>&1
    else
        skip "cgroup --memory" "start failed: $ERR"
        skip "cgroup --pids"   "start failed"
    fi
fi

# ===========================================================================
section "13. Network state serialization (--net, root)"
# ===========================================================================
if [ "$IS_ROOT" != "1" ] || [ "$ROOTFS_OK" != "1" ]; then
    skip "net veth in state.json" "$([ "$IS_ROOT" = 1 ] && echo 'rootfs missing' || echo 'needs root')"
elif ! command -v ip >/dev/null 2>&1; then
    skip "net veth in state.json" "host 'ip' not found"
else
    _run "$BIN" start --net --rootfs "$ROOTFS" "$C_SLEEP" 60
    NID="$OUT"
    if printf '%s' "$NID" | grep -Eq '^[0-9a-f]{12}$'; then
        track "$NID"
        _run "$BIN" inspect "$NID"
        ck_contains "inspect: network namespace true"  "$OUT" '"network": true'
        ck_contains "inspect: veth host_ip default"    "$OUT" '10.0.0.1'
        ck_contains "state.json: veth block present"    "$(cat "$STATE_ROOT/$NID/state.json")" '"veth"'
        "$BIN" stop "$NID" >/dev/null 2>&1
    else
        skip "net veth in state.json" "start --net failed: $ERR"
    fi
fi

# ===========================================================================
section "14. exec into a running container (root + rootfs)"
# ===========================================================================
if [ "$IS_ROOT" != "1" ] || [ "$ROOTFS_OK" != "1" ]; then
    skip "exec joins namespaces" "$([ "$IS_ROOT" = 1 ] && echo 'rootfs missing' || echo 'needs root')"
else
    # Target a plain (non-user-ns) container — the common case.  exec
    # now skips joining any namespace it already shares with the caller
    # (decisions.md Error #23), so the inherited user ns no longer
    # EINVALs the setns loop.  --user is deliberately NOT used: it can't
    # start under this host's AppArmor unprivileged-userns policy, and
    # exec must work without it.
    _run "$BIN" start --pid --hostname execbox --rootfs "$ROOTFS" "$C_SLEEP" 120
    XID="$OUT"
    if printf '%s' "$XID" | grep -Eq '^[0-9a-f]{12}$'; then
        track "$XID"
        # exec hostname → should report the container's UTS hostname,
        # proving setns joined the UTS namespace.
        _run_to 30 "$BIN" exec "$XID" "$C_HOSTNAME"
        ck_eq       "exec exit 0"                    "$RC" "0"
        ck_contains "exec joined UTS ns (hostname)"  "$OUT" "execbox"
        "$BIN" stop "$XID" >/dev/null 2>&1
    else
        skip "exec joins namespaces" "start failed: $ERR"
    fi
fi

# ===========================================================================
section "15. cgroup pids ENFORCEMENT — forks actually denied (Error #25)"
# ===========================================================================
# The pre-fix bug placed the child in its cgroup only AFTER it started,
# so a fork loop escaped the cap. This asserts the limit is *enforced*,
# not merely that pids.max holds the right number.
if [ "$IS_ROOT" != "1" ] || [ "$ROOTFS_OK" != "1" ]; then
    skip "pids enforcement (fork denial)" "$([ "$IS_ROOT" = 1 ] && echo 'rootfs missing' || echo 'needs root')"
elif [ ! -e /sys/fs/cgroup/cgroup.controllers ]; then
    skip "pids enforcement (fork denial)" "no cgroup v2"
else
    # PID 1 forks far more than the cap, then exits.  start() does not
    # reap the cgroup, so pids.events (cumulative denied forks) survives
    # for us to read.  Pure sh builtins + sleep — no `seq` (absent from
    # the minimal rootfs).
    _run "$BIN" start --pids 8 --rootfs "$ROOTFS" \
        "$C_SH" -c 'i=0; while [ $i -lt 40 ]; do sleep 30 & i=$((i+1)); done'
    FID="$OUT"
    if printf '%s' "$FID" | grep -Eq '^[0-9a-f]{12}$'; then
        track "$FID"
        sleep 1
        FCG="$(grep -oE '"cgroup_path":[[:space:]]*"[^"]*"' "$STATE_ROOT/$FID/state.json" \
               | sed -E 's/.*"([^"]*)"$/\1/')"
        PEV="$(cat "$FCG/pids.events" 2>/dev/null | tr '\n' ' ')"
        MAXEV="$(printf '%s' "$PEV" | grep -oE 'max [0-9]+' | grep -oE '[0-9]+' | head -1)"
        # pids.events 'max' is cumulative and survives PID 1's exit, so it
        # is the robust signal that a fork was actually denied (>0 ⇒ the
        # child was in the capped cgroup when it forked). A live
        # pids.current/placement check is in §12 instead, since PID 1
        # here exits right after the loop and the namespace is reaped.
        if [ "${MAXEV:-0}" -ge 1 ]; then
            pass "pids.events records denied forks (max=$MAXEV, cap=8)"
        else
            fail "pids.events records denied forks" "pids.events=[$PEV] (max=0 ⇒ forks escaped the cap)"
        fi
        # Kill any sleeps still in the cgroup so cleanup can rmdir it.
        for p in $(cat "$FCG/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
        "$BIN" stop "$FID" >/dev/null 2>&1
    else
        skip "pids enforcement (fork denial)" "start failed: $ERR"
    fi
fi

# ===========================================================================
section "16. Overlay --overlay end-to-end (Error #24)"
# ===========================================================================
# Pre-fix, every --overlay run aborted with "container_id is required"
# because cmd_run/cmd_start never copied the ID into cfg.container_id.
# The unit test_overlay can't catch this (it sets cfg.container_id
# directly) — only an end-to-end CLI run does.
if [ "$IS_ROOT" != "1" ] || [ "$ROOTFS_OK" != "1" ]; then
    skip "overlay end-to-end" "$([ "$IS_ROOT" = 1 ] && echo 'rootfs missing' || echo 'needs root')"
else
    _run_to 30 "$BIN" run --overlay --rootfs "$ROOTFS" \
        "$C_SH" -c 'echo cow-marker > /ovtest.txt; cat /ovtest.txt'
    ck_eq       "overlay run exit 0"                         "$RC" "0"
    ck_contains "overlay: write+read inside works"           "$OUT" "cow-marker"
    ck_lacks    "overlay: no 'container_id is required' (Error #24)" "$ERR" "container_id is required"
    ck_nofile   "overlay: host rootfs untouched (COW)"       "$ROOTFS/ovtest.txt"
fi

# ===========================================================================
section "Summary"
# ===========================================================================
printf "\n  total %d   ${G}pass %d${Z}   ${R}fail %d${Z}   ${Y}skip %d${Z}\n" \
    "$TESTS" "$PASS" "$FAIL" "$SKIP"
if [ "$FAIL" -gt 0 ]; then
    printf "\n  ${R}failed tests:${Z}\n"
    for n in "${FAILED_NAMES[@]}"; do printf "    - %s\n" "$n"; done
    exit 1
fi
printf "\n  ${G}all good${Z}\n"
exit 0
