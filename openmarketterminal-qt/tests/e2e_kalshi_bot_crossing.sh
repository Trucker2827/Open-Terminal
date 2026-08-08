#!/bin/bash
# e2e_kalshi_bot_crossing.sh — issue #158, pay-to-fill quoting proved against
# the REAL openterminalcli, offline, no GUI and no daemon.
#
# The unit tests prove KalshiBotDecision::decide() picks the tier correctly.
# They cannot prove the thing the issue is actually about — that a crossing
# quote FILLS where a resting one does not — because that happens in the
# lifecycle pass, on a later tick, through the ledger. So this does:
#
#   1. control: the SAME two contracts, from a report carrying NO book, both
#      rest at floor(mid) and neither fills. This is the behaviour of every
#      build before this rung, and it is what the crossing tier is measured
#      against — without it a fill below would prove nothing;
#   2. with the book: the tight-spread contract CROSSES (quoted at the ask,
#      above the mid) and the wide-spread one RESTS at the same mid it always
#      did — the spread alone decides, both have the same edge and the same mid;
#   3. the crossing order fills on the next tick's lifecycle pass, at the price
#      it PAID (its own limit), never at the mid — and it becomes a position;
#   4. the resting order does not fill, and TTL-cancels exactly as before.
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
TIGHT="KXBTC15M-E2ECROSS-TIGHT"
WIDE="KXBTC15M-E2ECROSS-WIDE"

fail() { echo "FAIL: $*"; exit 1; }

# Two contracts with the SAME calibrated probability (0.98) and the SAME market
# mid (0.835) — identical edge, identical passive price. They differ in exactly
# one thing: the spread that has to be crossed to be filled now.
#
#   TIGHT  bid 0.82 / ask 0.85 → crossing costs 1.5c of spread + 1c of fee,
#                                and 0.145 of edge covers that with room;
#   WIDE   bid 0.71 / ask 0.96 → crossing costs 12.5c + 1c, which 0.145 of
#                                edge does not cover, so it rests at 0.83.
#
# `--with-book no` writes the same report WITHOUT the book fields: the shape
# every calibrator build before this rung wrote.
write_report() {
  python3 - "$EVIDENCE/calibrator.json" "${1:-yes}" "$TIGHT" "$WIDE" <<'PY'
import json, math, sys, time
path, with_book, tight, wide = sys.argv[1:5]

def contract(bid, ask):
    entry = {
        "p_yes_full": 0.98,
        "p_yes_market_baseline": 0.835,
        "market_yes_mid": 0.835,
        "features": {
            "signed_distance_bps": 120.0,
            "per_min_vol_bps": 8.0,
            "sqrt_minutes_left": math.sqrt(10.0),
            "required_move_sigma": 1.5,
            "realized_move_bps": 0.0,
            "yes_mid": 0.835,
        },
    }
    if with_book == "yes":
        entry["market_yes_bid"] = bid
        entry["market_yes_ask"] = ask
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
    "adds_value_over_market": True,
    "predictions": {tight: contract(0.82, 0.85), wide: contract(0.71, 0.96)},
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
  python3 - "$LEDGER" "$1" "$2" <<'PY'
import json, sys
path, action, ticker = sys.argv[1:4]
n = 0
try:
    for line in open(path):
        try:
            row = json.loads(line)
        except ValueError:
            continue
        if row.get("action") == action and (not ticker or row.get("ticker") == ticker):
            n += 1
except IOError:
    pass
print(n)
PY
}

# TTL of 1s throughout, so part 4 can prove the cancel without a 3-minute
# sleep. The fill check runs BEFORE the TTL check by design, so this does not
# race the fill in part 3.
tick() { "$CLI" kalshi bot once --paper --quote-ttl-sec 1 >/dev/null; }

# --- 1. control: no book in the report → both contracts rest, nothing fills ---
write_report no
tick
[ "$(row_field bid "$TIGHT" quote_style)" = "rest" ] || fail "control: tight contract did not rest without book data"
[ "$(row_field bid "$TIGHT" quote_style_reason)" = "REST_NO_BOOK" ] || fail "control: rest was not attributed to the missing book"
[ "$(row_field bid "$TIGHT" price)" = "0.83" ] || fail "control: tight contract was not priced at floor(mid): $(row_field bid "$TIGHT" price)"
[ "$(row_field bid "$WIDE" price)" = "0.83" ] || fail "control: wide contract was not priced at floor(mid)"
write_report no
tick
[ "$(count_rows fill "")" -eq 0 ] || fail "control: a resting quote at floor(mid) filled; the fill in part 3 would prove nothing"
echo "ok: without book data both contracts rest at \$0.83 and neither fills"

