#!/bin/bash
# e2e_kalshi_bot_gate_cadence.sh — issue #167, proved against the REAL
# openterminalcli, offline, no GUI and no daemon.
#
# The bug this defends against: kalshi-bot-gate.json was written ONLY when a
# human ran `kalshi bot gate`. It sat five hours stale while the cockpit
# rendered its numbers as current. The verdict that decides paper->live must
# not depend on someone remembering to run a command.
#
# The unit tests prove the age rendering and the staleness roles in process.
# What they cannot prove is that the SHIPPED CLI re-evaluates on its own, so
# this does:
#
#   1. a paper tick — running AND stopped — writes the verdict file;
#   2. the tick's verdict is BYTE-IDENTICAL to the one `kalshi bot gate` writes
#      for the same record, apart from the timestamp: one writer, not two;
#   3. consecutive ticks are stable — same verdict, same criteria, same ledger
#      block, same record anchor, only `ts_ms` advancing. (Cadence turns the
#      anchor carry-forward from a few evaluations a day into 1440; a ratchet
#      in it would show up here as a RECORD_INCOMPLETE on tick 2.)
#   4. an automated evaluation is never SOFTER than a manual one: tampered
#      params under the loop cadence write the refusal and NO numbers;
#   5. `kalshi bot gate` still behaves identically — same lines, exit 0 when it
#      evaluated and 3 when it refused.
#
# Everything runs against a temp evidence dir via
# OPENTERMINAL_KALSHI_EVIDENCE_DIR (cli/ServeCommand.h's kalshi_evidence_path),
# so the operator's real ledger and sealed gate are never touched. PAPER only:
# no --mode live anywhere in this file, and a paper tick contacts no venue.
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
VERDICT="$EVIDENCE/kalshi-bot-gate.json"
PARAMS="$EVIDENCE/kalshi-bot-gate-params.json"

fail() { echo "FAIL: $*"; exit 1; }

# --- a small paper record: 4 bids, filled and settled over ~4 h --------------
NOW_MS=$(python3 -c 'import time; print(int(time.time()*1000))')
python3 - "$LEDGER" "$NOW_MS" <<'PY'
import json, sys
path, now = sys.argv[1], int(sys.argv[2])
span = 4 * 3600 * 1000
first = now - span
step = span // 4
rows = []
for i in range(4):
    ts = first + i * step
    ticker = f"KXBTC15M-E2E{i}"
    pid = f"{ticker}@{ts}"
    rows.append({"event": "kalshi_bot_decision", "ts_ms": ts, "mode": "paper",
                 "ticker": ticker, "action": "bid", "reason_code": "EDGE_CLEARS_THRESHOLD",
                 "calibrated_p": 0.75, "market_mid": 0.50, "side": "YES", "price": 0.50,
                 "contracts": 4, "stake_usd": 2.0, "fee_usd": 0.07, "quote_style": "rest",
                 "order_state": "resting", "ttl_ms": 120000, "position_id": pid})
    rows.append({"event": "kalshi_bot_decision", "ts_ms": ts + 1, "mode": "paper",
                 "ticker": ticker, "action": "fill", "reason_code": "FILLED_AT_LIMIT",
                 "position_id": pid, "contracts": 4, "price": 0.50, "fee_usd": 0.07,
                 "order_state": "filled"})
    won = i % 3 != 2
    rows.append({"event": "kalshi_bot_paper_settlement", "ts_ms": ts + 2, "mode": "paper",
                 "position_id": pid, "ticker": ticker, "side": "YES", "contracts": 4,
                 "stake_usd": 2.0, "fee_usd": 0.07,
                 "market_result": "YES" if won else "NO", "won": won,
                 "realized_pnl": 1.93 if won else -2.07})
with open(path, "w") as handle:
    for row in rows:
        handle.write(json.dumps(row) + "\n")
PY

"$CLI" kalshi bot gate seal '{"min_settled_bids":300,"max_drawdown_usd":5}' > /dev/null ||
    fail "could not seal the gate params"

