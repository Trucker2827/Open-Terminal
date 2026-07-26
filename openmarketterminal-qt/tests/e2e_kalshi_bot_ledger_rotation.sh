#!/bin/bash
# e2e_kalshi_bot_ledger_rotation.sh — issue #152, criteria 4 and 5 proved
# against the REAL openterminalcli, offline, no GUI and no daemon.
#
# The unit tests prove the reader, the rotation and the gate's refusal in
# process. They cannot prove that `openterminalcli --json kalshi bot gate` is
# the thing that reads every generation, so this does:
#
#   4. the gate scores the WHOLE record — the same settled_bids / wins /
#      losses / net_pnl_usd / max_drawdown_usd before and after the record is
#      split into two generations;
#   5. a record the gate can see is incomplete is REFUSED, not scored, and the
#      CLI exits non-zero: (a) a hole in the generation sequence, (b) a
#      contiguous record whose oldest row postdates the first settlement the
#      published verdict already scored, (c) the whole record again → a normal
#      verdict with the original numbers.
#
# Everything runs against a temp evidence dir via
# OPENTERMINAL_KALSHI_EVIDENCE_DIR (cli/ServeCommand.h's kalshi_evidence_path),
# so the operator's real ledger and sealed gate are never touched. `kalshi bot
# gate` only reads files and writes its verdict; nothing here can reach an
# exchange.
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-$(cd "$(dirname "$0")/../build" && pwd)}"
CLI="${CLI:-$BUILD_DIR/openterminalcli}"
[ -x "$CLI" ] || { echo "FAIL: no openterminalcli at $CLI"; exit 1; }

EVIDENCE="$(mktemp -d)"
HELPERS="$(mktemp -d)"
# The sealed params file is deliberately written 0444, so the cleanup restores
# write permission before removing the directory.
trap 'chmod -R u+w "$EVIDENCE" 2>/dev/null; rm -rf "$EVIDENCE" "$HELPERS"' EXIT
export OPENTERMINAL_KALSHI_EVIDENCE_DIR="$EVIDENCE"

LEDGER="$EVIDENCE/kalshi-bot-decisions.jsonl"

fail() { echo "FAIL: $*"; exit 1; }

# --- a small paper record: 6 settled bids, 4 won and 2 lost ------------------
# Row shape is rung 1's own; ts_ms increases monotonically, decision then
# settlement, so splitting the file anywhere splits it chronologically.
python3 - "$LEDGER" <<'PY'
import json, sys
base = 1_784_900_000_000
rows = []
for i in range(6):
    ticker = f"KXBTC15M-E2E{i}"
    pid = f"{ticker}@{base + i * 1000}"
    won = i % 3 != 2                      # 4 wins, 2 losses
    rows.append({"event": "kalshi_bot_decision", "ts_ms": base + i * 1000,
                 "mode": "paper", "ticker": ticker, "action": "bid",
                 "reason_code": "EDGE_CLEARS_THRESHOLD", "calibrated_p": 0.75,
                 "market_mid": 0.50, "side": "YES", "price": 0.50,
                 "contracts": 4, "stake_usd": 2.0, "fee_usd": 0.07,
                 "position_id": pid})
    rows.append({"event": "kalshi_bot_paper_settlement", "ts_ms": base + i * 1000 + 500,
                 "mode": "paper", "position_id": pid, "ticker": ticker,
                 "side": "YES", "contracts": 4, "stake_usd": 2.0, "fee_usd": 0.07,
                 "market_result": "YES" if won else "NO", "won": won,
                 "realized_pnl": 0.10 if won else -0.50})
with open(sys.argv[1], "w") as out:
    for row in rows:
        out.write(json.dumps(row) + "\n")
PY

# The sealed criteria. Numbers are irrelevant to this test — what is being
# proved is that the LEDGER numbers do not move — but they must be a real,
# floor-respecting preregistration, sealed before the record is read.
"$CLI" kalshi bot gate seal '{"min_settled_bids":300,"max_drawdown_usd":5}' >/dev/null