# --- 2. with the book: the spread alone decides the tier ---------------------
rm -f "$LEDGER"
write_report yes
tick
[ "$(row_field bid "$TIGHT" quote_style)" = "cross" ] || fail "tight spread did not cross: $(row_field bid "$TIGHT" quote_style_reason)"
[ "$(row_field bid "$TIGHT" quote_style_reason)" = "CROSS_EDGE_CLEARS_COST" ] || fail "cross was not attributed to the cost arithmetic"
[ "$(row_field bid "$TIGHT" price)" = "0.85" ] || fail "crossing bid was not quoted AT the ask: $(row_field bid "$TIGHT" price)"
[ "$(row_field bid "$TIGHT" rest_price)" = "0.83" ] || fail "crossing bid did not record the passive price it declined"
[ "$(row_field bid "$WIDE" quote_style)" = "rest" ] || fail "wide spread crossed; the cost hurdle did not hold"
[ "$(row_field bid "$WIDE" quote_style_reason)" = "REST_EDGE_BELOW_COST" ] || fail "wide-spread rest was not attributed to the cost"
[ "$(row_field bid "$WIDE" price)" = "0.83" ] || fail "wide-spread bid moved off floor(mid)"
echo "ok: same edge, same mid — the tight spread crossed at \$0.85, the wide one rested at \$0.83"

# --- 3. the crossing order fills, at what it PAID ----------------------------
write_report yes
tick
[ "$(row_field fill "$TIGHT" reason_code)" = "FILLED_AT_LIMIT" ] || fail "the crossing quote did not fill on the next tick"
[ "$(row_field fill "$TIGHT" price)" = "0.85" ] || fail "the fill was not at the price paid: $(row_field fill "$TIGHT" price)"
[ "$(row_field fill "$TIGHT" observed_mid)" = "0.835" ] || fail "the fill did not record the mid it was observed against"
[ "$(row_field fill "$TIGHT" fill_model)" = "rung6_conditional_mid" ] || fail "the fill did not state its model"
# The disclosure has to be the CROSSING one. The passive sentence claims the
# report carries no book, that the mid is the ask proxy, and that the fill
# selects on the market having moved to the quote: three clauses this very row
# falsifies. Matched on the phrase only the crossing sentence contains, and
# refused on the phrase only the passive one contains.
case "$(row_field fill "$TIGHT" fill_rule)" in
  *"crossing tier"*) ;;
  *) fail "the crossing fill did not carry the crossing disclosure: $(row_field fill "$TIGHT" fill_rule)" ;;
esac
case "$(row_field fill "$TIGHT" fill_rule)" in
  *"ask proxy"*) fail "the crossing fill still carries the passive tier's disclosure" ;;
esac
echo "ok: the crossing fill discloses the crossing model, not the passive one"
[ "$(count_rows fill "$WIDE")" -eq 0 ] || fail "the resting quote filled at floor(mid); the model was not left alone"
OPEN="$("$CLI" --json kalshi bot once --paper --quote-ttl-sec 1 | python3 -c 'import json,sys; print(json.load(sys.stdin)["open_positions"])')"
[ "$OPEN" -ge 1 ] || fail "the fill did not become a position (open_positions=$OPEN)"
echo "ok: the crossing quote filled at \$0.85 — the price it paid, not the \$0.835 mid — and is a position"

# --- 4. the resting order TTL-cancels, exactly as before ---------------------
sleep 2
write_report yes
tick
[ "$(row_field cancel "$WIDE" reason_code)" = "CANCELED_TTL" ] || fail "the resting quote did not TTL-cancel: $(row_field cancel "$WIDE" reason_code)"
echo "ok: the resting quote never filled and TTL-cancelled as it always did"

echo "PASS: e2e_kalshi_bot_crossing"
