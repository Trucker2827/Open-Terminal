#!/bin/bash
# End-to-end: the Phase-D FAST LIVE MODE constitution over the SERVE DAEMON,
# NO GUI, NO real credentials. This is the integration proof that the SECOND
# fast arm (cli.fast_live_armed) gates the 7 fast-live tools end to end, and
# that the gate stack is ACTIVE and DEFAULT-DENY when a human armed fast-live
# but the daemon has no usable broker account.
#
# A human flips the GUI-only constitution (modeled here by a DIRECT sqlite write
# of the cli.* keys — the tool path REFUSES cli.* writes, that is the KEYSTONE,
# so a direct DB write is the only way, exactly as the GUI does it). We drive the
# daemon through THREE constitutional phases:
#
#   Phase A — base trading + base live arm ON, fast arm OFF:
#       cli.allow_trading=true, cli.live_trading_armed=true,
#       cli.fast_live_armed=false, cli.kill_switch=false.
#     The destructive fast-live tools (cancel_order/fast_submit_order/
#     replace_order/exit_position) are DENIED at the host auth-checker (exit 5 +
#     "auth") — the SECOND fast arm is the gate, and it is off. The raw live
#     adapter (live_place_order) is denied too, and the KEYSTONE set_setting on
#     cli.fast_live_armed is denied.
#
#   Phase B — flip the fast arm ON, still NO allowed account:
#       cli.fast_live_armed=true, cli.allowed_account='' (kill still false).
#     Arming passes the host auth-checker, so destructive fast tools reach the
#     HANDLER, whose in-handler fast_live_gate rejects them: status "rejected",
#     reason "no allowed account" (default-deny; NO order placed; the call itself
#     succeeds = ok_data, rc 0). The 3 non-destructive READS (get_positions/
#     get_open_orders/get_fills) have no auth-checker installed at all, so they
#     ALSO reach the handler and get the same "no allowed account" rejection. The
#     raw live adapter STAYS denied (exit 5) even with the fast arm on — proving
#     fast-arm never unlocks the raw path.
#
#   Phase C — kill switch dominates:
#       cli.kill_switch=true (fast arm still on).
#     fast_submit_order passes the auth-checker (the checker predicate is the
#     three arms, NOT the kill switch) and reaches the handler, where the kill
#     switch short-circuits fast_live_gate FIRST: rejected "kill switch engaged"
#     (rc 0). Reset to false afterwards so it cannot contaminate teardown.
#
# IMPORTANT — the daemon has NO live broker account and NO credentials, so we do
# NOT (and CANNOT) exercise a real fast fill here. The AUTHORITATIVE happy-path
# fast-fill / risk-floor / daily-loss proofs are the UNIT TEST (tst_fast_live,
# FakeBroker sandbox, neuter-proven floor: broker place_calls/modify_calls==0).
# Here we HARD-assert ONLY the deterministic, network-free, credential-free
# constitution that the gate stack enforces.
#
# Mirrors tests/e2e_live_trade.sh: fixed-up-front throwaway profile name,
# cleanup_all/fail/force_stop, EXIT trap, and a `timeout` watchdog on every
# daemon-routed call so a wedge FAILS loudly instead of hanging. Leak-safe: never
# rm an unset path; SIGKILL fallback; profiles.json manifest entry removed.
#
# DB note: the settings table lives in <profile>/data/openmarketterminal.db (the
# main DB), NOT cache.db. Confirmed cols: key,value,category,updated_at.

CLI=/tmp/ot-build-ht/openterminalcli
APP_ROOT="$HOME/Library/Application Support/org.openterminal.OpenTerminal"
PROF_ROOT="$APP_ROOT/profiles"

# Watchdog: prefer timeout, fall back to gtimeout (macOS), else none (warn).
if   command -v timeout  >/dev/null 2>&1; then TIMEOUT="timeout"
elif command -v gtimeout >/dev/null 2>&1; then TIMEOUT="gtimeout"
else TIMEOUT=""; echo "WARN: no timeout/gtimeout — running WITHOUT a hang watchdog"; fi
wd() { local s="$1"; shift; if [ -n "$TIMEOUT" ]; then "$TIMEOUT" "$s" "$@"; else "$@"; fi; }

# Throwaway profile name fixed up front (BEFORE the EXIT trap) so cleanup never
# expands to an empty suffix and rm's the whole profiles dir.
PROF="fast-live-e2e-$$"
ROOT="$PROF_ROOT/$PROF"
PID=""

