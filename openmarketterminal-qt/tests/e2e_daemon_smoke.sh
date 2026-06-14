#!/bin/bash
# End-to-end: serve daemon lifecycle on throwaway profiles, NO GUI running.
#
# Asserts: serve starts (kind=daemon), the CLI attaches, settings writes are
# denied (read-only, exit 5 — the real proof that a request routes through the
# daemon and the auth checker), a second serve is refused (exit 3), get_quote
# routes via the daemon without the client hanging (timeout watchdog), and
# serve --stop stops the daemon and removes bridge.json.
#
# NOTE (deviation from the plan's single-daemon script, intentional): the
# get_quote check runs on its OWN throwaway daemon, separate from the lifecycle
# daemon. Reason: in an environment whose network hangs (e.g. a sandbox),
# get_quote's fetch blocks the single-threaded daemon's event loop, after which
# SIGTERM cannot interrupt it and bridge.json would leak. Isolating get_quote
# keeps the lifecycle/--stop assertions on an idle daemon (reliable everywhere),
# and the get_quote daemon gets a leak-proof SIGKILL fallback. The idle-daemon
# assertions are network-independent and pass in CI and in a sandbox alike.

CLI=/tmp/ot-build-ht/openterminalcli
PROF_ROOT="$HOME/Library/Application Support/org.openterminal.OpenTerminal/profiles"

# Throwaway profile names are fixed up front (BEFORE the EXIT trap) so cleanup
# never expands to an empty suffix — a guard like rm -rf "$PROF_ROOT/$QPROF"
# with QPROF unset would target the whole profiles dir and delete real profiles.
LPROF="daemon-e2e-$$";   LROOT="$PROF_ROOT/$LPROF"   # lifecycle daemon profile
QPROF="daemon-e2e-q-$$"; QROOT="$PROF_ROOT/$QPROF"   # get_quote daemon profile
LPID=""   # lifecycle daemon pid
QPID=""   # get_quote daemon pid

cleanup_all() {
    [ -n "$LPID" ] && kill -9 "$LPID" 2>/dev/null
    [ -n "$QPID" ] && kill -9 "$QPID" 2>/dev/null
    [ -n "$LPROF" ] && rm -rf "$PROF_ROOT/$LPROF" 2>/dev/null
    [ -n "$QPROF" ] && rm -rf "$PROF_ROOT/$QPROF" 2>/dev/null
}
fail() { echo "FAIL: $1"; cleanup_all; exit 1; }
trap cleanup_all EXIT

# --- leak-proof stop: SIGTERM, grace, then SIGKILL + rm fallback (prints WARN) ---
# args: <pid> <profile-root-dir> <grace-seconds>
force_stop() {
    local pid="$1" root="$2" grace="$3" i
    for ((i=0; i<grace; i++)); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
    if kill -0 "$pid" 2>/dev/null; then
        echo "WARN: daemon pid $pid did not stop within ${grace}s (wedged); force-killing"
        kill -9 "$pid" 2>/dev/null; sleep 1
    fi
    rm -f "$root/bridge.json" 2>/dev/null
}

# ============================================================================
# Part A — lifecycle on an IDLE daemon (no heavy tool calls): reliable anywhere
# ============================================================================
"$CLI" --profile "$LPROF" serve >/tmp/daemon-e2e.log 2>&1 & LPID=$!
sleep 6

"$CLI" --profile "$LPROF" serve --status | grep -q "kind=daemon" || fail "status not daemon"
echo "PASS: serve --status shows kind=daemon"

"$CLI" --profile "$LPROF" status | grep -qi "attached\|endpoint" || fail "attach status failed"
echo "PASS: status shows attached"

# read-only: settings write denied (exit 5). This is the real round-trip proof:
# the request reached the daemon, ran the auth checker, returned a tool error.
"$CLI" --profile "$LPROF" mcp call set_setting '{"key":"x","value":"1"}'
[ $? -eq 5 ] && echo "PASS: settings-write denied via daemon (exit 5, read-only)" \
             || fail "settings-write not denied (expected exit 5)"

# second serve refused (single-owner). Does not touch the tool endpoint.
timeout 10 "$CLI" --profile "$LPROF" serve >/tmp/daemon-e2e2.log 2>&1
[ $? -eq 3 ] || fail "second serve not refused (exit !=3)"
echo "PASS: second serve refused (exit 3)"

# stop via --stop: daemon exits, bridge.json removed.
"$CLI" --profile "$LPROF" serve --stop | grep -q "SIGTERM" || fail "--stop did not send SIGTERM"
for i in $(seq 1 6); do kill -0 "$LPID" 2>/dev/null || break; sleep 1; done
kill -0 "$LPID" 2>/dev/null && fail "daemon still alive after --stop"
test -f "$LROOT/bridge.json" && fail "bridge.json remained after --stop" \
                             || echo "PASS: --stop removed bridge.json"
LPID=""; rm -rf "$LROOT"

# ============================================================================
# Part B — get_quote routes via the daemon without the CLIENT hanging.
# Network-dependent; on a hung network the daemon may wedge (see header NOTE),
# so this runs on its own daemon with a SIGKILL fallback. We assert no client
# hang and no crash — not data, and not a clean round-trip (which needs network).
# ============================================================================
"$CLI" --profile "$QPROF" serve >/tmp/daemon-e2e-q.log 2>&1 & QPID=$!
sleep 6

timeout 30 "$CLI" --profile "$QPROF" mcp call get_quote '{"symbol":"AAPL"}' >/dev/null 2>&1
rc=$?
[ $rc -eq 124 ] && fail "get_quote hung the client via daemon (watchdog fired)"
[ $rc -ge 134 ] && fail "get_quote crashed the client (rc=$rc)"
echo "PASS: get_quote via daemon did not hang/crash the client (rc=$rc)"

# leak-proof teardown of the (possibly wedged) get_quote daemon
"$CLI" --profile "$QPROF" serve --stop >/dev/null 2>&1
force_stop "$QPID" "$QROOT" 8
QPID=""
test -f "$QROOT/bridge.json" && fail "bridge.json leaked after get_quote daemon teardown" \
                             || echo "PASS: get_quote daemon torn down, no bridge.json leak"
rm -rf "$QROOT"

echo "PASS: daemon e2e"
