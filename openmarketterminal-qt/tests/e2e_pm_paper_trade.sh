#!/bin/bash
# End-to-end: the PREDICTION-MARKET (Phase B) order flow over the SERVE DAEMON,
# NO GUI. The integration proof for Phase B's safety constitution.
#
# The human flips the GUI-only AI-trading constitution (modeled here by a DIRECT
# sqlite write of the cli.* keys — the tool path REFUSES cli.* writes, that is the
# KEYSTONE, so a direct DB write is the only way, exactly as the GUI does it):
#   - cli.allow_paper_trading = true   (paper executes)
#   - cli.allowed_venues = polymarket,kalshi  (the venue allow-list)
# then an AI client drives prepare_order / submit_order (asset_class=prediction)
# over the daemon.
#
# IMPORTANT — the daemon runs the REAL PM adapters (live network). So the
# AUTHORITATIVE happy-path fill proof is the UNIT TEST (tst_pm_paper); here we
# HARD-assert only the NETWORK-INDEPENDENT constitution and best-effort the
# live-data path under a watchdog (never failing the script on a network outcome).
#
# HARD asserts (deterministic, no network):
#   1. prepare_order (prediction) with a venue NOT in cli.allowed_venues is
#      REJECTED with "not in allowed venues" — the venue allow-list fires BEFORE
#      any book resolution, so it is deterministic. The call itself succeeds
#      (rc 0, ok_data) but status=="rejected".
#   2. submit_order mode=live (bad draft) NEVER FILLS. With paper-only toggles
#      (trading/arming OFF), the carve-out denies live at the auth gate
#      (exit 5 + "auth"); even if it reached the handler it hard-offs
#      (status rejected). Either way: NOT "filled". Live never executes.
#   3. KEYSTONE: set_setting on a cli.* key over the daemon is DENIED
#      (exit 5 + "auth") — the read-only gate blocks the constitution from being
#      rewritten by a tool, even with paper armed. (cli.allowed_venues here.)
#   4. carve-out is submit_order-ONLY: live_place_order over the daemon is DENIED
#      (exit 5 + "auth") — the carve-out opened nothing else.
#   5. pm_paper_portfolio over the daemon SUCCEEDS (non-destructive read reaches
#      the handler) — proves PM read tools route through the daemon.
#
# Mirrors tests/e2e_paper_trade.sh: fixed-up-front throwaway profile name,
# cleanup_all/fail/force_stop, EXIT trap, and a `timeout` watchdog on every
# daemon-routed call so a wedge FAILS loudly instead of hanging.

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
PROF="pm-e2e-$$"
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

echo "== openterminalcli PM-paper-trade e2e (over the serve daemon, Phase B) =="
echo "CLI=$CLI  profile=$PROF  watchdog=${TIMEOUT:-none}"
echo

# ============================================================================
# Step 1 — bootstrap the profile DB (migrations run), then SIMULATE the GUI
# constitution by writing the GUI-only cli.* keys DIRECTLY into the DB.
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
#  - allow_paper_trading ON -> paper executes.
#  - allowed_venues = polymarket,kalshi -> the PM venue allow-list.
# NOTE: cli.allow_trading / cli.live_trading_armed are deliberately LEFT OFF, so
# the submit_order LIVE carve-out denies at the auth gate (exit 5 + auth).
sqlite3 "$DB" \
  "INSERT OR REPLACE INTO settings(key,value,category,updated_at) VALUES('cli.allow_paper_trading','true','cli','2026-01-01'),('cli.allowed_venues','polymarket,kalshi','cli','2026-01-01');" \
    || fail "sqlite write of constitution failed"
[ "$(sqlite3 "$DB" "SELECT value FROM settings WHERE key='cli.allow_paper_trading';")" = "true" ] \
    || fail "paper gate write did not land in DB"
[ "$(sqlite3 "$DB" "SELECT value FROM settings WHERE key='cli.allowed_venues';")" = "polymarket,kalshi" ] \
    || fail "allowed_venues write did not land in DB"
echo "PASS: simulated GUI constitution written (paper ON; venues=polymarket,kalshi; live OFF)"

# ============================================================================
# Step 2 — start the daemon; assert kind=daemon.
# ============================================================================
"$CLI" --profile "$PROF" serve >/tmp/pm-e2e.log 2>&1 & PID=$!
sleep 6
wd 10 "$CLI" --profile "$PROF" serve --status | grep -q "kind=daemon" \
    || fail "serve --status did not report kind=daemon (log: $(cat /tmp/pm-e2e.log 2>/dev/null))"
echo "PASS: serve --status shows kind=daemon"

# ============================================================================
# HARD ASSERT 1 — prepare_order (prediction) venue NOT allowed -> rejected.
# Deterministic: the venue allow-list fires BEFORE any book resolution. The call
# succeeds (rc 0, ok_data) but data.status=="rejected" with the right reason.
# ============================================================================
V_OUT=$(wd 30 "$CLI" --json --profile "$PROF" mcp call prepare_order \
        '{"asset_class":"prediction","venue":"notavenue","market_id":"x","asset_id":"y","outcome":"YES","side":"buy","contracts":1}' 2>&1)