# --- criterion 1: a RUNNING paper tick publishes the verdict on its own -----
# No calibrator.json exists here, so the tick journals one REPORT_MISSING pass
# and bids nothing — the safest possible tick, and still a full one.
[ -f "$VERDICT" ] && fail "a verdict existed before any tick or any gate command"
"$CLI" kalshi bot once > /dev/null || fail "kalshi bot once exited non-zero"
[ -f "$VERDICT" ] || fail "a paper tick published no $VERDICT — the cadence hook is not wired"
cp "$VERDICT" "$HELPERS/tick-1.json"

python3 - "$HELPERS/tick-1.json" "$NOW_MS" <<'PY'
import json, sys
out = json.load(open(sys.argv[1]))
now = int(sys.argv[2])
assert out["evaluated"] is True, out
assert out["verdict"] == "FAIL", out["verdict"]   # 4 settled, floor is 300
assert out["ledger"]["settled_bids"] == 4, out["ledger"]
# The tick's evaluation is dated NOW, not whenever a human last ran the CLI.
assert out["ts_ms"] >= now, (out["ts_ms"], now)
PY
[ $? -eq 0 ] || fail "the tick's verdict does not describe the record it scored"

# --- criterion 2: one writer — the tick's file and the CLI's are the same ----
"$CLI" kalshi bot gate > "$HELPERS/gate-1.txt" 2>&1 && rc=0 || rc=$?
[ $rc -eq 0 ] || fail "kalshi bot gate exited $rc on an evaluated verdict (expected 0)"
cp "$VERDICT" "$HELPERS/cli-1.json"
python3 - "$HELPERS/tick-1.json" "$HELPERS/cli-1.json" <<'PY'
import json, sys
tick = json.load(open(sys.argv[1]))
cli = json.load(open(sys.argv[2]))
# Only the evaluation time may differ. Every other byte of the verdict — the
# criteria, the ledger block, the sealed params, the gate id — must match, or
# the loop is running a second implementation.
for blob in (tick, cli):
    blob.pop("ts_ms", None)
    blob.pop("ts", None)
assert tick == cli, f"the tick's verdict differs from the CLI's:\n{tick}\n{cli}"
PY
[ $? -eq 0 ] || fail "the automated and the manual verdict are not the same computation"

# The `gate` command's own output is unchanged: verdict headline, the sealed
# gate line, the ledger counts, one line per criterion, and the file it wrote.
grep -q "^KALSHI BOT GATE · FAIL" "$HELPERS/gate-1.txt" || fail "the gate headline changed"
grep -q "  gate .* · sealed " "$HELPERS/gate-1.txt" || fail "the sealed-gate line is gone"
grep -q "settled 4 (3 won / 1 lost)" "$HELPERS/gate-1.txt" || fail "the ledger counts line is gone"
grep -q "\[NOT MET\] min_settled_bids  observed 4 · required 300" "$HELPERS/gate-1.txt" ||
    fail "the criterion lines changed"
grep -q "verdict written to $VERDICT" "$HELPERS/gate-1.txt" || fail "the written-to line is gone"

# --- criterion 3: consecutive ticks are stable ------------------------------
# The record does not change (no report, so no bid), so nothing but the
# timestamp may move. This is where a ratchet in the record-anchor
# carry-forward would surface as a RECORD_INCOMPLETE on the second tick.
for n in 2 3 4; do
    "$CLI" kalshi bot once > /dev/null || fail "kalshi bot once exited non-zero on tick $n"
    cp "$VERDICT" "$HELPERS/tick-$n.json"
done
python3 - "$HELPERS/tick-1.json" "$HELPERS/tick-2.json" "$HELPERS/tick-3.json" \
         "$HELPERS/tick-4.json" <<'PY'
import json, sys
blobs = [json.load(open(p)) for p in sys.argv[1:]]
stamps = [b["ts_ms"] for b in blobs]
for a, b in zip(stamps, stamps[1:]):
    assert b >= a, f"the evaluation time went backwards: {stamps}"
