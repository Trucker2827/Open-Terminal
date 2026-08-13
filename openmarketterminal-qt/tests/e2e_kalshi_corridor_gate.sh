#!/usr/bin/env bash
set -euo pipefail

CLI="${BUILD_DIR:?BUILD_DIR is required}/openterminalcli"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export OPENTERMINAL_EVIDENCE_DIR="$TMP"
export OPENTERMINAL_KALSHI_EVIDENCE_DIR="$TMP"

PARAMS='{"min_scans":300,"min_distinct_events":3,"min_opportunity_scans":10,"min_opportunity_events":3,"min_best_net_edge_usd":0.01,"max_unavailable_rate":0.10}'
"$CLI" --json kalshi bot corridor-gate seal "$PARAMS" >"$TMP/sealed.json"

python3 - "$TMP/kalshi-btc-threshold-corridor.jsonl" <<'PY'
import json, sys
from datetime import datetime, timezone
path = sys.argv[1]
received_at = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
with open(path, "w", encoding="utf-8") as out:
    for i in range(300):
        opportunity = i < 10
        result = {"state": "opportunity" if opportunity else "not_profitable",
                  "net_edge_per_bundle": "0.025" if opportunity else "0"}
        row = {
            "event": "kalshi_btc_threshold_corridor_scan",
            "family": "btc_threshold_corridor",
            "received_at": received_at,
            "certificate": {"event_ticker": f"KXBTCD-E{i % 3}"},
            "certificate_sha256": "reviewed",
            "evaluation": {"state": "opportunity" if opportunity else "not_profitable",
                           "pairs_evaluated": 3,
                           "opportunities": 1 if opportunity else 0,
                           "pairs": [{"evaluation": result}]},
        }
        out.write(json.dumps(row, separators=(",", ":")) + "\n")
PY

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
assert disk["evidence"]["scans"] == 300
assert disk["evidence"]["distinct_events"] == 3
assert disk["evidence"]["opportunity_scans"] == 10
assert disk["evidence"]["opportunity_events"] == 3
PY

# The existing directional verdict path remains separate and absent.
test ! -e "$TMP/kalshi-bot-gate.json"
