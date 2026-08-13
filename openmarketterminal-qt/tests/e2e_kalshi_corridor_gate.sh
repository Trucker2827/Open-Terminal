#!/usr/bin/env bash
set -euo pipefail

CLI="${BUILD_DIR:?BUILD_DIR is required}/openterminalcli"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export OPENTERMINAL_EVIDENCE_DIR="$TMP"
export OPENTERMINAL_KALSHI_EVIDENCE_DIR="$TMP"
export HOME="$TMP/home"
mkdir -p "$HOME"

PARAMS='{"max_bundles_per_opportunity":2,"max_cost_per_opportunity_usd":2.0,"max_scan_age_ms":60000}'
"$CLI" --json kalshi bot corridor-gate seal "$PARAMS" >"$TMP/sealed.json"
MICRO_PARAMS='{"max_bundles_per_opportunity":2,"max_all_in_per_leg_usd":2.0,"max_scan_age_ms":60000,"max_executions_per_hour":3}'
"$CLI" --json kalshi bot corridor-micro-live seal "$MICRO_PARAMS" >"$TMP/micro-sealed.json"
python3 - "$TMP/micro-sealed.json" <<'PY'
import json, sys
seal=json.load(open(sys.argv[1], encoding="utf-8"))
assert seal["authority"] == "micro_live_only"
assert seal["production_live_authorized"] is False
assert seal["params"]["max_all_in_per_leg_usd"] == 2.0
PY

# With no evidence file at all, the paper experiment must arm. Requiring the
# history that the experiment exists to collect would be a circular deadlock.
"$CLI" --json kalshi bot corridor-gate >"$TMP/verdict.stdout.json"
python3 - "$TMP/verdict.stdout.json" "$TMP/kalshi-btc-corridor-gate.json" <<'PY'
import json, sys
stdout = json.load(open(sys.argv[1], encoding="utf-8"))
disk = json.load(open(sys.argv[2], encoding="utf-8"))
assert stdout == disk
assert disk["strategy_family"] == "btc_threshold_corridor"
assert disk["authority"] == "paper_only"
assert disk["verdict"] == "PASS"
assert disk["evaluated"] is True
assert disk["paper_bids_authorized"] is True
assert disk["live_orders_authorized"] is False
assert disk["evidence"]["scans"] == 0
assert disk["evidence"]["distinct_events"] == 0
assert disk["evidence"]["opportunity_scans"] == 0
assert disk["params"] == json.loads('''{"max_bundles_per_opportunity":2,"max_cost_per_opportunity_usd":2.0,"max_scan_age_ms":60000}''')
PY

# The existing directional verdict path remains separate and absent.
test ! -e "$TMP/kalshi-bot-gate.json"

# A fresh, certificate-backed opportunity is consumed by the real paper tick,
# once and only once. It lands in a separate paper ledger and cannot call the
# exchange because the row explicitly records live_order_submitted=false.
python3 - "$TMP/kalshi-btc-threshold-corridor.jsonl" <<'PY'
import hashlib, json, sys
from datetime import datetime, timezone
certificate = {"event_ticker":"KXBTCD-E0","family":"btc_threshold_corridor"}
digest = hashlib.sha256(json.dumps(certificate, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
row = {
  "event":"kalshi_btc_threshold_corridor_scan", "family":"btc_threshold_corridor",
  "received_at":datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
  "quantity":"1", "certificate":certificate, "certificate_sha256":digest,
  "evaluation":{"state":"opportunity","pairs_evaluated":1,"opportunities":1,"pairs":[{
    "lower_ticker":"KXBTCD-E0-T64000", "higher_ticker":"KXBTCD-E0-T65000",
    "evaluation":{"state":"opportunity","quantity":"1","acquisition_cost":"0.80",
      "fees":"0.05","execution_buffer":"0.02","net_edge_per_bundle":"0.13"}}]}}
open(sys.argv[1], "w", encoding="utf-8").write(json.dumps(row, separators=(",", ":"))+"\n")
PY

"$CLI" --json kalshi bot once --no-auto-refresh-calibrators >"$TMP/tick1.json"
"$CLI" --json kalshi bot once --no-auto-refresh-calibrators >"$TMP/tick2.json"
python3 - "$TMP/tick1.json" "$TMP/tick2.json" "$TMP/kalshi-btc-corridor-paper.jsonl" <<'PY'
import json, sys
one=json.load(open(sys.argv[1])); two=json.load(open(sys.argv[2]))
rows=[json.loads(line) for line in open(sys.argv[3], encoding="utf-8") if line.strip()]
assert one["corridor_paper_bids"] == 1, one
assert two["corridor_paper_bids"] == 0, two
assert len(rows) == 1, rows
assert rows[0]["event"] == "kalshi_btc_threshold_corridor_paper_bid"
assert rows[0]["mode"] == "paper"
assert rows[0]["live_order_submitted"] is False
assert rows[0]["result"] == "SIMULATED_AT_OBSERVED_BOOK"
PY

# A micro-live seal is not sufficient by itself. Reach the handler through the
# headless Authenticated/destructive boundary by enabling the BASE trading
# capability in this throwaway profile, while deliberately leaving the global
# LIVE arm off and creating no bounded human session. The handler itself must
# then refuse before any credential or order call and leave the execution
# ledger absent. This setup is explicit so the test cannot depend on the host
# user's persisted authentication/trading state.
"$CLI" --headless mcp list >/dev/null
DB="$(find "$HOME" -name openmarketterminal.db -type f -print -quit)"
test -n "$DB"
sqlite3 "$DB" \
  "INSERT OR REPLACE INTO settings(key,value,category,updated_at) VALUES('cli.allow_trading','true','cli','2026-01-01'),('cli.live_trading_armed','false','cli','2026-01-01');"
set +e
"$CLI" --json kalshi bot corridor-micro-live once >"$TMP/micro-refused.json"
rc=$?
set -e
test "$rc" -eq 6
python3 - "$TMP/micro-refused.json" <<'PY'
import json, sys
row=json.load(open(sys.argv[1], encoding="utf-8"))
assert row["status"] == "rejected", row
assert ("not armed" in row["reason"] or
        "current bounded human session" in row["reason"]), row
PY
test ! -e "$TMP/kalshi-btc-corridor-micro-live.jsonl"
