#!/bin/bash
# e2e_kalshi_bot_funnel.sh — issue #153, criteria 3, 4 and the CLI half of 5,
# proved against the REAL openterminalcli, offline, no GUI and no daemon.
#
# The unit tests prove the aggregator and the formatter in process, and they
# prove the BOT panel renders the formatter's output verbatim. What they cannot
# prove is that the SHIPPED CLI is the thing that publishes and renders it, so
# this does:
#
#   3. a paper tick writes kalshi-bot-funnel.json over the record it replayed;
#   4. `kalshi bot status` (human) and `--json kalshi bot status` both render
#      that file — the counts, the fill rate WITH its denominator, the settled
#      rate WITH the span it was measured over, the pace to the sealed gate, and
#      the fill model printed from the ledger's own value. An absent file
#      renders FUNNEL UNAVAILABLE and NO counts; a file older than two tick
#      intervals renders its age beside every number;
#   5. the human lines and the JSON `funnel_lines` array are IDENTICAL, because
#      both come from kalshi_bot_funnel_lines() — the same function the BOT
#      panel's `view.funnel` is asserted to equal in
#      tst_kalshi_bot_panel_presentation. One formatter, two renderers.
#
# Everything runs against a temp evidence dir via
# OPENTERMINAL_KALSHI_EVIDENCE_DIR (cli/ServeCommand.h's kalshi_evidence_path),
# so the operator's real ledger and sealed gate are never touched. The kill
# switch is thrown before the tick: a stopped tick takes no venue action at all,
# still replays the record, and still publishes the funnel — which is both the
# safest way to run this and the stopped-path publish under test.
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-$(cd "$(dirname "$0")/../build" && pwd)}"
CLI="${CLI:-$BUILD_DIR/openterminalcli}"
[ -x "$CLI" ] || { echo "FAIL: no openterminalcli at $CLI"; exit 1; }

EVIDENCE="$(mktemp -d)"
HELPERS="$(mktemp -d)"
# The sealed params file is written 0444; restore write permission before rm.
trap 'chmod -R u+w "$EVIDENCE" 2>/dev/null; rm -rf "$EVIDENCE" "$HELPERS"' EXIT
export OPENTERMINAL_KALSHI_EVIDENCE_DIR="$EVIDENCE"

LEDGER="$EVIDENCE/kalshi-bot-decisions.jsonl"
FUNNEL="$EVIDENCE/kalshi-bot-funnel.json"

fail() { echo "FAIL: $*"; exit 1; }

# --- criterion 4, the absent case, BEFORE anything is published --------------
"$CLI" kalshi bot status > "$HELPERS/status-absent.txt" 2>&1 ||
    fail "kalshi bot status exited non-zero with no funnel published"
grep -q "FUNNEL UNAVAILABLE" "$HELPERS/status-absent.txt" ||
    fail "an absent funnel file did not render FUNNEL UNAVAILABLE"
if grep -qE "FILL RATE ·|SETTLEMENT RATE ·|PACE TO THE SEALED GATE ·" \
        "$HELPERS/status-absent.txt"; then
    fail "an absent funnel file rendered numbers anyway"
fi

"$CLI" --json kalshi bot status > "$HELPERS/status-absent.json" ||
    fail "--json kalshi bot status exited non-zero with no funnel published"
python3 - "$HELPERS/status-absent.json" <<'PY'
import json, sys
out = json.load(open(sys.argv[1]))
assert out["funnel_available"] is False, out
# Not zeroed — ABSENT. A script must not be able to read a 0 fill rate off a
# record no reader ever saw.
assert "funnel" not in out, "an unavailable funnel published a funnel object"
assert out["funnel_unavailable_reason"], "no reason given for the unavailable funnel"
assert len(out["funnel_lines"]) == 1, out["funnel_lines"]
PY
[ $? -eq 0 ] || fail "the --json absent case is wrong"

