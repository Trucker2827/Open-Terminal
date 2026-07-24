#!/usr/bin/env python3
"""Versioned, deterministic qualification report for crypto-auto-scalp-v1.

This is deliberately a journal reader, never an execution surface. It resolves
each immutable shadow opportunity against the first consolidated spot print at
or after the fixed horizon and applies the selected proposal's recorded
round-trip cost. No current fee table or mutable strategy setting is consulted.
"""

import argparse
import hashlib
import json
import math
import pathlib
import random
import statistics
from bisect import bisect_left

REPORT_VERSION = "crypto-scalp-qualification-v1"
ENGINE_VERSION = "crypto-auto-scalp-v1"
MIN_RESOLVED = 200
MIN_COVERAGE = 0.80
DEFAULT_HORIZON_MS = 15_000


def _rows(path):
    path = pathlib.Path(path)
    result = []
    for candidate in (path.with_name(path.name + ".1"), path):
        if not candidate.exists():
            continue
        for line in candidate.read_text(errors="replace").splitlines():
            try:
                value = json.loads(line)
            except (ValueError, TypeError):
                continue
            if isinstance(value, dict):
                result.append(value)
    return result


def _canonical_hash(value):
    raw = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(raw).hexdigest()


def _mean_ci(values, samples=2000):
    if not values:
        return 0.0, 0.0, 0.0
    mean = statistics.fmean(values)
    if len(values) == 1:
        return mean, mean, mean
    rng = random.Random(0x5CA1F)
    boot = []
    for _ in range(samples):
        boot.append(statistics.fmean(rng.choice(values) for _ in values))
    boot.sort()
    return mean, boot[int(0.025 * (len(boot) - 1))], boot[int(0.975 * (len(boot) - 1))]


def compute_report(decisions, ticks, horizon_ms=DEFAULT_HORIZON_MS):
    opportunities = {}
    invalid = []
    for row in decisions:
        if row.get("engine_version") != ENGINE_VERSION:
            continue
        signal = row.get("reversal_signal") or {}
        if signal.get("call") not in ("BUY", "SELL"):
            continue
        opportunity_id = row.get("opportunity_id")
        if not opportunity_id:
            invalid.append("MISSING_OPPORTUNITY_ID")
            continue
        immutable = {
            "symbol": row.get("symbol"),
            "signal": signal,
            "venue_proposals": row.get("venue_proposals") or [],
            "selected_proposal_index": row.get("selected_proposal_index"),
        }
        digest = _canonical_hash(immutable)
        previous = opportunities.get(opportunity_id)
        if previous and previous["_hash"] != digest:
            invalid.append("OPPORTUNITY_MUTATION:" + opportunity_id)
            continue
        immutable["_hash"] = digest
        opportunities[opportunity_id] = immutable

    series = {}
    for tick in ticks:
        symbol = tick.get("symbol")
        try:
            ts = int(tick.get("received_ts_ms") or tick.get("exchange_ts_ms") or 0)
            px = float(tick.get("price") or 0)
        except (TypeError, ValueError):
            continue
        if symbol and ts > 0 and px > 0:
            series.setdefault(symbol, []).append((ts, px))
    for rows in series.values():
        rows.sort()

    resolved = []
    for opportunity_id, opportunity in opportunities.items():
        signal = opportunity["signal"]
        try:
            start_ts = int(signal.get("signal_bar_ms") or 0) + 1000
            entry = float(signal.get("reference_price") or 0)
            selected = int(opportunity.get("selected_proposal_index"))
            proposal = opportunity["venue_proposals"][selected]
        except (TypeError, ValueError, IndexError):
            continue
        if not proposal.get("executable") or entry <= 0 or start_ts <= 0:
            continue
        rows = series.get(opportunity["symbol"]) or []
        idx = bisect_left(rows, (start_ts + horizon_ms, -math.inf))
        if idx >= len(rows):
            continue
        exit_ts, exit_price = rows[idx]
        direction = 1.0 if signal["call"] == "BUY" else -1.0
        gross_bps = direction * (exit_price / entry - 1.0) * 10_000.0
        cost_bps = float(proposal.get("round_trip_cost_bps") or 0)
        resolved.append({
            "opportunity_id": opportunity_id,
            "symbol": opportunity["symbol"],
            "call": signal["call"],
            "entry_price": entry,
            "exit_price": exit_price,
            "exit_ts_ms": exit_ts,
            "venue": proposal.get("venue"),
            "gross_bps": gross_bps,
            "cost_bps": cost_bps,
            "net_bps": gross_bps - cost_bps,
            "won": gross_bps > 0,
        })

    nets = [row["net_bps"] for row in resolved]
    mean_net, ci_low, ci_high = _mean_ci(nets)
    candidate_count = len(opportunities)
    coverage = len(resolved) / candidate_count if candidate_count else 0.0
    checks = {
        "journal_integrity": not invalid,
        "minimum_resolved": len(resolved) >= MIN_RESOLVED,
        "coverage": coverage >= MIN_COVERAGE,
        "positive_mean_net_bps": mean_net > 0,
        "ci_low_above_zero": ci_low > 0,
    }
    state = "INVALID_EPOCH" if invalid else (
        "QUALIFIED" if all(checks.values()) else "SHADOW"
    )
    return {
        "report_version": REPORT_VERSION,
        "engine_version": ENGINE_VERSION,
        "state": state,
        "execution_eligible": state == "QUALIFIED",
        "candidate_count": candidate_count,
        "resolved_count": len(resolved),
        "required_resolved": MIN_RESOLVED,
        "coverage": coverage,
        "required_coverage": MIN_COVERAGE,
        "mean_net_bps": mean_net,
        "mean_net_ci95": [ci_low, ci_high],
        "win_rate": (sum(row["won"] for row in resolved) / len(resolved)) if resolved else 0.0,
        "checks": checks,
        "integrity_errors": invalid,
        "resolved": resolved,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--decisions", required=True)
    parser.add_argument("--ticks", required=True)
    parser.add_argument("--horizon-ms", type=int, default=DEFAULT_HORIZON_MS)
    parser.add_argument("--output")
    args = parser.parse_args()
    report = compute_report(_rows(args.decisions), _rows(args.ticks), args.horizon_ms)
    encoded = json.dumps(report, sort_keys=True, separators=(",", ":"))
    if args.output:
        target = pathlib.Path(args.output)
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_suffix(target.suffix + ".tmp")
        temporary.write_text(encoded + "\n")
        temporary.replace(target)
    print(encoded)


if __name__ == "__main__":
    main()
