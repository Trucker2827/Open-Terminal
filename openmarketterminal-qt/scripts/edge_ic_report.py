#!/usr/bin/env python3
"""Information-coefficient report for the edge journal's resolved decisions.

Read-only: answers "does the engine's predicted edge actually correlate with
what happened?" the same way the Quant Lab scores a factor — daily-agnostic
rank/linear IC between predicted edge and realized move, plus win rate by
predicted-edge quintile. A signal whose IC is ~0 is costing spread for
nothing, no matter how busy its journal looks.

Usage: edge_ic_report.py [--db PATH] [--venue coinbase] [--days 30]
"""
import argparse
import json
import math
import os
import re
import sqlite3
import time

DEFAULT_DB = os.path.expanduser(
    "~/Library/Application Support/org.openterminal.OpenTerminal/data/openmarketterminal.db")
MOVE_RE = re.compile(r"scored: future=[-0-9.eE]+ move=([-0-9.eE]+)")


def parse_realized_move(reasons):
    """The resolver appends 'scored: future=X move=Y breakeven=Z' to reasons."""
    if not reasons:
        return None
    match = MOVE_RE.search(reasons)
    if not match:
        return None
    try:
        return float(match.group(1))
    except ValueError:
        return None


def pearson(xs, ys):
    n = len(xs)
    if n < 3:
        return None
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx <= 0.0 or syy <= 0.0:
        return None
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    return sxy / math.sqrt(sxx * syy)


def ranks(values):
    """Average ranks (ties averaged), 1-based."""
    order = sorted(range(len(values)), key=lambda i: values[i])
    out = [0.0] * len(values)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and values[order[j + 1]] == values[order[i]]:
            j += 1
        avg = (i + j) / 2.0 + 1.0
        for k in range(i, j + 1):
            out[order[k]] = avg
        i = j + 1
    return out


def spearman(xs, ys):
    if len(xs) < 3:
        return None
    return pearson(ranks(xs), ranks(ys))


def quintile_win_rates(predicted, outcomes):
    """Win rate per predicted-edge quintile (lowest -> highest)."""
    n = len(predicted)
    if n < 5:
        return []
    order = sorted(range(n), key=lambda i: predicted[i])
    buckets = []
    for q in range(5):
        lo, hi = q * n // 5, (q + 1) * n // 5
        idx = order[lo:hi]
        if not idx:
            continue
        buckets.append({
            "quintile": q + 1,
            "count": len(idx),
            "mean_predicted_edge": sum(predicted[i] for i in idx) / len(idx),
            "win_rate": sum(1 for i in idx if outcomes[i]) / len(idx),
        })
    return buckets


def build_report(rows, now_ms):
    """rows: (predicted_edge, confidence, realized_move, outcome_win) tuples."""
    predicted = [r[0] for r in rows]
    conf_scaled = [r[0] * r[1] for r in rows]
    realized = [r[2] for r in rows]
    outcomes = [r[3] for r in rows]
    return {
        "schema": 1, "event": "edge_ic_report", "generated_at_ms": now_ms,
        "resolved_decisions": len(rows),
        "ic_pearson_edge_vs_move": pearson(predicted, realized),
        "ic_spearman_edge_vs_move": spearman(predicted, realized),
        "ic_pearson_confweighted": pearson(conf_scaled, realized),
        "overall_win_rate": (sum(outcomes) / len(outcomes)) if outcomes else None,
        "win_rate_by_edge_quintile": quintile_win_rates(predicted, outcomes),
        "reading": ("IC near 0 means predicted edge carries no information about the "
                    "realized move; quintiles should climb if the signal is real."),
    }


def load_rows(db_path, venue=None, days=30):
    since_ms = int(time.time() * 1000) - days * 24 * 3600 * 1000
    query = ("SELECT raw_edge, confidence, reasons, outcome FROM edge_decision_journal"
             " WHERE outcome IN (0, 1) AND resolved_at IS NOT NULL"
             " AND venue != 'kalshi' AND created_at >= ?")
    params = [since_ms]
    if venue:
        query += " AND venue = ?"
        params.append(venue)
    conn = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    try:
        rows = []
        for raw_edge, confidence, reasons, outcome in conn.execute(query, params):
            move = parse_realized_move(reasons)
            if move is None or raw_edge is None:
                continue
            rows.append((float(raw_edge), float(confidence or 0.0), move, int(outcome) == 1))
        return rows
    finally:
        conn.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=DEFAULT_DB)
    ap.add_argument("--venue")
    ap.add_argument("--days", type=int, default=30)
    args = ap.parse_args()
    try:
        rows = load_rows(args.db, args.venue, args.days)
    except sqlite3.Error as exc:
        print(json.dumps({"error": str(exc), "db": args.db}))
        return 1
    print(json.dumps(build_report(rows, int(time.time() * 1000)), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
