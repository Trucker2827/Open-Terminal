#!/bin/bash
# e2e_kalshi_bot_live_refusal.sh — issue #130, micro-live containment proved
# against the REAL openterminalcli, offline, no GUI and no daemon.
#
# The unit tests (tst_kalshi_bot_live) prove each carve-out condition refuses on
# its own. They cannot prove the four command-level properties the issue asks
# for, so this does:
#
#   1. `kalshi bot run --mode live` EXITS NON-ZERO with a named refusal when no
#      human has armed a session, and journals that refusal with no numbers;
#   2. the kill switch outranks everything — with the stop file present, live
#      mode is refused as BOT_STOPPED before the arm is even considered;
#   3. PAPER is still the default: the same command with no --mode bids and
#      writes mode=paper rows (positive control — without it, "live placed no
#      order" would prove nothing);
#   4. NOTHING in this rung arms anything: after every refusal above, the live
#      session file is still absent and no live automation setting was written.
#
# Everything runs against a temp evidence dir via
# OPENTERMINAL_KALSHI_EVIDENCE_DIR (cli/ServeCommand.h's kalshi_evidence_path
# and CommandDispatch.cpp's kalshi_auto_evidence_path both honour it), so the
# operator's real ledger and real live session are never touched. No armed
# session exists in that directory, so no path here can reach an exchange.
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-$(cd "$(dirname "$0")/../build" && pwd)}"
CLI="${CLI:-$BUILD_DIR/openterminalcli}"
[ -x "$CLI" ] || { echo "FAIL: no openterminalcli at $CLI"; exit 1; }

EVIDENCE="$(mktemp -d)"
trap 'rm -rf "$EVIDENCE"' EXIT
export OPENTERMINAL_KALSHI_EVIDENCE_DIR="$EVIDENCE"

LEDGER="$EVIDENCE/kalshi-bot-decisions.jsonl"
STOP="$EVIDENCE/kalshi-bot-stop.json"
SESSION="$EVIDENCE/kalshi-live-session.json"

fail() { echo "FAIL: $*"; exit 1; }

# The same report shape the kill-switch e2e uses: one contract, a 12-point
# edge, 10 minutes of runway, generated NOW.
write_report() {
  python3 - "$EVIDENCE/calibrator.json" <<'PY'
import json, math, sys, time
now = int(time.time() * 1000)
json.dump({
    "schema": 1,
    "event": "spot_calibrator",
    "advisory_only": True,
    "generated_at_ms": now,
    "resolved_contracts": 371,
    "training_samples": 500,
    "brier_full": 0.1079,
    "brier_market_baseline": 0.1083,
    "adds_value_over_market": True,
    "predictions": {
        "KXBTC15M-E2ELIVE-15": {
            "p_yes_full": 0.98,
            "p_yes_market_baseline": 0.83,
            "market_yes_mid": 0.83,
            "features": {
                "signed_distance_bps": 120.0,
                "per_min_vol_bps": 8.0,
                "sqrt_minutes_left": math.sqrt(10.0),
                "required_move_sigma": 1.5,
                "realized_move_bps": 0.0,
                "yes_mid": 0.83,
            },
        }
    },
}, open(sys.argv[1], "w"))
PY
}

# grep -c exits 1 on zero matches, so the count is taken with the exit status
# ignored rather than through `||`, which would print a second number.
count_matches() {
  [ -f "$LEDGER" ] || { echo 0; return; }
  local n
  n="$(grep -c "$1" "$LEDGER" || true)"
  echo "${n:-0}"
}

# Nothing in this rung may arm anything. Asserted after EVERY refusal, not once
# at the end: a run that armed and disarmed would still have armed.
assert_nothing_armed() {
  [ ! -f "$SESSION" ] || fail "$1: a live session file appeared — the bot armed something"
  local settings
  settings="$("$CLI" --json kalshi auto live status 2>/dev/null || true)"
  case "$settings" in
    *'"session_active":true'*) fail "$1: the live session reads active after a refusal" ;;
  esac
}

write_report

# --- 1. no armed session: refused, non-zero, journaled ------------------------
set +e
"$CLI" kalshi bot run --mode live --interval 1 --iterations 3 >/dev/null 2>"$EVIDENCE/live1.err"
RC=$?
set -e
[ "$RC" -ne 0 ] || fail "live mode exited 0 with no armed session"
grep -q "LIVE REFUSED" "$EVIDENCE/live1.err" || fail "no refusal printed: $(cat "$EVIDENCE/live1.err")"
grep -q "LIVE_REFUSED_NOT_ARMED" "$EVIDENCE/live1.err" || fail "wrong refusal: $(cat "$EVIDENCE/live1.err")"
[ "$(count_matches '"reason_code":"LIVE_REFUSED_NOT_ARMED"')" -ge 1 ] \
  || fail "the refusal was not journaled"
