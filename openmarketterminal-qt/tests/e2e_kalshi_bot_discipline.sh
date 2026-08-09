#!/bin/bash
# e2e_kalshi_bot_discipline.sh — issue #165, proved against the REAL
# openterminalcli, offline, no GUI and no daemon.
#
# The unit tests prove KalshiBotDecision::decide() refuses. What they cannot
# prove is that the SHIPPED binary refuses, and that the refusal leaves nothing
# in the BOOK the next tick replays — which is the criterion as written ("zero
# orders in the book"), and which only the ledger can answer. So this does:
#
#   a. untrusted signal, huge edge: the tick places NO order, journals one
#      SIGNAL_UNTRUSTED pass, and the tick summary reports 0 resting orders and
#      $0.00 exposure. The positive control is the SAME report with the
#      calibrator's own verdict flipped to true — it bids — so the refusal
#      cannot be blamed on the fixture;
#   b. trusted signal, edge above the base threshold but below
#      threshold + rest_premium: the bookless contract PASSES with
#      REST_EDGE_BELOW_PREMIUM and the tight-book contract with the SAME edge
#      CROSSES. The asymmetry is the point — crossing may fire where resting
#      may not — and both rows carry the arithmetic they were judged against.
#
# Everything runs against a temp evidence dir via
# OPENTERMINAL_KALSHI_EVIDENCE_DIR (cli/ServeCommand.h's kalshi_evidence_path),
# so the operator's real ledger is never touched. Paper-only: no --mode live
# anywhere here, so nothing can reach an exchange.
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-$(cd "$(dirname "$0")/../build" && pwd)}"
CLI="${CLI:-$BUILD_DIR/openterminalcli}"
[ -x "$CLI" ] || { echo "FAIL: no openterminalcli at $CLI"; exit 1; }

EVIDENCE="$(mktemp -d)"
trap 'rm -rf "$EVIDENCE"' EXIT
export OPENTERMINAL_KALSHI_EVIDENCE_DIR="$EVIDENCE"

LEDGER="$EVIDENCE/kalshi-bot-decisions.jsonl"
THIN="KXBTC15M-E2EDISC-THIN"
TIGHT="KXBTC15M-E2EDISC-TIGHT"

fail() { echo "FAIL: $*"; exit 1; }

