#!/bin/bash
# e2e_kalshi_bot_kill_switch.sh — issue #129, the kill switch proved against
# the REAL openterminalcli, offline, no GUI and no daemon.
#
# The unit tests prove KalshiBotDecision::decide() refuses to bid with the
# switch engaged. They cannot prove the two loop-level properties the issue
# actually asks for, so this does:
#
#   1. a bid the bot WOULD have placed is not placed once the stop file exists
#      (positive control first: the same calibrator report bids without it);
#   2. `kalshi bot run` sees a switch thrown while it is looping and exits
#      within ONE tick — not after --iterations, not never.
#
# Everything runs against a temp evidence dir via
# OPENTERMINAL_KALSHI_EVIDENCE_DIR (cli/ServeCommand.h's kalshi_evidence_path),
# so the operator's real ledger is never touched. Paper-only: `kalshi bot` has
# no live order path at all, so nothing here can reach an exchange.
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-$(cd "$(dirname "$0")/../build" && pwd)}"
CLI="${CLI:-$BUILD_DIR/openterminalcli}"
[ -x "$CLI" ] || { echo "FAIL: no openterminalcli at $CLI"; exit 1; }

EVIDENCE="$(mktemp -d)"
trap 'rm -rf "$EVIDENCE"' EXIT
export OPENTERMINAL_KALSHI_EVIDENCE_DIR="$EVIDENCE"

LEDGER="$EVIDENCE/kalshi-bot-decisions.jsonl"
STOP="$EVIDENCE/kalshi-bot-stop.json"

fail() { echo "FAIL: $*"; exit 1; }

# A calibrator report generated NOW (the bot refuses one >= 120s old) carrying
# one contract with a 15-point edge and 10 minutes of runway — enough to clear
# the resting tier's edge threshold plus its adverse-selection premium (#165) —
# the same shape
# spot_calibrator.py's build_report() writes.
write_report() {
  python3 - "$EVIDENCE/calibrator.json" <<'PY'
import json, math, sys, time
now = int(time.time() * 1000)
json.dump({
    "schema": 2,
    "event": "spot_calibrator",
    "advisory_only": True,
    "generated_at_ms": now,
    "resolved_contracts": 371,
    "scored_contracts": 244,
    "training_observations": 12049,
    "brier_full": 0.1079,
    "brier_market_mid_raw": 0.1083,
    "brier_market_trained_logit": 0.1101,
    "adds_value_over_market": True,
    "predictions": {
        "KXBTC15M-E2ETEST-15": {
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
count_rows() { count_matches "\"reason_code\":\"$1\""; }
count_bids() { count_matches '"action":"bid"'; }

# --- 1. positive control: this report DOES bid -------------------------------
write_report
"$CLI" kalshi bot once >/dev/null
BIDS_BEFORE="$(count_bids)"
[ "$BIDS_BEFORE" -ge 1 ] || fail "control tick placed no bid; the rest of this test would prove nothing"
echo "ok: control tick placed $BIDS_BEFORE bid(s)"

# The control bid is now an open position, which would pass ALREADY_HELD on a
# re-run. A fresh ledger keeps the stopped run comparable to the control.
rm -f "$LEDGER"

# --- 2. the switch refuses the bid -------------------------------------------
"$CLI" kalshi bot stop --reason "e2e kill switch test" >/dev/null
[ -f "$STOP" ] || fail "kalshi bot stop wrote no $STOP"

write_report
"$CLI" kalshi bot once >/dev/null
[ "$(count_bids)" -eq 0 ] || fail "a bid was placed with the kill switch engaged"
[ "$(count_rows BOT_STOPPED)" -ge 1 ] || fail "the stopped tick journaled no BOT_STOPPED row"
echo "ok: stopped tick placed 0 bids and journaled its refusal"

# --- 3. status mirrors the chip ----------------------------------------------
STATUS_JSON="$("$CLI" --json kalshi bot status)"
echo "$STATUS_JSON" | grep -q '"state":"stopped"' || fail "status did not read stopped: $STATUS_JSON"
echo "$STATUS_JSON" | grep -q '"color_role":"red"' || fail "stopped status is not red: $STATUS_JSON"
echo "$STATUS_JSON" | grep -q '"stale_after_ms":120000' || fail "status threshold is not the shared 2 intervals: $STATUS_JSON"
echo "ok: kalshi bot status reads stopped/red from the shared classifier"

# --- 4. the loop exits within one tick ---------------------------------------
# --interval 1 with 60 iterations: if the switch were only checked at startup,
# or seen but not acted on, this would run ~60s and journal 60 rows. It must
# return after the FIRST tick instead. The row count is the precise assertion;
# the 25s wall-clock bound is the coarse one, chosen to leave a slow CI runner
# plenty of headroom while still being nowhere near the ~60s a neutered exit
# takes (both were confirmed by neutering the loop's exit).
BEFORE="$(count_rows BOT_STOPPED)"
START="$(date +%s)"
set +e
"$CLI" kalshi bot run --paper --interval 1 --iterations 60 >/dev/null 2>&1
RUN_RC=$?
set -e
ELAPSED=$(( $(date +%s) - START ))
[ "$RUN_RC" -eq 0 ] || fail "run exited $RUN_RC on the kill switch; a clean stop must exit 0 (the launchd job's KeepAlive is Crashed-only)"
[ "$ELAPSED" -le 25 ] || fail "run took ${ELAPSED}s to honour the switch; one tick is 1s here"
[ "$(( $(count_rows BOT_STOPPED) - BEFORE ))" -eq 1 ] || fail "run journaled more than one refusal — it did not exit within one tick"
[ "$(count_bids)" -eq 0 ] || fail "run placed a bid with the kill switch engaged"
echo "ok: run exited 0 after exactly one refused tick (${ELAPSED}s)"

# --- 5. resume clears it ------------------------------------------------------
"$CLI" kalshi bot resume >/dev/null
[ ! -f "$STOP" ] || fail "resume left $STOP behind"
write_report
"$CLI" kalshi bot once >/dev/null
[ "$(count_bids)" -ge 1 ] || fail "the bot did not bid again after resume"
echo "ok: resume cleared the switch and the bot bids again"

echo "PASS: e2e_kalshi_bot_kill_switch"