# --- a record with a KNOWN funnel: 40 bids, 6 fills, 4 settled over 48 h -----
# 34 quotes expire on TTL — the operator's own shape, where the market never
# comes to ~95% of the bids.
NOW_MS=$(python3 -c 'import time; print(int(time.time()*1000))')
python3 - "$LEDGER" "$NOW_MS" <<'PY'
import json, sys
path, now = sys.argv[1], int(sys.argv[2])
span = 48 * 3600 * 1000
first = now - span
step = span // 39
rule = ("paper: calibrator.json carries no book, so its market mid is the ask proxy — a resting "
        "limit fills only once an observed mid is at or through the limit")
rows = []
for i in range(40):
    ts = first + i * step
    ticker = f"KXBTC15M-E2E{i}"
    pid = f"{ticker}@{ts}"
    rows.append({"event": "kalshi_bot_decision", "ts_ms": ts, "mode": "paper",
                 "ticker": ticker, "action": "bid", "reason_code": "EDGE_CLEARS_THRESHOLD",
                 "calibrated_p": 0.75, "market_mid": 0.50, "side": "YES", "price": 0.50,
                 "contracts": 4, "stake_usd": 2.0, "fee_usd": 0.07,
                 "order_state": "resting", "ttl_ms": 120000, "position_id": pid})
    if i < 6:
        rows.append({"event": "kalshi_bot_decision", "ts_ms": ts + 1, "mode": "paper",
                     "ticker": ticker, "action": "fill", "reason_code": "FILLED_AT_LIMIT",
                     "position_id": pid, "contracts": 4, "price": 0.50, "fee_usd": 0.07,
                     "order_state": "filled", "fill_model": "rung6_conditional_mid",
                     "fill_rule": rule})
        if i < 4:
            won = i % 3 != 2
            rows.append({"event": "kalshi_bot_paper_settlement", "ts_ms": ts + 2,
                         "mode": "paper", "position_id": pid, "ticker": ticker,
                         "side": "YES", "contracts": 4, "stake_usd": 2.0, "fee_usd": 0.07,
                         "market_result": "YES" if won else "NO", "won": won,
                         "realized_pnl": 1.93 if won else -2.07})
    else:
        rows.append({"event": "kalshi_bot_decision", "ts_ms": ts + 1, "mode": "paper",
                     "ticker": ticker, "action": "cancel", "reason_code": "CANCELED_TTL",
                     "position_id": pid, "contracts": 4, "order_state": "cancelled"})
with open(path, "w") as handle:
    for row in rows:
        handle.write(json.dumps(row) + "\n")
PY

# The sealed gate the pace is measured against. 300 is the ladder's floor and
# this test never touches it — issue #153 REPORTS the pace, it does not move it.
"$CLI" kalshi bot gate seal '{"min_settled_bids":300,"max_drawdown_usd":5}' > /dev/null ||
    fail "could not seal the gate params"

# --- criterion 3: a tick publishes the funnel -------------------------------
"$CLI" kalshi bot stop --reason "e2e: no venue action in this test" > /dev/null ||
    fail "could not throw the kill switch"
if [ -f "$FUNNEL" ]; then fail "a funnel existed before any tick ran"; fi
"$CLI" kalshi bot once > /dev/null || fail "kalshi bot once exited non-zero"
[ -f "$FUNNEL" ] || fail "the tick published no $FUNNEL"

python3 - "$FUNNEL" <<'PY'
import json, sys
funnel = json.load(open(sys.argv[1]))
assert funnel["bids"] == 40, funnel["bids"]
assert funnel["fills"] == 6, funnel["fills"]
assert funnel["settlements"] == 4, funnel["settlements"]
assert funnel["canceled_ttl"] == 34, funnel["canceled_ttl"]
assert funnel["resting_now"] == 0, funnel["resting_now"]
assert abs(funnel["fill_rate"] - 6 / 40) < 1e-9, funnel["fill_rate"]
# 4 settlements over ~48 h is ~2/day, so 296 more at that rate is ~148 days.
assert 1.9 < funnel["settled_per_day"] < 2.1, funnel["settled_per_day"]
assert funnel["gate_required"] == 300, funnel["gate_required"]
assert funnel["settled_remaining"] == 296, funnel["settled_remaining"]
assert 140 < funnel["days_to_gate_at_observed_rate"] < 160, funnel
assert funnel["fill_models"] == ["rung6_conditional_mid"], funnel["fill_models"]
assert funnel["span_ms"] > 47 * 3600 * 1000, funnel["span_ms"]
assert funnel["rows_read"] > 80, funnel["rows_read"]
PY
[ $? -eq 0 ] || fail "the published funnel does not match the record it measured"

