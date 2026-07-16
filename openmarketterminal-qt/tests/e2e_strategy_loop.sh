#!/bin/bash
# End-to-end: the paper Strategy-Loop Driver over the HEADLESS in-process runtime,
# NO GUI, NO daemon. Proves `ai run strategy <name> --mode paper …` drives the
# StrategyRunner (which only calls the gated paper substrate) and honors bounds,
# the paper refusal of --mode live, and the live kill switch.
#
# Asserts the headline safety/behavior properties:
#   1. `meanrev --mode paper --interval-sec 0 --max-iters 3` runs EXACTLY 3 ticks
#      and exits cleanly (rc 0) with a greppable summary line. Headless has no
#      live quote feed so the strategy may propose nothing — that's fine; what we
#      assert is the LOOP ran its bound and exited.
#   2. `--mode live` is REFUSED: nonzero exit + "live strategy loop not supported"
#      (the driver is paper-only; it opens no live execution path).
#   3. KILL SWITCH: with cli.kill_switch=true the loop halts on the FIRST tick —
#      exits 0, the summary shows halted=true (+ "halted by kill switch" log), and
#      it ran FEWER than the 5-iter bound (1 tick). Reset the switch after.
#
# Mirrors tests/e2e_paper_trade.sh safety scaffolding: fixed-up-front throwaway
# profile name, cleanup_all/fail, EXIT trap, and a `timeout` watchdog on every
# CLI call so a wedged loop FAILS loudly instead of hanging forever.
#
# DB note: the settings table lives in <profile>/data/openmarketterminal.db (the
# main DB). The CLI tool path REFUSES cli.* writes (that's the keystone), so —
# exactly like the GUI — we toggle the gate via a DIRECT sqlite write. Confirmed
# schema: key,value,category,updated_at.

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
PROF="strat-e2e-$$"
ROOT="$PROF_ROOT/$PROF"

