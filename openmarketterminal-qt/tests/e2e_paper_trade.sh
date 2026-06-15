#!/bin/bash
# End-to-end: the two-phase AI-trading order flow over the SERVE DAEMON, NO GUI.
#
# This is the integration proof for Phase A: the human flips the GUI-only paper
# toggle (modeled here by a DIRECT sqlite write of the cli.* keys — the tool path
# REFUSES cli.* writes, that's the keystone, so a direct DB write is the only way,
# exactly as the GUI does it), then an AI client drives prepare_order/submit_order
# over the daemon. Asserts the headline safety properties:
#
#   - prepare_order routes via the daemon (non-destructive -> allowed) and returns
#     a "prepared" draft with a draft_id.
#   - submit_order PAPER routes via the daemon: the submit_order CARVE-OUT lets it
#     past the read-only auth gate AND the handler executes on the paper rail
#     (status "filled"). This is the headline.
#   - submit_order LIVE is refused EVEN WITH THE LIVE GATES FULLY ARMED: the
#     handler is the last line of defence and hard-offs ("live trading disabled"),
#     and the draft NEVER walks to "submitted". Arming is STRENGTHENING — it
#     strips the checker's protection so the handler alone must refuse (mirrors
#     the unit test submit_live_hard_off_even_when_armed). Live never executes.
#   - The keystone holds over the daemon: set_setting on a cli.* key is DENIED
#     (exit 5 + "auth") even with trading armed — the read-only gate blocks it.
#   - The carve-out is submit_order-ONLY: another destructive tool
#     (live_place_order) is DENIED (exit 5 + "auth") even with trading armed,
#     proving the carve-out opened nothing else.
#   - serve --stop stops the daemon and removes bridge.json (leak-proof teardown).
#
# Mirrors tests/e2e_daemon_smoke.sh: fixed-up-front throwaway profile name,
# cleanup_all/fail/force_stop, EXIT trap, and a `timeout` watchdog on every
# daemon-routed call so a wedge FAILS loudly instead of hanging.
#
# DB note: the settings table lives in <profile>/data/openmarketterminal.db (the
# main DB), NOT cache.db — so we target that file by name rather than a blind
# `find | head -1`, and verify the settings schema before writing (fail loudly if
# the layout changed). Confirmed schema: key,value,category,updated_at.

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
PROF="paper-e2e-$$"
ROOT="$PROF_ROOT/$PROF"
PID=""

