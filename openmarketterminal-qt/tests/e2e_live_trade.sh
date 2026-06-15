#!/bin/bash
# End-to-end: the Phase-C LIVE-mode AI-trading constitution over the SERVE DAEMON,
# NO GUI, NO real credentials. This is the integration proof that the live gate
# stack is ACTIVE and DEFAULT-DENY when a human armed live but the daemon has no
# usable broker account.
#
# A human flips the GUI-only live constitution (modeled here by a DIRECT sqlite
# write of the cli.* keys — the tool path REFUSES cli.* writes, that is the
# KEYSTONE, so a direct DB write is the only way, exactly as the GUI does it):
#   - cli.allow_trading      = true   (trading permitted)
#   - cli.live_trading_armed = true   (live arming flipped on)
#   - cli.allowed_account    = ''     (NO account named → default-deny)
#   - cli.kill_switch        = false  (panic button off)
# then an AI client drives the order flow over the daemon.
#
# IMPORTANT — the daemon has NO live broker account and NO credentials, so we do
# NOT (and CANNOT) exercise a real fill here. The AUTHORITATIVE happy-path live
# fill proof is the UNIT TEST (tst_live_trading::live_happy_path_fills_via_fake_broker,
# FakeBroker sandbox). Here we HARD-assert ONLY the deterministic, network-free,
# credential-free constitution that the gate stack enforces.
#
# HARD asserts (deterministic, no network / no creds):
#   1. DIRECT live tool DENIED even when armed: live_place_order over the daemon
#      is denied at the auth checker (exit 5 + "auth") — is_live_execution_tool
#      never bypasses the gated submit_order path. The AI's ONLY live path is
#      submit_order. (Mirrors unit live_direct_tool_denied_even_when_armed.)
#   2. submit_order LIVE with NO allowed account → REJECTED. Arming passes the
#      auth checker (allow_trading && live_armed), so the request reaches the
#      HANDLER, whose gate stack rejects it: status "rejected", reason mentions
#      "no allowed account" (default-deny; NO order placed). (Unit (b).)
#   3. KILL SWITCH halts: with cli.kill_switch=true, prepare_order over the daemon
#      is rejected "kill switch engaged" (the panic button short-circuits at the
#      handler top, BEFORE any gate). Reset to false after. (Unit layer 2.)
#   4. KEYSTONE: set_setting on each constitution key (cli.allow_trading,
#      cli.kill_switch, cli.allowed_account) over the daemon is DENIED
#      (exit 5 + "auth") — the read-only gate blocks the constitution from being
#      rewritten by a tool, even with trading armed.
#   5. submit_order LIVE is GATED, not hard-off: the live submit returns a
#      STRUCTURED gate rejection — NOT the removed Phase-A hard-off string
#      "live trading disabled". Proven by reusing #2's output and asserting the
#      absence of that text alongside the "no allowed account" gate reason.
#
# Mirrors tests/e2e_paper_trade.sh: fixed-up-front throwaway profile name,
# cleanup_all/fail/force_stop, EXIT trap, and a `timeout` watchdog on every
# daemon-routed call so a wedge FAILS loudly instead of hanging. Leak-safe: never
# rm an unset path; SIGKILL fallback; profiles.json manifest entry removed.
#
# DB note: the settings table lives in <profile>/data/openmarketterminal.db (the
# main DB), NOT cache.db — target that file by name and verify the schema before
# writing (fail loudly if the layout changed). Confirmed cols: key,value,category,
# updated_at.

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
PROF="live-e2e-$$"
ROOT="$PROF_ROOT/$PROF"
PID=""