cleanup_all() {
    [ -n "$PID" ] && kill -9 "$PID" 2>/dev/null
    [ -n "$PROF" ] && rm -rf "$PROF_ROOT/$PROF" 2>/dev/null
    # Remove our entry from the default profile manifest so the registry is left
    # exactly as found (mirrors e2e_live_trade.sh's cleanup).
    local mf="$APP_ROOT/profiles.json"
    if [ -n "$PROF" ] && [ -f "$mf" ] && command -v python3 >/dev/null 2>&1; then
        python3 - "$mf" "$PROF" <<'PY' 2>/dev/null || true
import json, sys
mf, prof = sys.argv[1], sys.argv[2]
try:
    d = json.load(open(mf))
    d["profiles"] = [p for p in d.get("profiles", []) if p.get("name") != prof]
    json.dump(d, open(mf, "w"), indent=4)
except Exception:
    pass
PY
    fi
}
fail() { echo "FAIL: $1"; cleanup_all; exit 1; }
trap cleanup_all EXIT

# leak-proof stop: SIGTERM grace, then SIGKILL + rm fallback (prints WARN).
force_stop() {
    local pid="$1" root="$2" grace="$3" i
    for ((i=0; i<grace; i++)); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
    if kill -0 "$pid" 2>/dev/null; then
        echo "WARN: daemon pid $pid did not stop within ${grace}s (wedged); force-killing"
        kill -9 "$pid" 2>/dev/null; sleep 1
    fi
    rm -f "$root/bridge.json" 2>/dev/null
}