[ "$(count_matches '"action":"bid"')" -eq 0 ] || fail "a bid was journaled on a refused live run"
# A refusal describes no trade: no ticker, no price, no size, no cap.
grep 'LIVE_REFUSED_NOT_ARMED' "$LEDGER" | grep -q '"price"' \
  && fail "the refusal row quotes a price it never had"
assert_nothing_armed "unarmed live run"
echo "ok: live mode refused (rc $RC), journaled with no numbers, armed nothing"

# --- 2. the kill switch outranks the arm check --------------------------------
rm -f "$LEDGER"
"$CLI" kalshi bot stop --reason "e2e live refusal test" >/dev/null
[ -f "$STOP" ] || fail "kalshi bot stop wrote no $STOP"
set +e
"$CLI" kalshi bot run --mode live --interval 1 --iterations 3 >/dev/null 2>"$EVIDENCE/live2.err"
RC=$?
set -e
[ "$RC" -ne 0 ] || fail "live mode exited 0 with the kill switch engaged"
grep -q "LIVE_REFUSED_BOT_STOPPED" "$EVIDENCE/live2.err" \
  || fail "the kill switch did not outrank the arm check: $(cat "$EVIDENCE/live2.err")"
[ "$(count_matches '"reason_code":"LIVE_REFUSED_BOT_STOPPED"')" -eq 1 ] \
  || fail "the stopped live run did not halt within one tick"
[ "$(count_matches '"action":"bid"')" -eq 0 ] || fail "a bid was journaled with the switch engaged"
assert_nothing_armed "stopped live run"
echo "ok: the kill switch halts live bidding within one tick, before the arm is considered"

"$CLI" kalshi bot resume >/dev/null
[ ! -f "$STOP" ] || fail "resume left $STOP behind"

# --- 3. paper is still the default (positive control) -------------------------
rm -f "$LEDGER"
write_report
"$CLI" kalshi bot once >/dev/null
[ "$(count_matches '"action":"bid"')" -ge 1 ] \
  || fail "the default tick placed no bid; sections 1 and 2 would prove nothing"
[ "$(count_matches '"mode":"paper"')" -ge 1 ] || fail "the default tick did not journal mode=paper"
[ "$(count_matches '"mode":"live"')" -eq 0 ] || fail "the default tick journaled a live row"
echo "ok: paper is the default and the same report DOES bid on paper"

# --- 4. an unknown mode is refused, not silently downgraded -------------------
set +e
"$CLI" kalshi bot once --mode sortof-live >/dev/null 2>"$EVIDENCE/live3.err"
RC=$?
set -e
[ "$RC" -eq 2 ] || fail "an unknown mode exited $RC; it must be a usage error"
grep -q "unknown mode" "$EVIDENCE/live3.err" || fail "no clear reason: $(cat "$EVIDENCE/live3.err")"
echo "ok: an unknown mode is refused"

# --- 4b. --paper and --mode live contradict; the conflict is refused ----------
# The launchd job's whole safety story is the --paper string. A --mode live that
# silently won over it would escalate a paper job to live.
set +e
"$CLI" kalshi bot once --paper --mode live >/dev/null 2>"$EVIDENCE/live4.err"
RC=$?
set -e
[ "$RC" -eq 2 ] || fail "--paper --mode live exited $RC; the conflict must be a usage error"
grep -q "contradict" "$EVIDENCE/live4.err" || fail "no clear reason: $(cat "$EVIDENCE/live4.err")"
[ "$(count_matches '"mode":"live"')" -eq 0 ] || fail "the contradictory run journaled a live row"
echo "ok: --paper --mode live is refused rather than silently escalated"

# --- 5. the launchd job never asks for live -----------------------------------
PLIST="$(cd "$(dirname "$0")/../scripts/deploy" && pwd)/org.openterminal.kalshi-bot.plist"
[ -f "$PLIST" ] || fail "no $PLIST"
grep -q "<string>--paper</string>" "$PLIST" || fail "the launchd job does not pass --paper"
grep -q "<string>--mode</string>" "$PLIST" && fail "the launchd job passes --mode"
echo "ok: the launchd job runs --paper and never --mode live"

echo "PASS: e2e_kalshi_bot_live_refusal"