cleanup_all() {
    [ -n "$PID" ] && kill -9 "$PID" 2>/dev/null
    [ -n "$PROF" ] && rm -rf "$PROF_ROOT/$PROF" 2>/dev/null
    # Remove our entry from the default profile manifest so the registry is left
    # exactly as found (mirrors e2e_paper_trade.sh's cleanup).
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

[ -x "$CLI" ] || fail "CLI not found/executable at $CLI"

echo "== openterminalcli LIVE-trade e2e (over the serve daemon, Phase C) =="
echo "CLI=$CLI  profile=$PROF  watchdog=${TIMEOUT:-none}"
echo "NOTE: no live account / no creds — only deterministic gate-stack asserts."
echo

# ============================================================================
# Step 1 — bootstrap the profile DB (migrations run), then SIMULATE a human who
# ARMED live but configured NO broker account (cli.allowed_account empty).
# ============================================================================
wd 60 "$CLI" --headless --profile "$PROF" mcp list >/dev/null 2>&1 \
    || fail "bootstrap: headless mcp list failed (DB not created/migrated)"

DB="$ROOT/data/openmarketterminal.db"
[ -f "$DB" ] || DB=$(find "$ROOT" -name 'openmarketterminal.db' 2>/dev/null | head -1)
[ -n "$DB" ] && [ -f "$DB" ] || fail "could not locate openmarketterminal.db under $ROOT"
echo "PASS: profile DB bootstrapped -> $DB"

# Confirm the settings schema before writing (fail loudly if the layout changed).
SCHEMA=$(sqlite3 "$DB" ".schema settings" 2>&1)
printf '%s' "$SCHEMA" | grep -qi "CREATE TABLE settings" \
    || fail "settings table missing/changed in $DB -> $SCHEMA"
echo "PASS: settings schema present (key,value,category,updated_at)"

# Simulated GUI constitution. Done BEFORE `serve` so the daemon reads it live.
# Armed for live, but NO allowed account → default-deny. Kill switch off.
sqlite3 "$DB" \
  "INSERT OR REPLACE INTO settings(key,value,category,updated_at) VALUES('cli.allow_trading','true','cli','2026-01-01'),('cli.live_trading_armed','true','cli','2026-01-01'),('cli.allowed_account','','cli','2026-01-01'),('cli.kill_switch','false','cli','2026-01-01');" \
    || fail "sqlite write of live constitution failed"
[ "$(sqlite3 "$DB" "SELECT value FROM settings WHERE key='cli.allow_trading';")" = "true" ] \
    || fail "allow_trading write did not land in DB"
[ "$(sqlite3 "$DB" "SELECT value FROM settings WHERE key='cli.live_trading_armed';")" = "true" ] \
    || fail "live_trading_armed write did not land in DB"
echo "PASS: simulated GUI constitution written (live ARMED; NO allowed account; kill OFF)"

# ============================================================================
# Step 2 — start the daemon; assert kind=daemon.
# ============================================================================
"$CLI" --profile "$PROF" serve >/tmp/live-e2e.log 2>&1 & PID=$!
sleep 6
wd 10 "$CLI" --profile "$PROF" serve --status | grep -q "kind=daemon" \
    || fail "serve --status did not report kind=daemon (log: $(cat /tmp/live-e2e.log 2>/dev/null))"
echo "PASS: serve --status shows kind=daemon"

# ============================================================================
# HARD ASSERT 1 — DIRECT live tool DENIED even when armed. live_place_order over
# the daemon is denied at the auth checker (exit 5 + auth) — is_live_execution_tool
# never bypasses the gated submit_order path, even with trading fully armed.
# ============================================================================
D_OUT=$(wd 20 "$CLI" --profile "$PROF" mcp call live_place_order \
        '{"account_id":"x","symbol":"AAPL","side":"buy","quantity":1}' 2>&1)
D_RC=$?
{ [ $D_RC -eq 5 ] && printf '%s' "$D_OUT" | grep -qi "auth"; } \
    && echo "PASS: #1 direct live_place_order DENIED even when armed (exit 5 + auth) -> $D_OUT" \
    || fail "#1 direct live tool not denied (rc=$D_RC out=$D_OUT)"

# ============================================================================
# HARD ASSERT 2 — submit_order LIVE with NO allowed account → REJECTED by the
# handler gate stack. Prepare a valid equity LIMIT draft (deterministic price),
# then submit live. Arming passes the auth checker, so the request reaches the
# HANDLER; default-deny (no allowed account) → status "rejected", reason mentions
# "no allowed account". NO order placed. (Also feeds HARD ASSERT 5.)
# ============================================================================
P_OUT=$(wd 30 "$CLI" --json --profile "$PROF" mcp call prepare_order \
        '{"symbol":"AAPL","side":"buy","quantity":1,"order_type":"limit","limit_price":100}' 2>&1)
P_RC=$?
[ $P_RC -eq 0 ] || fail "#2 prepare_order over daemon rc=$P_RC out=$P_OUT"
DID=$(printf '%s' "$P_OUT" | json_field draft_id)
[ "$(printf '%s' "$P_OUT" | json_field status)" = "prepared" ] \
    || fail "#2 prepare_order status != prepared out=$P_OUT"
[ -n "$DID" ] || fail "#2 prepare_order returned empty draft_id out=$P_OUT"
echo "PASS: #2a prepare_order via daemon -> status=prepared draft_id=$DID"

L_OUT=$(wd 30 "$CLI" --json --profile "$PROF" mcp call submit_order \
        "{\"draft_id\":\"$DID\",\"mode\":\"live\"}" 2>&1)
L_RC=$?
L_STATUS=$(printf '%s' "$L_OUT" | json_field status)
L_REASON=$(printf '%s' "$L_OUT" | json_field reason)
# A live FILL would also be success:true — discriminate on status/reason. A fill
# here would mean live executed with no account: hard fail.
[ "$L_STATUS" = "filled" ] && fail "#2 submit_order LIVE FILLED with no allowed account — live executed! out=$L_OUT"
{ [ $L_RC -eq 0 ] && [ "$L_STATUS" = "rejected" ] \
    && printf '%s' "$L_REASON" | grep -qi "no allowed account"; } \
    && echo "PASS: #2b submit_order LIVE rejected by gate stack -> '$L_REASON'" \
    || fail "#2 submit_order LIVE not rejected for no-allowed-account (rc=$L_RC status=$L_STATUS out=$L_OUT)"
# Non-execution proof: the draft NEVER walked to submitted (still prepared).
D_DB=$(sqlite3 "$DB" "SELECT status FROM order_drafts WHERE draft_id='$DID';")
[ "$D_DB" = "prepared" ] \
    || fail "#2 live draft walked away from prepared (got '$D_DB') — live may have executed!"
echo "PASS: #2c live draft remained 'prepared' in DB (live never executed)"

# ============================================================================
# HARD ASSERT 5 — submit_order LIVE is GATED, not hard-off. The rejection above
# is a STRUCTURED gate reason; the removed Phase-A hard-off text "live trading
# disabled" must NOT appear. Asserting its absence makes "the hard-off is gone,
# the gate stack replaced it" airtight rather than implied.
# ============================================================================
if printf '%s' "$L_OUT" | grep -qi "live trading disabled"; then
    fail "#5 submit_order LIVE returned the REMOVED hard-off text (gate stack bypassed?): $L_OUT"
fi
echo "PASS: #5 live submit is GATED (structured reason; no 'live trading disabled' hard-off text)"

# ============================================================================
# HARD ASSERT 3 — KILL SWITCH halts. Set cli.kill_switch=true (direct DB write;
# the daemon re-reads settings live, no cache), then prepare_order over the
# daemon is rejected "kill switch engaged" (short-circuits at the handler top).
# Reset to false afterwards.
# ============================================================================
sqlite3 "$DB" \
  "INSERT OR REPLACE INTO settings(key,value,category,updated_at) VALUES('cli.kill_switch','true','cli','2026-01-01');" \
    || fail "#3 sqlite write of kill_switch=true failed"
K_OUT=$(wd 30 "$CLI" --json --profile "$PROF" mcp call prepare_order \
        '{"symbol":"AAPL","side":"buy","quantity":1,"order_type":"market"}' 2>&1)
K_RC=$?
K_STATUS=$(printf '%s' "$K_OUT" | json_field status)
K_REASON=$(printf '%s' "$K_OUT" | json_field reason)
{ [ $K_RC -eq 0 ] && [ "$K_STATUS" = "rejected" ] \
    && printf '%s' "$K_REASON" | grep -qi "kill switch engaged"; } \
    && echo "PASS: #3 kill switch HALTS prepare_order -> '$K_REASON'" \
    || fail "#3 kill switch did not halt prepare_order (rc=$K_RC status=$K_STATUS out=$K_OUT)"
# Reset kill switch off so it does not contaminate later assertions.
sqlite3 "$DB" \
  "INSERT OR REPLACE INTO settings(key,value,category,updated_at) VALUES('cli.kill_switch','false','cli','2026-01-01');" \
    || fail "#3 sqlite reset of kill_switch=false failed"
[ "$(sqlite3 "$DB" "SELECT value FROM settings WHERE key='cli.kill_switch';")" = "false" ] \
    || fail "#3 kill_switch reset did not land in DB"
echo "PASS: #3 kill switch reset to false"

# ============================================================================
# HARD ASSERT 4 — KEYSTONE. set_setting on EACH constitution key over the daemon
# is DENIED (exit 5 + auth) even with trading armed — the read-only gate blocks
# the constitution from being rewritten by a tool. An agent must never be able to
# enable trading, disengage the kill switch, or name its own allowed account.
# ============================================================================
keystone_denied() {
    local key="$1" val="$2" out rc
    out=$(wd 20 "$CLI" --profile "$PROF" mcp call set_setting \
          "{\"key\":\"$key\",\"value\":\"$val\"}" 2>&1)
    rc=$?
    { [ $rc -eq 5 ] && printf '%s' "$out" | grep -qi "auth"; } \
        && echo "PASS: #4 keystone — set_setting $key DENIED via daemon (exit 5 + auth) -> $out" \
        || fail "#4 keystone: set_setting $key not denied (rc=$rc out=$out)"
}
keystone_denied "cli.allow_trading"    "true"
keystone_denied "cli.kill_switch"      "false"
keystone_denied "cli.allowed_account"  "x"

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
echo "PASS: LIVE-trade e2e"