# Two contracts with the SAME calibrated probability (0.95) and the SAME market
# mid (0.83) — an edge of 0.12, which is past the 0.10 base threshold and short
# of the 0.13 a rest now demands. They differ in one thing:
#
#   THIN   no book at all → it can only rest → REST_EDGE_BELOW_PREMIUM;
#   TIGHT  bid 0.82 / ask 0.84 → 1c of spread + 1c of fee, which 0.12 of edge
#          clears with the $0.02 margin to spare → it CROSSES at 0.84.
#
# `$1` is the calibrator's own adds_value_over_market verdict.
write_report() {
  python3 - "$EVIDENCE/calibrator.json" "${1:-True}" "$THIN" "$TIGHT" <<'PY'
import json, math, sys, time
path, trusted, thin, tight = sys.argv[1:5]

def contract(book):
    entry = {
        "p_yes_full": 0.95,
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
    if book:
        entry["market_yes_bid"] = 0.82
        entry["market_yes_ask"] = 0.84
    return entry

json.dump({
    "schema": 2,
    "event": "spot_calibrator",
    "advisory_only": True,
    "generated_at_ms": int(time.time() * 1000),
    "resolved_contracts": 371,
    "scored_contracts": 244,
    "training_observations": 12049,
    "brier_full": 0.1079,
    "brier_market_mid_raw": 0.1083,
    "brier_market_trained_logit": 0.1101,
    "adds_value_over_market": trusted == "True",
    "adds_value_on_bet_eligible": trusted == "True",
    "brier_eligible_full": 0.2130 if trusted == "True" else 0.2576,
    "brier_eligible_market_mid_raw": 0.2576 if trusted == "True" else 0.2130,
    "predictions": {thin: contract(False), tight: contract(True)},
}, open(path, "w"))
PY
  # The KXBTC15M family now has its own calibrator: the bot filters 15-minute
  # contracts OUT of calibrator.json (filter_predictions_for_family with
  # keep_kxbtc15m=false) and reads them from kxbtc15m-calibrator.json instead.
  # These fixtures predate that split, so the same report has to reach both
  # families or the control tick sees NO_PREDICTIONS and proves nothing.
  cp "$EVIDENCE/calibrator.json" "$EVIDENCE/kxbtc15m-calibrator.json"
}

# One field off the newest ledger row matching an action and a ticker. Prints
# nothing when there is no such row, so a missing row fails an assertion
# instead of silently comparing empty to empty.
row_field() {
  python3 - "$LEDGER" "$1" "$2" "$3" <<'PY'
import json, sys
path, action, ticker, field = sys.argv[1:5]
found = None
try:
    for line in open(path):
        try:
            row = json.loads(line)
        except ValueError:
            continue
        if row.get("action") == action and row.get("ticker") == ticker:
            found = row
except IOError:
    pass
if found is not None and field in found:
    print(found[field])
PY
}

count_rows() {
  python3 - "$LEDGER" "$1" <<'PY'
import json, sys
path, action = sys.argv[1:3]
n = 0
try:
    for line in open(path):
        try:
            row = json.loads(line)
        except ValueError:
            continue
        if row.get("action") == action:
            n += 1
except IOError:
    pass
print(n)
PY
}

# One field off a --json tick summary, printed as a number so an int 0 and a
# float 0.0 do not compare differently.
summary_field() {
  "$CLI" --json kalshi bot once --paper |
    python3 -c "import json,sys; print(float(json.load(sys.stdin)[sys.argv[1]]))" "$1"
}

# A numeric field off a ledger row, compared AS A NUMBER: JSON round-trips
# doubles, and 0.12 does not print as "0.12" once it has been through one.
row_num_is() {
  python3 - "$LEDGER" "$1" "$2" "$3" "$4" <<'NUM'
import json, sys
path, action, ticker, field, expected = sys.argv[1:6]
found = None
try:
    for line in open(path):
        try:
            row = json.loads(line)
        except ValueError:
            continue
        if row.get("action") == action and row.get("ticker") == ticker:
            found = row
except IOError:
    pass
value = None if found is None else found.get(field)
sys.exit(0 if isinstance(value, (int, float)) and not isinstance(value, bool)
         and abs(value - float(expected)) < 1e-9 else 1)
NUM
}

tick() { "$CLI" kalshi bot once --paper >/dev/null; }

# --- a. an untrusted signal places no order ----------------------------------
# Control first: the same report, trusted, DOES put orders in the book.
write_report True
tick
[ "$(count_rows bid)" -ge 1 ] || fail "control: the trusted report placed no bid; the refusal below would prove nothing"
echo "ok: control — the trusted report placed $(count_rows bid) bid(s)"

rm -f "$LEDGER"
write_report False
tick
[ "$(count_rows bid)" -eq 0 ] || fail "a bid was placed on an untrusted signal"
[ "$(row_field pass "" reason_code)" = "SIGNAL_UNTRUSTED" ] || fail "the untrusted tick did not journal SIGNAL_UNTRUSTED: $(row_field pass "" reason_code)"
[ "$(row_field pass "" signal_trusted)" = "False" ] || fail "the refusal row did not carry the trust verdict"
# The refusal invented no price: the contracts were never priced.
[ -z "$(row_field pass "" price)" ] || fail "the refusal row carried a price"
# The criterion as written: ZERO orders in the BOOK, read off the shipped
# binary's own tick summary rather than inferred from the rows.
write_report False
RESTING="$(summary_field resting_orders)"
EXPOSURE="$(summary_field exposure_usd)"
[ "$RESTING" = "0.0" ] || fail "an untrusted tick left $RESTING order(s) resting"
[ "$EXPOSURE" = "0.0" ] || fail "an untrusted tick left \$$EXPOSURE of exposure"
# And the human readout says so rather than claiming a labelled bid.
write_report False
"$CLI" kalshi bot once --paper | grep -q "UNTRUSTED — the tick placed NO bid" ||
    fail "the CLI did not report the untrusted tick as placing no bid"
echo "ok: an untrusted signal bids nothing, rests nothing, and says so"

# --- b. the resting premium, and the asymmetry it creates --------------------
rm -f "$LEDGER"
write_report True
tick
[ "$(row_field pass "$THIN" reason_code)" = "REST_EDGE_BELOW_PREMIUM" ] || fail "the thin resting edge was not refused: $(row_field pass "$THIN" reason_code)"
[ "$(row_field pass "$THIN" quote_style)" = "rest" ] || fail "the refused contract did not name the tier it was pricing"
# The hurdle arithmetic, on the row: edge, threshold, premium, and their sum.
row_num_is pass "$THIN" side_edge 0.12 || fail "the rest row did not journal the edge it was judged on: $(row_field pass "$THIN" side_edge)"
row_num_is pass "$THIN" edge_threshold 0.10 || fail "the rest row did not journal the base threshold"
row_num_is pass "$THIN" rest_premium_usd 0.03 || fail "the rest row did not journal the premium"
row_num_is pass "$THIN" rest_threshold 0.13 || fail "the rest row did not journal threshold + premium: $(row_field pass "$THIN" rest_threshold)"
# The SAME edge, on a contract whose book makes it marketable, still bids —
# crossing may fire exactly where resting may not.
[ "$(row_field bid "$TIGHT" quote_style)" = "cross" ] || fail "the tight-book contract did not cross: $(row_field bid "$TIGHT" quote_style_reason)"
row_num_is bid "$TIGHT" price 0.84 || fail "the crossing bid was not quoted at the ask: $(row_field bid "$TIGHT" price)"
[ "$(row_field bid "$TIGHT" side_edge)" = "$(row_field pass "$THIN" side_edge)" ] || fail "the two contracts did not have the same edge; the asymmetry would prove nothing"
# The premium is the resting tier's hurdle and is not claimed of a cross.
[ -z "$(row_field bid "$TIGHT" rest_threshold)" ] || fail "the crossing row claimed a resting hurdle it was never judged against"
echo "ok: same edge — the bookless contract passed on the premium, the tight-book one crossed"

# The premium is configuration, not a constant: shrink it to a tenth of a cent
# — the CLI's money flags refuse a literal 0, as they do for --cross-margin —
# and the SAME contract rests at floor(mid) exactly as it did before this rung.
# So the refusal above is the premium's doing and nothing else's.
rm -f "$LEDGER"
write_report True
"$CLI" kalshi bot once --paper --rest-premium 0.001 >/dev/null
[ "$(row_field bid "$THIN" quote_style)" = "rest" ] || fail "a shrunken --rest-premium did not restore the resting bid"
row_num_is bid "$THIN" price 0.83 || fail "the restored resting bid was not at floor(mid)"
row_num_is bid "$THIN" rest_threshold 0.101 || fail "the restored bid did not journal the configured hurdle"
echo "ok: --rest-premium 0.001 restores the pre-#165 resting bid, so the refusal is the premium's doing"

echo "PASS: e2e_kalshi_bot_discipline"