cleanup_all() {
    [ -n "$PROF" ] && rm -rf "$PROF_ROOT/$PROF" 2>/dev/null
    # Remove our entry from the profile manifest so the registry is left as found.
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

[ -x "$CLI" ] || fail "CLI not found/executable at $CLI"

echo "== openterminalcli strategy-loop e2e (headless, no GUI) =="
echo "CLI=$CLI  profile=$PROF  watchdog=${TIMEOUT:-none}"
echo

# ============================================================================
# Step 0 — bootstrap the profile DB (migrations run), then SIMULATE the GUI
# paper-trading toggle by writing the cli.* key DIRECTLY into the DB.
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

set_gate() {
    sqlite3 "$DB" \
      "INSERT OR REPLACE INTO settings(key,value,category,updated_at) VALUES('$1','$2','cli','2026-01-01');" \
      || fail "sqlite write of $1 failed"
}
set_gate cli.allow_paper_trading true
[ "$(sqlite3 "$DB" "SELECT value FROM settings WHERE key='cli.allow_paper_trading';")" = "true" ] \
    || fail "paper gate write did not land in DB"
echo "PASS: simulated GUI toggle written to DB (paper trading ON)"

# ============================================================================
# Step 1 — bounded paper run: meanrev, interval 0, max-iters 3. Exit 0 + summary
# line + EXACTLY 3 ticks (the loop honored its bound and exited cleanly).
# ============================================================================
R1=$(wd 60 "$CLI" --headless --profile "$PROF" \
        ai run strategy meanrev --mode paper --interval-sec 0 --max-iters 3 --symbols AAPL 2>&1)
RC1=$?
echo "$R1" | sed 's/^/    | /'
[ $RC1 -eq 0 ] || fail "bounded meanrev run rc=$RC1 (expected 0)"
SUMMARY=$(printf '%s\n' "$R1" | grep -E "^summary:|ticks=") \
    || fail "no summary line (expected one containing 'ticks')"
TICKS=$(printf '%s\n' "$SUMMARY" | sed -n 's/.*ticks=\([0-9][0-9]*\).*/\1/p' | head -1)
[ "$TICKS" = "3" ] || fail "expected 3 ticks, got '$TICKS' (summary: $SUMMARY)"
echo "PASS: bounded paper run -> rc=0, ran exactly 3 ticks, clean summary"
# The default-ON deterministic floor must be observable + CLI-toggleable.
printf '%s' "$R1" | grep -q "floor=on" \
    || fail "default run did not report 'floor=on' (out: $R1)"
printf '%s' "$SUMMARY" | grep -q "floor_skipped=" \
    || fail "summary line missing 'floor_skipped=' (summary: $SUMMARY)"

# ============================================================================
# Step 1b — --no-floor opts out of the default-ON floor (CLI parity): exit 0,
# and the run line reports floor=off.
# ============================================================================
R1B=$(wd 60 "$CLI" --headless --profile "$PROF" \
        ai run strategy meanrev --mode paper --interval-sec 0 --max-iters 3 --symbols AAPL --no-floor 2>&1)
RC1B=$?
[ $RC1B -eq 0 ] || fail "--no-floor run rc=$RC1B (expected 0)"
printf '%s' "$R1B" | grep -q "floor=off" \
    || fail "--no-floor did not report 'floor=off' (out: $R1B)"
echo "PASS: --no-floor accepted, floor=off"

# ============================================================================
# Step 1c — --max-aggregate-qty N threads the cross-handler aggregate cap into
# RunConfig (CLI parity, ⑦ Task 3): exit 0, and the run line reports agg_cap=5.
# ============================================================================
R1C=$(wd 60 "$CLI" --headless --profile "$PROF" \
        ai run strategy meanrev --mode paper --interval-sec 0 --max-iters 3 --symbols AAPL --max-aggregate-qty 5 2>&1)
RC1C=$?
[ $RC1C -eq 0 ] || fail "--max-aggregate-qty run rc=$RC1C (expected 0)"
printf '%s' "$R1C" | grep -q "agg_cap=5" \
    || fail "--max-aggregate-qty did not report 'agg_cap=5' (out: $R1C)"
echo "PASS: --max-aggregate-qty accepted, agg_cap=5"

# ============================================================================
# Step 2 — --mode live is REFUSED: nonzero exit + exact-substring message.
# ============================================================================
R2=$(wd 30 "$CLI" --headless --profile "$PROF" \
        ai run strategy meanrev --mode live --max-iters 1 2>&1)
RC2=$?
[ $RC2 -ne 0 ] || fail "--mode live returned 0 (must be refused, nonzero)"
printf '%s' "$R2" | grep -qi "live strategy loop not supported" \
    || fail "--mode live did not print 'live strategy loop not supported' (out: $R2)"
echo "PASS: --mode live refused (rc=$RC2, 'live strategy loop not supported')"

# ============================================================================
# Step 3 — KILL SWITCH: engage it, run a 5-iter bound; the loop must halt on the
# FIRST tick (exit 0, halted=true, fewer than 5 ticks). Then reset.
# ============================================================================
set_gate cli.kill_switch true
[ "$(sqlite3 "$DB" "SELECT value FROM settings WHERE key='cli.kill_switch';")" = "true" ] \
    || fail "kill_switch write did not land in DB"

R3=$(wd 60 "$CLI" --headless --profile "$PROF" \
        ai run strategy meanrev --mode paper --interval-sec 0 --max-iters 5 --symbols AAPL 2>&1)
RC3=$?
echo "$R3" | sed 's/^/    | /'
set_gate cli.kill_switch false   # reset regardless of outcome below

[ $RC3 -eq 0 ] || fail "kill-switch run rc=$RC3 (expected clean exit 0)"
printf '%s' "$R3" | grep -qiE "kill switch|halted" \
    || fail "kill-switch run did not report a halt (no 'kill switch'/'halted')"
KTICKS=$(printf '%s\n' "$R3" | grep -E "^summary:|ticks=" | sed -n 's/.*ticks=\([0-9][0-9]*\).*/\1/p' | head -1)
[ -n "$KTICKS" ] || fail "kill-switch run: could not parse ticks from summary (out: $R3)"
[ "$KTICKS" -lt 5 ] || fail "kill-switch run ran $KTICKS ticks (expected < 5; should halt on first tick)"
printf '%s' "$R3" | grep -qi "halted=true" \
    || fail "kill-switch summary did not show halted=true (out: $R3)"
echo "PASS: kill switch halted the loop on first tick (rc=0, halted=true, ticks=$KTICKS < 5)"

echo
echo "PASS: strategy-loop e2e"