# Prints the five ledger numbers of a verdict, or "REFUSED <verdict>" when the
# gate refused. A refusal that carried ledger numbers is a hard failure here:
# criterion 5 is precisely that it must not. (Written to a file rather than run
# from a heredoc: a heredoc IS the script's stdin, so the same python could not
# also read the verdict from a pipe.)
cat > "$HELPERS/summary.py" <<'PY'
import json, sys
out = json.load(sys.stdin)
if not out.get("evaluated", False):
    if "ledger" in out or "criteria" in out:
        print("REFUSAL_WITH_NUMBERS")
    else:
        print("REFUSED " + out.get("verdict", "?"))
    sys.exit(0)
led = out["ledger"]
print(" ".join(str(led[k]) for k in
      ("settled_bids", "wins", "losses", "net_pnl_usd", "max_drawdown_usd")))
PY
summary() { python3 "$HELPERS/summary.py"; }

gate() {
  set +e
  OUT="$("$CLI" --json kalshi bot gate)"
  RC=$?
  set -e
}

# --- 1. the whole record in one file: the baseline numbers -------------------
gate
[ "$RC" -eq 0 ] || fail "gate exited $RC on a complete record"
BASELINE="$(printf '%s' "$OUT" | summary)"
[ "$BASELINE" = "6 4 2 -0.6 0.8" ] || fail "unexpected baseline ledger: $BASELINE"
ANCHOR="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["ledger"]["first_settled_ts_ms"])' <<<"$OUT")"
echo "ok: complete record scores $BASELINE (anchor $ANCHOR)"

# --- 2. criterion 4: the same numbers across a rotation ----------------------
# The record is split exactly as a rotation splits it: the older half becomes
# .1, the newer half stays in the live file.
head -n 6 "$LEDGER" > "$HELPERS/older-half.jsonl"
cp "$HELPERS/older-half.jsonl" "$LEDGER.1"
tail -n 6 "$LEDGER" > "$LEDGER.tmp" && mv "$LEDGER.tmp" "$LEDGER"
gate
[ "$RC" -eq 0 ] || fail "gate exited $RC on a rotated record"
ROTATED="$(printf '%s' "$OUT" | summary)"
[ "$ROTATED" = "$BASELINE" ] || fail "rotation moved the gate's numbers: '$ROTATED' != '$BASELINE'"
echo "ok: the gate reports the same record across a rotation ($ROTATED)"

# --- 3. criterion 5a: a hole in the sequence is refused ----------------------
mv "$LEDGER.1" "$LEDGER.2"
gate
[ "$RC" -ne 0 ] || fail "gate exited 0 on a record with a missing generation"
HOLED="$(printf '%s' "$OUT" | summary)"
[ "$HOLED" = "REFUSED RECORD_INCOMPLETE" ] || fail "holed record was not refused cleanly: $HOLED"
grep -q 'kalshi-bot-decisions.jsonl.1' <<<"$OUT" || fail "the refusal did not name the missing generation: $OUT"
echo "ok: a hole in the generation sequence refuses with no numbers (exit $RC)"

# --- 4. criterion 5b: a contiguous record that starts after the anchor -------
# The published verdict is now a REFUSAL, so this also proves the anchor
# survived being written over the verdict that first measured it.
rm -f "$LEDGER.2"
gate
[ "$RC" -ne 0 ] || fail "gate exited 0 on a record truncated before its scored settlements"
TRUNCATED="$(printf '%s' "$OUT" | summary)"
[ "$TRUNCATED" = "REFUSED RECORD_INCOMPLETE" ] || fail "truncated record was not refused cleanly: $TRUNCATED"
grep -q "$ANCHOR" <<<"$OUT" || fail "the refusal did not name the anchor it was measured against: $OUT"
echo "ok: a record that begins after the published anchor refuses (exit $RC)"

# --- 5. the whole record again scores exactly as it did before ---------------
# A refusal must be recoverable, not a wedge: restore the older generation and
# the gate goes back to judging the record, with the numbers it started with.
cp "$HELPERS/older-half.jsonl" "$LEDGER.1"
gate
[ "$RC" -eq 0 ] || fail "gate exited $RC after the record was made whole again"
RESTORED="$(printf '%s' "$OUT" | summary)"
[ "$RESTORED" = "$BASELINE" ] || fail "the restored record does not score as it did: '$RESTORED' != '$BASELINE'"
echo "ok: the restored record scores exactly as it did before ($RESTORED)"

echo "PASS: e2e_kalshi_bot_ledger_rotation"