for b in blobs:
    assert b["verdict"] == "FAIL", b["verdict"]
    b.pop("ts_ms", None)
    b.pop("ts", None)
for other in blobs[1:]:
    assert other == blobs[0], f"a repeat tick changed the verdict:\n{blobs[0]}\n{other}"
PY
[ $? -eq 0 ] || fail "the verdict is not stable across consecutive ticks"

# --- criterion 1, the stopped path ------------------------------------------
# A stopped tick takes no venue action and adds nothing to the record — but the
# record it refuses to add to is the record the gate scores, so its verdict is
# just as current and must not be left to age.
"$CLI" kalshi bot stop --reason "e2e: no venue action in this test" > /dev/null ||
    fail "could not throw the kill switch"
rm -f "$VERDICT"
"$CLI" kalshi bot once > /dev/null || fail "a stopped kalshi bot once exited non-zero"
[ -f "$VERDICT" ] || fail "a stopped tick published no verdict"
python3 - "$VERDICT" <<'PY'
import json, sys
out = json.load(open(sys.argv[1]))
assert out["evaluated"] is True, out
assert out["ledger"]["settled_bids"] == 4, out["ledger"]
PY
[ $? -eq 0 ] || fail "the stopped tick's verdict does not describe the record"
"$CLI" kalshi bot resume > /dev/null || fail "could not clear the kill switch"

# --- criterion 4: automation is never softer than a human -------------------
# The seal is broken by editing the sealed params in place. A manual `gate`
# refuses this with TAMPERED and publishes no numbers; the loop must do exactly
# the same, or automation would have quietly become the soft path.
chmod u+w "$PARAMS"
python3 - "$PARAMS" <<'PY'
import json, sys
params = json.load(open(sys.argv[1]))
params["params"]["min_settled_bids"] = 1   # the edit the seal exists to catch
json.dump(params, open(sys.argv[1], "w"))
PY
rm -f "$VERDICT"
"$CLI" kalshi bot once > /dev/null || fail "a tick with tampered params exited non-zero"
[ -f "$VERDICT" ] || fail "a tick with tampered params published no refusal at all"
cp "$VERDICT" "$HELPERS/tampered-tick.json"
python3 - "$HELPERS/tampered-tick.json" <<'PY'
import json, sys
out = json.load(open(sys.argv[1]))
assert out["verdict"] == "TAMPERED", out["verdict"]
assert out["evaluated"] is False, out
# A refusal is not a verdict about the record: NO criteria, NO ledger block,
# and above all no PASS-shaped numbers from a gate that would have passed on
# the edited floor of 1.
assert "criteria" not in out, "a refusal published criteria"
assert "ledger" not in out, "a refusal published a ledger block"
assert out["reason"], "a refusal gave no reason"
PY
[ $? -eq 0 ] || fail "the automated evaluation was softer than a manual one"

"$CLI" kalshi bot gate > "$HELPERS/gate-tampered.txt" 2>&1 && rc=0 || rc=$?
[ $rc -eq 3 ] || fail "kalshi bot gate exited $rc on a refusal (expected 3)"
grep -q "^KALSHI BOT GATE · TAMPERED" "$HELPERS/gate-tampered.txt" ||
    fail "the manual refusal headline changed"
if grep -qE "\[MET\]|\[NOT MET\]|settled [0-9]+ \(" "$HELPERS/gate-tampered.txt"; then
    fail "a manual refusal printed criteria numbers"
fi
python3 - "$HELPERS/tampered-tick.json" "$VERDICT" <<'PY'
import json, sys
tick = json.load(open(sys.argv[1]))
cli = json.load(open(sys.argv[2]))
for blob in (tick, cli):
    blob.pop("ts_ms", None)
    blob.pop("ts", None)
assert tick == cli, f"the automated refusal differs from the manual one:\n{tick}\n{cli}"
PY
[ $? -eq 0 ] || fail "the automated and manual refusals are not identical"

echo "PASS: e2e_kalshi_bot_gate_cadence — cadence, one writer, stability, refusal parity"