# --- criterion 4: the CLI renders it, human and JSON ------------------------
"$CLI" kalshi bot status > "$HELPERS/status.txt" 2>&1 || fail "kalshi bot status exited non-zero"
grep -q "FUNNEL · 40 bids → 6 fills → 4 settled" "$HELPERS/status.txt" ||
    fail "the human status did not render the funnel counts"
grep -q "6 of 40 bids filled" "$HELPERS/status.txt" ||
    fail "the fill rate was rendered without its denominator"
grep -q "settled per day" "$HELPERS/status.txt" ||
    fail "the settled/day rate is missing"
grep -q "of record" "$HELPERS/status.txt" ||
    fail "the settled/day rate was rendered without the span it was measured over"
grep -q "296 more settled bids needed of 300" "$HELPERS/status.txt" ||
    fail "the pace to the sealed gate is missing"
grep -q "FILL MODEL · rung6_conditional_mid" "$HELPERS/status.txt" ||
    fail "the fill model the record was selected by is not named"

"$CLI" --json kalshi bot status > "$HELPERS/status.json" ||
    fail "--json kalshi bot status exited non-zero"

# --- criterion 5 (CLI leg): human text and JSON come from ONE formatter -----
# The panel's `view.funnel` is asserted equal to kalshi_bot_funnel_lines() in
# tst_kalshi_bot_panel_presentation; this pins the CLI to the same output, so
# the window and the terminal cannot disagree.
python3 - "$HELPERS/status.json" "$HELPERS/status.txt" <<'PY'
import json, sys
out = json.load(open(sys.argv[1]))
human = open(sys.argv[2], encoding="utf-8").read().splitlines()
assert out["funnel_available"] is True, out
lines = out["funnel_lines"]
assert len(lines) == 5, lines
assert out["funnel"]["bids"] == 40, out["funnel"]
for line in lines:
    if "  " + line not in human:
        raise AssertionError(f"the human status does not carry the formatter's line: {line!r}")
PY
[ $? -eq 0 ] || fail "the human and --json funnel lines are not the same formatter's output"

# --- criterion 4: a stale funnel carries its age beside every number --------
# Two tick intervals is kKalshiBotStaleMs, the same threshold the status chip
# goes stale on. The file's own ts_ms is aged; the record is untouched.
python3 - "$FUNNEL" <<'PY'
import json, sys
funnel = json.load(open(sys.argv[1]))
funnel["ts_ms"] -= 10 * 60 * 1000
json.dump(funnel, open(sys.argv[1], "w"))
PY
"$CLI" --json kalshi bot status > "$HELPERS/status-stale.json" ||
    fail "--json kalshi bot status exited non-zero on a stale funnel"
python3 - "$HELPERS/status-stale.json" <<'PY'
import json, sys
out = json.load(open(sys.argv[1]))
lines = out["funnel_lines"]
assert len(lines) == 5, lines
for line in lines:
    assert "· as of" in line, f"a stale funnel line carries no age: {line!r}"
assert out["funnel_age_ms"] >= 10 * 60 * 1000, out["funnel_age_ms"]
PY
[ $? -eq 0 ] || fail "a stale funnel did not carry its age beside every number"

# --- criterion 4: an unparseable file renders no numbers at all -------------
printf '{"bids": 40, not json' > "$FUNNEL"
"$CLI" kalshi bot status > "$HELPERS/status-broken.txt" 2>&1 ||
    fail "kalshi bot status exited non-zero on an unparseable funnel"
grep -q "FUNNEL UNAVAILABLE" "$HELPERS/status-broken.txt" ||
    fail "an unparseable funnel did not render FUNNEL UNAVAILABLE"
if grep -q "40 bids" "$HELPERS/status-broken.txt"; then
    fail "an unparseable funnel rendered a number out of the bytes on disk"
fi

echo "PASS: e2e_kalshi_bot_funnel — publish, render, parity, staleness, refusal"