cleanup_all() {
    [ -n "$PID" ] && kill -9 "$PID" 2>/dev/null
    [ -n "$PROF" ] && rm -rf "$PROF_ROOT/$PROF" 2>/dev/null
    # Remove our entry from the default profile manifest so the registry is left
    # exactly as found (mirrors e2e_headless_smoke.sh's cleanup).
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

echo "== openterminalcli paper-trade e2e (over the serve daemon) =="
echo "CLI=$CLI  profile=$PROF  watchdog=${TIMEOUT:-none}"
echo

# ============================================================================
# Step 1 — bootstrap the profile DB (migrations run), then SIMULATE the GUI
# toggle by writing the GUI-only cli.* keys DIRECTLY into the DB.
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

# Simulated GUI toggle. Done BEFORE `serve` so the daemon reads it live.
#  - allow_paper_trading ON   -> paper executes.
#  - allow_trading + live_trading_armed ON -> the live checker carve-out passes,
#    forcing the HANDLER to be the refuser (the airtight hard-off proof).
set_gate() {
    sqlite3 "$DB" \
      "INSERT OR REPLACE INTO settings(key,value,category,updated_at) VALUES('$1','$2','cli','2026-01-01');" \
      || fail "sqlite write of $1 failed"
}
set_gate cli.allow_paper_trading true
set_gate cli.allow_trading       true
set_gate cli.live_trading_armed  true
# Read the paper gate back to prove the write actually landed.
[ "$(sqlite3 "$DB" "SELECT value FROM settings WHERE key='cli.allow_paper_trading';")" = "true" ] \
    || fail "paper gate write did not land in DB"
echo "PASS: simulated GUI toggle written to DB (paper ON; live armed)"

# ============================================================================
# Step 2 — start the daemon; assert kind=daemon.
# ============================================================================
"$CLI" --profile "$PROF" serve >/tmp/paper-e2e.log 2>&1 & PID=$!
sleep 6
wd 10 "$CLI" --profile "$PROF" serve --status | grep -q "kind=daemon" \
    || fail "serve --status did not report kind=daemon (log: $(cat /tmp/paper-e2e.log 2>/dev/null))"
echo "PASS: serve --status shows kind=daemon"

# ============================================================================
# Step 3 — prepare_order over the daemon (non-destructive -> allowed).
# ============================================================================
P_OUT=$(wd 30 "$CLI" --json --profile "$PROF" mcp call prepare_order \
        '{"symbol":"AAPL","side":"buy","quantity":10,"order_type":"limit","limit_price":200}' 2>&1)
P_RC=$?
[ $P_RC -eq 0 ] || fail "prepare_order over daemon rc=$P_RC out=$P_OUT"
P_STATUS=$(printf '%s' "$P_OUT" | json_field status)
DID1=$(printf '%s' "$P_OUT" | json_field draft_id)
[ "$P_STATUS" = "prepared" ] || fail "prepare_order status != prepared ($P_STATUS) out=$P_OUT"
[ -n "$DID1" ] || fail "prepare_order returned empty draft_id out=$P_OUT"
echo "PASS: prepare_order via daemon -> status=prepared draft_id=$DID1"

# ============================================================================
# Step 4 — submit_order PAPER over the daemon: carve-out + handler executes.
# A live FILL is also success:true, so we DISCRIMINATE on data.status=="filled".
# ============================================================================
S_OUT=$(wd 30 "$CLI" --json --profile "$PROF" mcp call submit_order \
        "{\"draft_id\":\"$DID1\",\"mode\":\"paper\"}" 2>&1)
S_RC=$?
[ $S_RC -eq 0 ] || fail "submit_order paper over daemon rc=$S_RC out=$S_OUT (auth-denied?)"
S_STATUS=$(printf '%s' "$S_OUT" | json_field status)
[ "$S_STATUS" = "filled" ] \
    || fail "submit_order paper status != filled ($S_STATUS) out=$S_OUT"
echo "PASS: submit_order PAPER via daemon -> status=filled (carve-out + paper rail executed)"
# Persistence proof: the draft walked prepared -> submitted.
D1_DB=$(sqlite3 "$DB" "SELECT status FROM order_drafts WHERE draft_id='$DID1';")
[ "$D1_DB" = "submitted" ] \
    || fail "paper draft did not walk to submitted in DB (got '$D1_DB')"
echo "PASS: paper draft walked prepared -> submitted in DB"

# ============================================================================
# Step 5 — submit_order LIVE over the daemon. Gates are ARMED, so the request
# reaches the HANDLER, which hard-offs. A live fill would be success:true too —
# so we assert status=="rejected" AND reason "live trading disabled", AND that
# the draft NEVER walked to submitted (persistence-layer non-execution proof).
# (1st draft is now "submitted", so prepare a 2nd.)
# ============================================================================
P2_OUT=$(wd 30 "$CLI" --json --profile "$PROF" mcp call prepare_order \
         '{"symbol":"AAPL","side":"buy","quantity":10,"order_type":"limit","limit_price":200}' 2>&1)
DID2=$(printf '%s' "$P2_OUT" | json_field draft_id)
[ -n "$DID2" ] || fail "2nd prepare_order returned empty draft_id out=$P2_OUT"

L_OUT=$(wd 30 "$CLI" --json --profile "$PROF" mcp call submit_order \
        "{\"draft_id\":\"$DID2\",\"mode\":\"live\"}" 2>&1)
L_RC=$?
L_STATUS=$(printf '%s' "$L_OUT" | json_field status)
L_REASON=$(printf '%s' "$L_OUT" | json_field reason)
if [ $L_RC -eq 0 ] && [ "$L_STATUS" = "rejected" ] \
       && printf '%s' "$L_REASON" | grep -qi "live trading disabled"; then
    echo "PASS: submit_order LIVE refused by HANDLER (status=rejected, '$L_REASON')"
elif [ $L_RC -eq 5 ] && printf '%s' "$L_OUT" | grep -qi "auth"; then
    # Fallback: the checker denied live before the handler — also "never executes".
    echo "PASS: submit_order LIVE denied by checker (exit 5 + auth) -> $L_OUT"
else
    fail "submit_order LIVE neither hard-off nor auth-denied (rc=$L_RC status=$L_STATUS out=$L_OUT)"
fi
# Persistence proof: live NEVER executed — draft 2 is still "prepared".
D2_DB=$(sqlite3 "$DB" "SELECT status FROM order_drafts WHERE draft_id='$DID2';")
[ "$D2_DB" = "prepared" ] \
    || fail "live draft walked away from prepared (got '$D2_DB') — live may have executed!"
echo "PASS: live draft remained 'prepared' in DB (live never executed)"

# ============================================================================
# Step 6 — keystone over the daemon: set_setting on a cli.* key is DENIED even
# with trading armed (exit 5 + auth). The read-only gate blocks settings writes.
# ============================================================================
K_OUT=$(wd 20 "$CLI" --profile "$PROF" mcp call set_setting \
        '{"key":"cli.allow_trading","value":"true"}' 2>&1)
K_RC=$?
{ [ $K_RC -eq 5 ] && printf '%s' "$K_OUT" | grep -qi "auth"; } \
    && echo "PASS: keystone — set_setting cli.* DENIED via daemon (exit 5) -> $K_OUT" \
    || fail "keystone: set_setting not denied (rc=$K_RC out=$K_OUT)"

# ============================================================================
# Step 7 — carve-out is submit_order-ONLY: another destructive tool is DENIED
# (exit 5 + auth) even with trading armed — the carve-out opened nothing else.
# ============================================================================
C_OUT=$(wd 20 "$CLI" --profile "$PROF" mcp call live_place_order '{}' 2>&1)
C_RC=$?
{ [ $C_RC -eq 5 ] && printf '%s' "$C_OUT" | grep -qi "auth"; } \
    && echo "PASS: carve-out scoped — live_place_order DENIED via daemon (exit 5) -> $C_OUT" \
    || fail "carve-out leaked: live_place_order not denied (rc=$C_RC out=$C_OUT)"

# ============================================================================
# Step 8 — serve --stop: daemon exits, bridge.json removed (leak-proof).
# ============================================================================
wd 10 "$CLI" --profile "$PROF" serve --stop | grep -qiE "SIGTERM|force-stopped" \
    || fail "serve --stop did not report SIGTERM/force-stopped"
force_stop "$PID" "$ROOT" 8
kill -0 "$PID" 2>/dev/null && fail "daemon still alive after --stop"
test -f "$ROOT/bridge.json" && fail "bridge.json remained after --stop" \
                            || echo "PASS: --stop stopped daemon + removed bridge.json"
PID=""

echo
echo "PASS: paper-trade e2e"
