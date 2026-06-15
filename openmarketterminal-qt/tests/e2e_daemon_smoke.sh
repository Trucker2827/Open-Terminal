#!/bin/bash
# End-to-end: serve daemon lifecycle on throwaway profiles, NO GUI running.
#
# Asserts: serve starts (kind=daemon), the CLI attaches, settings writes AND
# destructive tools are denied (read-only, exit 5 + "requires ... auth" reason —
# the real proof that a request routes through the daemon and the auth checker),
# a second serve is refused (exit 3), get_quote routes via the daemon without the
# client hanging (timeout watchdog), serve --stop stops the daemon and removes
# bridge.json, AND — the core proof of off-thread dispatch — the daemon stays
# CONTROLLABLE (serve --status / serve --stop both work) while a tool is in
# flight on the bridge's worker thread (Part C).
#
# NOTE on the two extra throwaway daemons (intentional): get_quote runs on its
# own daemon (Part B) and the under-load check on yet another (Part C), separate
# from the idle lifecycle daemon (Part A).
#   - Part A (idle daemon) is network-independent and passes in CI and sandbox
#     alike — it carries the lifecycle/auth/--stop assertions reliably.
#   - Part C is the load-bearing off-thread proof: a background get_quote runs on
#     the bridge worker; the event loop must still answer --status and honor
#     --stop (SIGTERM, escalating to SIGKILL). Pre-off-thread, an in-flight
#     blocking tool would WEDGE the single-threaded loop and SIGTERM could not
#     interrupt it; this part asserts that is no longer true. If the daemon DOES
#     wedge here, that is a real regression of the off-thread fix — the script
#     fails loudly rather than masking it.
#   - Part B (no-client-hang) and Part C each get a leak-proof SIGKILL fallback
#     in teardown so a wedged daemon never leaks bridge.json into cleanup.

CLI=/tmp/ot-build-ht/openterminalcli
PROF_ROOT="$HOME/Library/Application Support/org.openterminal.OpenTerminal/profiles"

# Throwaway profile names are fixed up front (BEFORE the EXIT trap) so cleanup
# never expands to an empty suffix — a guard like rm -rf "$PROF_ROOT/$QPROF"
# with QPROF unset would target the whole profiles dir and delete real profiles.
LPROF="daemon-e2e-$$";   LROOT="$PROF_ROOT/$LPROF"   # lifecycle daemon profile
QPROF="daemon-e2e-q-$$"; QROOT="$PROF_ROOT/$QPROF"   # get_quote daemon profile
RPROF="daemon-e2e-r-$$"; RROOT="$PROF_ROOT/$RPROF"   # under-load (responsiveness) daemon
LPID=""   # lifecycle daemon pid
QPID=""   # get_quote daemon pid
RPID=""   # under-load daemon pid
INFLIGHT="" # background in-flight tool-call client pid (Part C)

cleanup_all() {
    [ -n "$INFLIGHT" ] && kill -9 "$INFLIGHT" 2>/dev/null
    [ -n "$LPID" ] && kill -9 "$LPID" 2>/dev/null
    [ -n "$QPID" ] && kill -9 "$QPID" 2>/dev/null
    [ -n "$RPID" ] && kill -9 "$RPID" 2>/dev/null
    [ -n "$LPROF" ] && rm -rf "$PROF_ROOT/$LPROF" 2>/dev/null
    [ -n "$QPROF" ] && rm -rf "$PROF_ROOT/$QPROF" 2>/dev/null
    [ -n "$RPROF" ] && rm -rf "$PROF_ROOT/$RPROF" 2>/dev/null
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
# Exit 5 alone is overloaded (a handler failure also exits 5), so we ALSO assert
# the denial reason ("requires ... auth") to prove the read-only auth gate fired
# rather than the tool merely failing for some other reason.
SS_ERR=$("$CLI" --profile "$LPROF" mcp call set_setting '{"key":"x","value":"1"}' 2>&1); SS_RC=$?
{ [ $SS_RC -eq 5 ] && printf '%s' "$SS_ERR" | grep -qi "auth"; } \
    && echo "PASS: settings-write denied via daemon (exit 5, read-only gate) -> $SS_ERR" \
    || fail "settings-write not denied by read-only gate (rc=$SS_RC msg=$SS_ERR)"

# read-only also denies DESTRUCTIVE tools (exit 5 + denial reason). Same gate,
# the is_destructive branch — proves a write/trade tool can't run via the daemon.
LC_ERR=$("$CLI" --profile "$LPROF" mcp call live_cancel_all_orders '{}' 2>&1); LC_RC=$?
{ [ $LC_RC -eq 5 ] && printf '%s' "$LC_ERR" | grep -qi "auth"; } \
    && echo "PASS: destructive live_cancel_all_orders denied via daemon (exit 5) -> $LC_ERR" \
    || fail "destructive tool not denied by read-only gate (rc=$LC_RC msg=$LC_ERR)"

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

# ============================================================================
# Part C — daemon stays CONTROLLABLE while a (possibly slow/cold) tool is in
# flight. THE core proof of off-thread dispatch: a background get_quote runs on
# the bridge's worker thread; the event loop must still answer serve --status
# within ~3s AND honor serve --stop within ~8s (SIGTERM, escalating to SIGKILL),
# removing bridge.json. Pre-off-thread, the in-flight tool would wedge the loop.
# ============================================================================
"$CLI" --profile "$RPROF" serve >/tmp/daemon-e2e-r.log 2>&1 & RPID=$!
sleep 6

# Fire a tool in the BACKGROUND; it may be slow/cold. We do not care about its
# result/exit code (network may make it rc 0 or 5) — only that it is IN FLIGHT
# on the worker while we drive the daemon.
timeout 60 "$CLI" --profile "$RPROF" mcp call get_quote '{"symbol":"AAPL"}' >/dev/null 2>&1 &
INFLIGHT=$!
sleep 1   # let the call reach the worker thread

# --status must answer despite the in-flight tool (watchdog: a wedged loop fires
# the timeout -> grep fails -> fail). Headroom 5s covers CLI launch overhead.
timeout 5 "$CLI" --profile "$RPROF" serve --status | grep -q "kind=daemon" \
    || fail "serve --status UNRESPONSIVE while a tool is in flight (loop wedged)"
echo "PASS: serve --status responsive during in-flight tool"

# --stop must stop the daemon under load (clean SIGTERM, or force-stopped via
# SIGKILL if shutdown drains the worker on a hung network — both are acceptable;
# the gate is "the daemon stopped", not which signal did it). Headroom 10s.
timeout 10 "$CLI" --profile "$RPROF" serve --stop | grep -qiE "SIGTERM|force-stopped" \
    || fail "serve --stop UNRESPONSIVE while a tool is in flight"
for i in $(seq 1 8); do kill -0 "$RPID" 2>/dev/null || break; sleep 1; done
kill -0 "$RPID" 2>/dev/null && fail "daemon still alive after --stop under load (wedged)"
sleep 1
test -f "$RROOT/bridge.json" && fail "bridge.json remained after --stop under load" \
    || echo "PASS: --stop stopped daemon + removed bridge.json under in-flight tool"
kill -9 "$INFLIGHT" 2>/dev/null; INFLIGHT=""
RPID=""; rm -rf "$RROOT"

echo "PASS: daemon e2e"
