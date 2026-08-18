#!/usr/bin/env python3
"""Read-only diagnostics and postmortems for hourly paper research ledgers."""
from __future__ import annotations

import argparse
import json
import os

import kalshi_edge_common as common

LEDGERS = {
    "gold_core": "kxgoldh-forward-paper.json",
    "silver_core": "kxsilverh-forward-paper.json",
    "wti_core": "kxwtih-forward-paper.json",
    "btc_chronos": "btc1h-chronos-shadow.json",
    "gold_chronos": "chronos-kxgoldh-shadow.json",
    "silver_chronos": "chronos-kxsilverh-shadow.json",
    "wti_chronos": "chronos-kxwtih-shadow.json",
}


def load(name):
    path = common.evidence_path(LEDGERS[name])
    if not os.path.exists(path):
        return path, {}
    with open(path, encoding="utf-8") as handle:
        return path, json.load(handle)


def pnl_for(record, chronos=False):
    key = "chronos_pnl" if chronos else "control_pnl"
    if key in record:
        return float(record[key])
    return float(record.get("net_pnl") or 0)


def ledger_summary(name, path, state):
    records = list((state.get("records") or {}).values())
    completed = [r for r in records if r.get("status") == "completed"]
    chronos = name.endswith("_chronos") or name == "btc_chronos"
    rows = []
    for record in sorted(completed, key=lambda r: int(r.get("settled_at_ms") or 0)):
        postmortem = dict(record.get("postmortem") or {})
        postmortem.update({
            "ticker": record.get("ticker") or record.get("hour_bucket"),
            "relationship": record.get("relationship"),
            "side": record.get("side") or record.get("control_side"),
            "entry_price": postmortem.get("entry_price", record.get("entry_price", record.get("control_price"))),
            "model_probability": record.get("model_probability"),
            "executable_edge": record.get("executable_edge", record.get("control_edge")),
            "contracts": record.get("contracts", record.get("control_contracts")),
            "entry_fee": record.get("entry_fee"),
            "outcome": postmortem.get("outcome", "YES" if record.get("outcome_yes") else "NO"),
            "pnl": pnl_for(record, chronos),
            "won": record.get("chronos_won") if chronos else record.get("won", record.get("control_won")),
            "settled_at_ms": record.get("settled_at_ms"),
        })
        rows.append(postmortem)
    count = len(completed)
    return {
        "ledger": path,
        "policy_sha256": state.get("policy_sha256"),
        "completed": count,
        "open": sum(r.get("status") == "open" for r in records),
        "net_pnl": sum(pnl_for(r, chronos) for r in completed),
        "wins": sum(bool(r.get("chronos_won") if chronos else r.get("won", r.get("control_won"))) for r in completed),
        "diagnostics": state.get("diagnostics") or {},
        "evidence_gate": "DIAGNOSTIC" if count < 30 else ("MINIMUM" if count < 50 else ("VALIDATION" if count < 100 else "COMPLETE")),
        "postmortems": rows,
    }


def audit():
    ledgers = {}
    for name in LEDGERS:
        path, state = load(name)
        ledgers[name] = ledger_summary(name, path, state)
    return {
        "event": "hourly_trial_diagnostics",
        "authority": "read_only_paper_research_no_order_api",
        "thresholds": {"diagnostic": 30, "minimum_comparison": 50, "validation": 100},
        "wti_settlement_feed": "BLOCKED_UNTIL_AUTHORITATIVE_ALIGNED_SOURCE_AVAILABLE",
        "ledgers": ledgers,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    print(json.dumps(audit(), sort_keys=True, indent=None if args.json else 2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
