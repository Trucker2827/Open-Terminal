#!/usr/bin/env python3
"""Data inventory for the Kalshi edge autopsy (issue #169).

The table the report's "data used" column is built from: for every evidence log
the analysis reads, its size, row count, timestamp span and observed cadence,
plus the retention each file actually has as opposed to the retention its age
suggests.

Retention is the point. These logs rotate BY SIZE (~67MB) into a single `.1`
sibling that the next rotation overwrites, so a fast-writing log like the Kalshi
ticker feed retains hours while a slow one retains days. Any question that needs
two of them jointly is limited by whichever retains less — which is why Q1's
paired window is a fraction of Q2's.

Read-only. Prints JSON to stdout; writes nothing.

  python3 scripts/research/inventory.py
"""
import json
import os
import sys

import kalshi_edge_common as common

# (file, key extracting a millisecond timestamp, what the analysis uses it for)
FILES = (
    ("kalshi-cf-benchmarks.jsonl",
     lambda r: r.get("time"),
     "CF BRTI index — the series Kalshi settles against (Q1 events, Q2/Q3 "
     "derived outcomes, volatility)"),
    ("kalshi-tickers.jsonl",
     lambda r: r.get("ts_ms"),
     "Kalshi top-of-book per market (Q1 quote series)"),
    ("kalshi-bot-decisions.jsonl",
     lambda r: r.get("ts_ms"),
     "the bot's own ledger: calibrated_p, market_mid, bids, fills, settlements "
     "(Q2/Q3 forecast history, Q4 post-mortem)"),
    ("kalshi-settlements.jsonl",
     lambda r: None,
     "public settlement feed — recorded outcomes (Q2/Q3 ground truth)"),
    ("kalshi-venue-features.jsonl",
     lambda r: None,
     "pre-joined spot + Kalshi price snapshots (Q1 cross-check)"),
)


def profile(name, ts_key, purpose):
    entry = {"log": name, "purpose": purpose, "rotations": []}
    total_rows = 0
    stamps = []
    for pattern in common.ROTATIONS:
        path = common.evidence_path(pattern.format(name=name))
        if not os.path.exists(path):
            continue
        rows = 0
        local = []
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                rows += 1
                try:
                    record = json.loads(line)
                except ValueError:
                    continue
                value = ts_key(record)
                if value is None:
                    continue
                try:
                    local.append(int(value))
                except (TypeError, ValueError):
                    continue
        total_rows += rows
        stamps.extend(local)
        entry["rotations"].append({
            "file": os.path.basename(path),
            "megabytes": round(os.path.getsize(path) / 1e6, 1),
            "rows": rows,
            "span_utc": ([common.iso(min(local)), common.iso(max(local))]
                         if local else None),
        })
    entry["total_rows"] = total_rows
    if stamps:
        low, high = min(stamps), max(stamps)
        hours = (high - low) / 3_600_000.0
        entry["retained_span_utc"] = [common.iso(low), common.iso(high)]
        entry["retained_hours"] = round(hours, 2)
        entry["mean_cadence_seconds"] = (round((high - low) / 1000.0
                                               / max(1, total_rows - 1), 3))
    else:
        entry["retained_span_utc"] = None
        entry["note"] = ("no millisecond timestamp extracted for this log — row "
                         "count and size only")
    return entry


def main():
    common.emit({
        "as_of_utc": common.as_of(),
        "command": "python3 scripts/research/inventory.py",
        "evidence_dir": common.evidence_dir(),
        "rotation_policy": ("logs rotate by size (~67MB) into a single .1 "
                            "sibling that the next rotation overwrites; "
                            "retention is therefore a function of write rate, "
                            "not of elapsed time"),
        "logs": [profile(*spec) for spec in FILES],
    })
    return 0


if __name__ == "__main__":
    sys.exit(main())