V_RC=$?
V_STATUS=$(printf '%s' "$V_OUT" | json_field status)
V_REASON=$(printf '%s' "$V_OUT" | json_field reason)
{ [ $V_RC -eq 0 ] && [ "$V_STATUS" = "rejected" ] \
    && printf '%s' "$V_REASON" | grep -qi "not in allowed venues"; } \
    && echo "PASS: prepare_order venue-not-allowed REJECTED -> '$V_REASON'" \
    || fail "venue gate did not reject (rc=$V_RC status=$V_STATUS out=$V_OUT)"

# ============================================================================
# HARD ASSERT 2 — submit_order mode=live NEVER FILLS. With paper-only toggles,
# the carve-out denies live at the auth gate (exit 5 + auth). Even if it reached
# the handler it hard-offs (status rejected). Either way: NOT "filled".
# ============================================================================
L_OUT=$(wd 30 "$CLI" --json --profile "$PROF" mcp call submit_order \
        '{"draft_id":"nope","mode":"live"}' 2>&1)
L_RC=$?
L_STATUS=$(printf '%s' "$L_OUT" | json_field status)
if [ "$L_STATUS" = "filled" ]; then
    fail "submit_order LIVE FILLED — live executed! (rc=$L_RC out=$L_OUT)"
fi
if [ $L_RC -eq 5 ] && printf '%s' "$L_OUT" | grep -qi "auth"; then
    echo "PASS: submit_order LIVE denied by checker (exit 5 + auth, never reached handler) -> $L_OUT"
elif [ $L_RC -eq 0 ] && [ "$L_STATUS" = "rejected" ]; then
    echo "PASS: submit_order LIVE rejected by handler (status=rejected, did not fill) -> $L_OUT"
else
    fail "submit_order LIVE neither auth-denied nor rejected (rc=$L_RC status=$L_STATUS out=$L_OUT)"
fi

# ============================================================================
# HARD ASSERT 3 — KEYSTONE: set_setting on a cli.* constitution key over the
# daemon is DENIED (exit 5 + auth). The read-only gate blocks the constitution
# from being rewritten by a tool, even with paper armed.
# ============================================================================
K_OUT=$(wd 20 "$CLI" --profile "$PROF" mcp call set_setting \
        '{"key":"cli.allowed_venues","value":"hacked"}' 2>&1)
K_RC=$?
{ [ $K_RC -eq 5 ] && printf '%s' "$K_OUT" | grep -qi "auth"; } \
    && echo "PASS: keystone — set_setting cli.allowed_venues DENIED via daemon (exit 5 + auth) -> $K_OUT" \
    || fail "keystone: set_setting not denied (rc=$K_RC out=$K_OUT)"

# ============================================================================
# HARD ASSERT 4 — carve-out is submit_order-ONLY: live_place_order over the
# daemon is DENIED (exit 5 + auth) even with paper armed.
# ============================================================================
C_OUT=$(wd 20 "$CLI" --profile "$PROF" mcp call live_place_order '{}' 2>&1)
C_RC=$?
{ [ $C_RC -eq 5 ] && printf '%s' "$C_OUT" | grep -qi "auth"; } \
    && echo "PASS: carve-out scoped — live_place_order DENIED via daemon (exit 5 + auth) -> $C_OUT" \
    || fail "carve-out leaked: live_place_order not denied (rc=$C_RC out=$C_OUT)"

# ============================================================================
# HARD ASSERT 5 — pm_paper_portfolio over the daemon SUCCEEDS: a non-destructive
# PM read reaches the handler (proves PM read tools route through the daemon).
# ============================================================================
PP_OUT=$(wd 20 "$CLI" --json --profile "$PROF" mcp call pm_paper_portfolio '{}' 2>&1)
PP_RC=$?
[ $PP_RC -eq 0 ] \
    && echo "PASS: pm_paper_portfolio via daemon SUCCEEDED (rc=0, non-destructive read reached handler)" \
    || fail "pm_paper_portfolio failed via daemon (rc=$PP_RC out=$PP_OUT)"

# ============================================================================
# BEST-EFFORT (LIVE network) — pm_search_markets under a watchdog. NEVER fail the
# script on a live-network outcome (rc 0 ok / 5 adapter-error / 124 watchdog are
# all logged, not fatal). The AUTHORITATIVE paper-fill proof is tst_pm_paper.
# ============================================================================
echo "-- best-effort (live network; non-fatal) --"
SM_OUT=$(wd 30 "$CLI" --json --profile "$PROF" mcp call pm_search_markets \
         '{"venue":"polymarket","query":"election"}' 2>&1)
SM_RC=$?
case $SM_RC in
    0)   echo "INFO: pm_search_markets returned (rc=0) — live data reachable" ;;
    5)   echo "INFO: pm_search_markets adapter-error (rc=5) — live network unavailable (non-fatal)" ;;
    124) echo "INFO: pm_search_markets watchdog-timeout (rc=124) — live network slow (non-fatal)" ;;
    *)   echo "INFO: pm_search_markets rc=$SM_RC (non-fatal, live network)" ;;
esac
echo "INFO: live happy-path fill is NOT asserted here; authoritative proof is tst_pm_paper (unit)."

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
echo "PASS: PM-paper-trade e2e"