# extract a JSON field from {"data":{...},"success":...} stdout via python3.
json_field() { python3 -c 'import sys,json
try:
    d=json.load(sys.stdin)
    print(d.get("data",{}).get(sys.argv[1],""))
except Exception:
    print("")' "$1"; }

# Set one cli.* settings key directly in the DB (the GUI-only path; the tool path
# refuses cli.* writes — that is the keystone).
set_key() {
    sqlite3 "$DB" \
      "INSERT OR REPLACE INTO settings(key,value,category,updated_at) VALUES('$1','$2','cli','2026-01-01');" \
        || fail "sqlite write of $1=$2 failed"
    [ "$(sqlite3 "$DB" "SELECT value FROM settings WHERE key='$1';")" = "$2" ] \
        || fail "$1=$2 write did not land in DB"
}

[ -x "$CLI" ] || fail "CLI not found/executable at $CLI"

echo "== openterminalcli FAST-LIVE e2e (over the serve daemon, Phase D) =="
echo "CLI=$CLI  profile=$PROF  watchdog=${TIMEOUT:-none}"
echo "NOTE: no live account / no creds — only deterministic gate-stack asserts."
echo

# ============================================================================
# Step 1 — bootstrap the profile DB (migrations run), confirm settings schema.
# ============================================================================
wd 60 "$CLI" --headless --profile "$PROF" mcp list >/dev/null 2>&1 \
    || fail "bootstrap: headless mcp list failed (DB not created/migrated)"

DB="$ROOT/data/openmarketterminal.db"
[ -f "$DB" ] || DB=$(find "$ROOT" -name 'openmarketterminal.db' 2>/dev/null | head -1)
[ -n "$DB" ] && [ -f "$DB" ] || fail "could not locate openmarketterminal.db under $ROOT"
echo "PASS: profile DB bootstrapped -> $DB"

SCHEMA=$(sqlite3 "$DB" ".schema settings" 2>&1)
printf '%s' "$SCHEMA" | grep -qi "CREATE TABLE settings" \
    || fail "settings table missing/changed in $DB -> $SCHEMA"
echo "PASS: settings schema present (key,value,category,updated_at)"

# ============================================================================
# PHASE A constitution — base trading + base live arm ON, the SECOND fast arm
# OFF, kill OFF, NO allowed account. Written BEFORE `serve` so the daemon reads
# it live.
# ============================================================================
set_key "cli.allow_trading"      "true"
set_key "cli.live_trading_armed" "true"
set_key "cli.fast_live_armed"    "false"
set_key "cli.allowed_account"    ""
set_key "cli.kill_switch"        "false"
echo "PASS: Phase-A constitution written (live ARMED; fast arm OFF; no account; kill OFF)"

# ============================================================================
# Step 2 — start the daemon; assert kind=daemon.
# ============================================================================
"$CLI" --profile "$PROF" serve >/tmp/fast-live-e2e.log 2>&1 & PID=$!
sleep 6
wd 10 "$CLI" --profile "$PROF" serve --status | grep -q "kind=daemon" \
    || fail "serve --status did not report kind=daemon (log: $(cat /tmp/fast-live-e2e.log 2>/dev/null))"
echo "PASS: serve --status shows kind=daemon"

# An exit-5+auth deny assert: rc MUST be 5 AND output MUST mention "auth".
assert_denied() {
    local label="$1" tool="$2" payload="$3" out rc
    out=$(wd 20 "$CLI" --profile "$PROF" mcp call "$tool" "$payload" 2>&1)
    rc=$?
    { [ $rc -eq 5 ] && printf '%s' "$out" | grep -qi "auth"; } \
        && echo "PASS: $label — $tool DENIED (exit 5 + auth) -> $out" \
        || fail "$label: $tool not denied (rc=$rc out=$out)"
}

# A handler-gate reject assert: rc 0, data.status=rejected, reason has substring.
assert_rejected() {
    local label="$1" tool="$2" payload="$3" needle="$4" out rc status reason
    out=$(wd 30 "$CLI" --json --profile "$PROF" mcp call "$tool" "$payload" 2>&1)
    rc=$?
    status=$(printf '%s' "$out" | json_field status)
    reason=$(printf '%s' "$out" | json_field reason)
    # A "filled"/"cancelled"/"closed"/"replaced" here would mean a live broker
    # action executed with no account — hard fail.
    [ "$status" = "rejected" ] || fail "$label: $tool status='$status' (expected rejected; live may have acted!) out=$out"
    { [ $rc -eq 0 ] && printf '%s' "$reason" | grep -qi "$needle"; } \
        && echo "PASS: $label — $tool rejected by handler gate -> '$reason'" \
        || fail "$label: $tool not rejected with '$needle' (rc=$rc status=$status out=$out)"
}

# ============================================================================
# PHASE A asserts — fast arm OFF.
# ============================================================================
echo "-- Phase A (fast arm OFF) --"
# A1: destructive fast-live tools DENIED at the host checker (the second fast arm
#     is the gate; it is off). Schema-valid payloads so what denies is the gate.
assert_denied "A1" cancel_order      '{"order_id":"X1"}'
assert_denied "A1" fast_submit_order '{"symbol":"AAPL","side":"buy","quantity":1,"order_type":"limit","limit_price":100}'
assert_denied "A1" replace_order     '{"order_id":"X1","symbol":"AAPL","side":"buy","quantity":1,"order_type":"limit","limit_price":100}'
assert_denied "A1" exit_position     '{"symbol":"AAPL"}'
# A2: raw live adapter DENIED regardless of fast-arm.
assert_denied "A2" live_place_order  '{"account_id":"x","symbol":"AAPL","side":"buy","quantity":1}'
# A3: KEYSTONE — set_setting on cli.fast_live_armed DENIED over the daemon.
assert_denied "A3" set_setting       '{"key":"cli.fast_live_armed","value":"true"}'

# ============================================================================
# PHASE B — flip the fast arm ON (still NO allowed account). The daemon re-reads
# settings live (no cache).
# ============================================================================
set_key "cli.fast_live_armed" "true"
echo "-- Phase B (fast arm ON; no allowed account) --"
# B1: destructive fast tools now REACH the handler; gate refuses (no account).
assert_rejected "B1" fast_submit_order '{"symbol":"AAPL","side":"buy","quantity":1,"order_type":"limit","limit_price":100}' "no allowed account"
assert_rejected "B1" cancel_order      '{"order_id":"X1"}' "no allowed account"
assert_rejected "B1" exit_position     '{"symbol":"AAPL"}' "no allowed account"
assert_rejected "B1" replace_order     '{"order_id":"X1","symbol":"AAPL","side":"buy","quantity":1,"order_type":"limit","limit_price":100}' "no allowed account"
# B2: non-destructive READS (no auth-checker installed) also reach the handler.
assert_rejected "B2" get_positions   '{}' "no allowed account"
assert_rejected "B2" get_open_orders '{}' "no allowed account"
assert_rejected "B2" get_fills       '{}' "no allowed account"
# B3: the raw live adapter STAYS denied even with the fast arm ON — fast-arm
#     never unlocks the raw path (the meaningful "regardless of fast-arm" proof).
assert_denied "B3" live_place_order  '{"account_id":"x","symbol":"AAPL","side":"buy","quantity":1}'
# B4: KEYSTONE still holds with the fast arm on.
assert_denied "B4" set_setting       '{"key":"cli.fast_live_armed","value":"false"}'

# ============================================================================
# PHASE C — kill switch dominates. With the fast arm ON, the auth-checker passes
# (its predicate is the three arms, NOT the kill switch), so fast_submit_order
# reaches the handler — where the kill switch short-circuits fast_live_gate FIRST.
# ============================================================================
set_key "cli.kill_switch" "true"
echo "-- Phase C (kill switch ON, fast arm still ON) --"
assert_rejected "C1" fast_submit_order '{"symbol":"AAPL","side":"buy","quantity":1,"order_type":"limit","limit_price":100}' "kill switch engaged"
# Reset kill switch off so it cannot contaminate teardown.
set_key "cli.kill_switch" "false"
echo "PASS: C — kill switch reset to false"

# ============================================================================
# Teardown — serve --stop: daemon exits, bridge.json removed (leak-proof).
# ============================================================================
wd 10 "$CLI" --profile "$PROF" serve --stop | grep -qiE "SIGTERM|force-stopped" \
    || fail "serve --stop did not report SIGTERM/force-stopped"
force_stop "$PID" "$ROOT" 8
kill -0 "$PID" 2>/dev/null && fail "daemon still alive after --stop"
test -f "$ROOT/bridge.json" && fail "bridge.json remained after --stop" \
                            || echo "PASS: --stop stopped daemon + removed bridge.json"
PID=""

echo
echo "PASS: FAST-LIVE e2e"
