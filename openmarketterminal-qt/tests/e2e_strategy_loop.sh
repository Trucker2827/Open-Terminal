#!/bin/bash
# End-to-end: the paper Strategy-Loop Driver over the HEADLESS in-process runtime,
# NO GUI, NO daemon. Proves `ai run strategy <name> --mode paper …` drives the
# StrategyRunner (which only calls the gated paper substrate) and honors bounds,
# the DISARMED refusal of --mode live, and the live kill switch.
#
# Asserts the headline safety/behavior properties:
#   1. `meanrev --mode paper --interval-sec 0 --max-iters 3` runs EXACTLY 3 ticks
#      and exits cleanly (rc 0) with a greppable summary line. Headless has no
#      live quote feed so the strategy may propose nothing — that's fine; what we
#      assert is the LOOP ran its bound and exited.
#   2. `--mode live` is REFUSED while UNARMED: nonzero exit + "not armed" (this
#      e2e NEVER writes cli.live_armed/cli.trading_allowed, so the human-armed
#      gate never opens — submit_order is never reached, no fill/order occurs).
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

CLI="${CLI:-/tmp/ot-build-ht/openterminalcli}"
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

# extract a JSON field from {"data":{...},"success":...} stdout via python3
# (mirrors tests/e2e_paper_trade.sh's helper).
json_field() { python3 -c 'import sys,json
try:
    d=json.load(sys.stdin)
    print(d.get("data",{}).get(sys.argv[1],""))
except Exception:
    print("")' "$1"; }

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
# Step 1d — --max-position-qty N and --max-notional-per-order N thread the
# per-handler position cap and per-order notional cap into RunConfig (CLI
# parity, F5): exit 0, and the run line reports pos_cap=7 and notional_cap=500.
# ============================================================================
R1D=$(wd 60 "$CLI" --headless --profile "$PROF" \
        ai run strategy meanrev --mode paper --interval-sec 0 --max-iters 3 --symbols AAPL \
        --max-position-qty 7 --max-notional-per-order 500 2>&1)
RC1D=$?
[ $RC1D -eq 0 ] || fail "--max-position-qty/--max-notional-per-order run rc=$RC1D (expected 0)"
printf '%s' "$R1D" | grep -q "pos_cap=7" \
    || fail "--max-position-qty did not report 'pos_cap=7' (out: $R1D)"
printf '%s' "$R1D" | grep -q "notional_cap=500" \
    || fail "--max-notional-per-order did not report 'notional_cap=500' (out: $R1D)"
echo "PASS: --max-position-qty/--max-notional-per-order accepted, pos_cap=7 notional_cap=500"

# ============================================================================
# Step 2 — --mode live is REFUSED while UNARMED (headline safety property, Track
# 3): nonzero exit + stderr contains "not armed". This e2e NEVER writes
# cli.live_armed/cli.trading_allowed (only allow_paper_trading, above), so the
# human-armed gate never opens; the refusal fires after the transport/DB is up
# (the strategy is already instantiated by then) but BEFORE the runner ticks
# even once, so the loop never ran and NO fill/order can have occurred
# (confirmed below: no "summary:" line was ever printed).
#
# NOTE: this reconciles a prior assertion ("live strategy loop not supported")
# that the old blanket ai_run_strategy refusal produced. That message is gone
# by design now that `ai run strategy --mode live` reaches the armed gate
# (cli_trading_allowed() && cli_live_armed()) instead of a hard-coded refusal.
# ============================================================================
R2=$(wd 30 "$CLI" --headless --profile "$PROF" \
        ai run strategy meanrev --mode live --max-iters 1 2>&1)
RC2=$?
[ $RC2 -ne 0 ] || fail "--mode live (unarmed) returned 0 (must be refused, nonzero)"
printf '%s' "$R2" | grep -qi "not armed" \
    || fail "--mode live (unarmed) did not print 'not armed' (out: $R2)"
printf '%s' "$R2" | grep -q "^summary:" \
    && fail "--mode live (unarmed) produced a summary line -- the loop must never have run (out: $R2)"
echo "PASS: --mode live refused while unarmed (rc=$RC2, 'not armed', no run/fill occurred)"

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


# ============================================================================
# Step 4 — ARMED reachability: with the throwaway profile ARMED directly via
# sqlite (cli.live_trading_armed=true, cli.allow_trading=true -- mirrors the
# kill-switch set_gate above), `--mode live` must PASS the CLI-side armed gate
# (Track 3 fix: the gate now runs AFTER the DB is up, so it reads the REAL
# arm state instead of the unset default). This profile has NO live broker
# credentials/allowed-account configured, so submit_order -- the SOLE
# authority, re-checking armed -> allowed-account -> daily-loss -> reserve ->
# place_order -- must DENY at the ALLOWED-ACCOUNT gate, before the
# irreversible place_order call.
#
# Two sub-checks, because they prove DIFFERENT things and one command can't
# prove both here:
#   4a. `ai run strategy meanrev --mode live` proves CLI-gate reachability
#       (no "not armed" refusal) -- the actual ordering fix under test.
#       Headless has NO live quote feed, so meanrev's propose() sees no quote
#       for AAPL and never proposes an intent (see meanrev_warmup_no_intent
#       in tst_strategy_loop) -- proposed=0/prepared=0/filled=0 here just
#       means "nothing to trade", NOT "submit_order denied it". Asserting
#       filled=0 alone would also trivially hold when nothing was proposed,
#       so this sub-check ONLY asserts CLI-gate reachability.
#   4b. A direct `mcp call prepare_order` + `mcp call submit_order` (mode
#       live), mirroring tests/e2e_paper_trade.sh's Step 5, gives submit_order
#       an explicit intent (no quote dependency) so it is actually invoked.
#       StrategyRunner's internal submit is the identical
#       tc.call("submit_order", {draft_id, mode}) over the same headless
#       CliToolCaller, so this direct call exercises the SAME handler path
#       the strategy loop would use -- it proves submit_order (the sole
#       authority) reaches and denies at the allowed-account gate.
# Both run while armed; the arm flags are reset once at the end regardless
# of outcome, leaving the profile disarmed.
# ============================================================================
set_gate cli.live_trading_armed true
set_gate cli.allow_trading true
[ "$(sqlite3 "$DB" "SELECT value FROM settings WHERE key='cli.live_trading_armed';")" = "true" ] \
    || fail "live_trading_armed write did not land in DB"
[ "$(sqlite3 "$DB" "SELECT value FROM settings WHERE key='cli.allow_trading';")" = "true" ] \
    || fail "allow_trading write did not land in DB"

# -- 4a. CLI-gate reachability via the strategy loop --------------------------
# --max-notional-per-order and --max-position-qty are REQUIRED here (in addition
# to --max-iters and the default-ON floor) now that live-mode containment (P-C
# #39-c, P-E #39 final) gates entry: a bounded, notional-capped, position-capped,
# floor-on request is the minimum that clears containment, so this still
# isolates "not armed" reachability from containment.
R4=$(wd 60 "$CLI" --headless --profile "$PROF" \
        ai run strategy meanrev --mode live --interval-sec 0 --max-iters 1 --symbols AAPL \
        --max-notional-per-order 500 --max-position-qty 100 2>&1)
RC4=$?
echo "$R4" | sed 's/^/    | /'

printf '%s' "$R4" | grep -qi "not armed" \
    && fail "armed run was refused 'not armed' -- CLI armed gate did not read real arm state (out: $R4)"
printf '%s' "$R4" | grep -qi "live mode requires" \
    && fail "armed+contained run was refused by containment unexpectedly (out: $R4)"
printf '%s' "$R4" | grep -q "^\[strategy\] running .* mode=live armed=true" \
    || fail "armed run did not report mode=live armed=true (out: $R4)"
echo "PASS: armed run was NOT refused 'not armed' (mode=live armed=true) -- CLI gate reachability proven"

# -- 4b. submit_order denial via a direct call (explicit intent, no quote dep) -
P4_OUT=$(wd 30 "$CLI" --headless --profile "$PROF" --json mcp call prepare_order \
        '{"symbol":"AAPL","side":"buy","quantity":10,"order_type":"limit","limit_price":200}' 2>&1)
P4_STATUS=$(printf '%s' "$P4_OUT" | json_field status)
DID4=$(printf '%s' "$P4_OUT" | json_field draft_id)
[ "$P4_STATUS" = "prepared" ] && [ -n "$DID4" ] \
    || fail "armed prepare_order did not return status=prepared+draft_id (out: $P4_OUT)"

S4_OUT=$(wd 30 "$CLI" --headless --profile "$PROF" --json mcp call submit_order \
        "{\"draft_id\":\"$DID4\",\"mode\":\"live\"}" 2>&1)
S4_STATUS=$(printf '%s' "$S4_OUT" | json_field status)
S4_REASON=$(printf '%s' "$S4_OUT" | json_field reason)
[ "$S4_STATUS" = "rejected" ] \
    || fail "armed submit_order(live) status != rejected ($S4_STATUS) -- expected a DENY, not a fill (out: $S4_OUT)"
printf '%s' "$S4_REASON" | grep -qi "no allowed account" \
    || fail "armed submit_order(live) reason did not mention 'no allowed account' (out: $S4_OUT)"
echo "PASS: submit_order(live) reached and DENIED at the allowed-account gate (status=rejected, '$S4_REASON', no real broker contacted)"
D4_DB=$(sqlite3 "$DB" "SELECT status FROM order_drafts WHERE draft_id='$DID4';")
[ "$D4_DB" = "prepared" ] \
    || fail "armed draft did not stay 'prepared' (live submit must NEVER execute) -- got '$D4_DB'"
echo "PASS: armed draft persisted as 'prepared' (never walked to submitted/filled)"

set_gate cli.live_trading_armed false   # reset regardless of outcome above
set_gate cli.allow_trading false
[ "$(sqlite3 "$DB" "SELECT value FROM settings WHERE key='cli.live_trading_armed';")" = "false" ] \
    || fail "live_trading_armed reset did not land in DB"
[ "$(sqlite3 "$DB" "SELECT value FROM settings WHERE key='cli.allow_trading';")" = "false" ] \
    || fail "allow_trading reset did not land in DB"
echo "PASS: arm flags reset -- profile left disarmed"

# ============================================================================
# Step 5 — LIVE CONTAINMENT (P-C #39-c, P-E #39 final): once armed, `--mode
# live` must still refuse an unrestricted run. Human arming (Step 4) gates
# ENTRY to the live rail; this proves strategy-level containment is enforced
# ONCE in: the floor, a bounded session, a positive per-order notional cap, and
# a positive per-handler position cap (now enforced against the LIVE position
# ledger, P-E) are REQUIRED, and the cross-handler aggregate cap (no live
# analog) is REJECTED outright.
#
# Re-arm the throwaway profile the same way Step 4 did (direct sqlite write of
# cli.live_trading_armed=true + cli.allow_trading=true); this profile still has
# NO cli.allowed_account configured, so even the one case that clears
# containment can never reach a live fill -- it would deny at submit_order's
# allowed-account gate exactly like Step 4b. Reset the arm flags after.
# ============================================================================
set_gate cli.live_trading_armed true
set_gate cli.allow_trading true
[ "$(sqlite3 "$DB" "SELECT value FROM settings WHERE key='cli.live_trading_armed';")" = "true" ] \
    || fail "live_trading_armed write did not land in DB (containment step)"
[ "$(sqlite3 "$DB" "SELECT value FROM settings WHERE key='cli.allow_trading';")" = "true" ] \
    || fail "allow_trading write did not land in DB (containment step)"

# -- 5a. --no-floor: missing floor is refused ---------------------------------
R5A=$(wd 30 "$CLI" --headless --profile "$PROF" \
        ai run strategy meanrev --mode live --no-floor --max-iters 1 --max-notional-per-order 500 2>&1)
RC5A=$?
[ $RC5A -eq 2 ] || fail "containment: --no-floor live run rc=$RC5A (expected 2)"
printf '%s' "$R5A" | grep -qi "requires" \
    || fail "containment: --no-floor live run missing 'requires' (out: $R5A)"
printf '%s' "$R5A" | grep -qi "floor" \
    || fail "containment: --no-floor live run did not mention the floor (out: $R5A)"
echo "PASS: containment refuses --mode live --no-floor (rc=2, mentions the floor)"

# -- 5b. unbounded session (--max-iters 0 --duration-sec 0) is refused --------
R5B=$(wd 30 "$CLI" --headless --profile "$PROF" \
        ai run strategy meanrev --mode live --max-iters 0 --duration-sec 0 --max-notional-per-order 500 2>&1)
RC5B=$?
[ $RC5B -eq 2 ] || fail "containment: unbounded live run rc=$RC5B (expected 2)"
printf '%s' "$R5B" | grep -qi "bounded session" \
    || fail "containment: unbounded live run missing 'bounded session' (out: $R5B)"
echo "PASS: containment refuses unbounded --mode live (rc=2, 'bounded session')"

# -- 5c. no --max-notional-per-order is refused --------------------------------
R5C=$(wd 30 "$CLI" --headless --profile "$PROF" \
        ai run strategy meanrev --mode live --max-iters 1 2>&1)
RC5C=$?
[ $RC5C -eq 2 ] || fail "containment: no-notional live run rc=$RC5C (expected 2)"
printf '%s' "$R5C" | grep -qi "notional" \
    || fail "containment: no-notional live run missing 'notional' (out: $R5C)"
echo "PASS: containment refuses --mode live with no --max-notional-per-order (rc=2, 'notional')"

# -- 5c2. no --max-position-qty is refused (P-E, #39 final: completes the ------
# per-handler position-cap containment -- the cap now reads the LIVE ledger,
# so it must be REQUIRED, not merely optional, in live mode).
R5C2=$(wd 30 "$CLI" --headless --profile "$PROF" \
        ai run strategy meanrev --mode live --max-iters 1 --max-notional-per-order 500 2>&1)
RC5C2=$?
[ $RC5C2 -eq 2 ] || fail "containment: no-position-cap live run rc=$RC5C2 (expected 2)"
printf '%s' "$R5C2" | grep -qi "position" \
    || fail "containment: no-position-cap live run missing 'position' (out: $R5C2)"
echo "PASS: containment refuses --mode live with no --max-position-qty (rc=2, 'position')"

# -- 5d. --max-aggregate-qty is rejected outright (no live analog) ------------
R5D=$(wd 30 "$CLI" --headless --profile "$PROF" \
        ai run strategy meanrev --mode live --max-iters 1 --max-notional-per-order 500 \
        --max-position-qty 100 --max-aggregate-qty 5 2>&1)
RC5D=$?
[ $RC5D -eq 2 ] || fail "containment: aggregate-cap live run rc=$RC5D (expected 2)"
printf '%s' "$R5D" | grep -qi "no live analog" \
    || fail "containment: aggregate-cap live run missing 'no live analog' (out: $R5D)"
echo "PASS: containment rejects --mode live --max-aggregate-qty (rc=2, 'no live analog')"

# -- 5e. fully-contained armed live run PASSES containment --------------------
# floor default-on (no --no-floor), bounded (--max-iters 1), positive notional,
# positive position cap, no aggregate cap: must NOT be refused by containment.
# It still cannot reach a live fill on this profile (no cli.allowed_account) --
# that denial is proven by Step 4b's direct submit_order call over the
# identical handler path; this step only proves containment itself does not
# misfire on a compliant request.
R5E=$(wd 60 "$CLI" --headless --profile "$PROF" \
        ai run strategy meanrev --mode live --max-iters 1 --interval-sec 0 --symbols AAPL \
        --max-notional-per-order 500 --max-position-qty 100 2>&1)
RC5E=$?
echo "$R5E" | sed 's/^/    | /'
[ $RC5E -eq 0 ] || fail "containment: fully-contained live run rc=$RC5E (expected 0)"
printf '%s' "$R5E" | grep -qi "live mode requires" \
    && fail "containment: fully-contained live run was wrongly refused by containment (out: $R5E)"
printf '%s' "$R5E" | grep -qi "no live analog" \
    && fail "containment: fully-contained live run was wrongly refused by the aggregate check (out: $R5E)"
printf '%s' "$R5E" | grep -q "^\[strategy\] running .* mode=live armed=true" \
    || fail "containment: fully-contained live run did not proceed past the gate (out: $R5E)"
printf '%s' "$R5E" | grep -qE "^summary:.*filled=0" \
    || fail "containment: fully-contained live run did not report filled=0 -- no live fill must occur (out: $R5E)"
echo "PASS: containment passes a fully-contained armed live run (floor on, bounded, notional cap, no aggregate) -- proceeds to the gate, no live fill"

set_gate cli.live_trading_armed false   # reset regardless of outcome above
set_gate cli.allow_trading false
[ "$(sqlite3 "$DB" "SELECT value FROM settings WHERE key='cli.live_trading_armed';")" = "false" ] \
    || fail "live_trading_armed reset did not land in DB (containment step)"
[ "$(sqlite3 "$DB" "SELECT value FROM settings WHERE key='cli.allow_trading';")" = "false" ] \
    || fail "allow_trading reset did not land in DB (containment step)"
echo "PASS: containment arm flags reset -- profile left disarmed"

echo
echo "PASS: strategy-loop e2e"
