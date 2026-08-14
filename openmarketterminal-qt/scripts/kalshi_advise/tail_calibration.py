#!/usr/bin/env python3
"""ANALYSIS ONLY. Is the market's price a fair probability, bucket by bucket?

Nothing here is read by the bot, the gate, or any admission path. It writes a
report and changes no behaviour.

THE QUESTION
------------
Not "can a model beat the mid" -- that was measured across six model variants
and every family, and the answer was no. This asks something the ablation could
not: is the mid ITSELF biased, and if so WHERE on the price range?

WHY THE TAILS SPECIFICALLY
--------------------------
Kalshi's BTC and commodity series quote on `linear_cent`: a $0.01 tick, floor
$0.01, cap $0.99. A contract whose fair value is $0.002 CANNOT be quoted at its
fair value -- the grid's lowest expressible price is five times too high, and no
participant can undercut it. Symmetrically, nothing worth $0.998 can be quoted
above $0.99.

So the grid mechanically compresses both tails toward the middle, and the
compression is not a market opinion that competition can remove. If that shows
up in settled outcomes it appears as: contracts priced near zero win LESS often
than their price, and contracts priced near one win MORE often. Both tails
biased in the same direction, from arithmetic rather than sentiment.

A second, independent mechanism pushes the same way. Settlement is the mean of
the underlying index over the final sixty seconds, not a point print. The
variance of that mean is below the variance of a point, so a market pricing the
contract as if it settled on a point is using too much volatility -- which for a
digital pushes probability toward 0.5 and overprices both tails.

This file does not distinguish the two causes. It measures whether the effect is
there at all, and with what denominator.

WHAT WOULD MAKE THIS WRONG
--------------------------
1. Resampling contracts rather than settlement EVENTS. Strikes on one ladder are
   one price path; treating 500 correlated contracts as 500 draws makes every
   interval far too narrow. The bootstrap here resamples events.
2. Reporting an interval for a bucket whose outcomes are all identical. The
   bootstrap cannot draw a loss from an all-win bucket, so it returns a tight
   interval that means nothing. Those buckets get `None` and a stated reason,
   and are tested against the market's own null instead.
3. Pooling families. Gold hourly and BTC 15-minute are different statistical
   problems; a pooled row is printed as a DIAGNOSTIC and is never the finding.
"""
from __future__ import annotations

import json
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ablation_matrix as ab
from openterminal_paths import evidence_file

BOOTSTRAP_SEED = 20260813
BOOTSTRAP_SAMPLES = 4000
NULL_SAMPLES = 20000

# Fine at the tails, coarse in the belly: the thesis is about the tails, and a
# single 0.00-0.20 bucket would average a $0.01 contract together with a $0.19
# one whose grid distortion is twenty times smaller.
BUCKETS = (
    (0.000, 0.015), (0.015, 0.025), (0.025, 0.050), (0.050, 0.100),
    (0.100, 0.200), (0.200, 0.350), (0.350, 0.650), (0.650, 0.800),
    (0.800, 0.900), (0.900, 0.950), (0.950, 0.975), (0.975, 0.985),
    (0.985, 1.000),
)

STATE_FILES = (
    ("spot-calibrator-state.json", "BTC (daemon-tracked)"),
    ("commodities-hourly-calibrator-state.json", "commodities hourly"),
    ("commodities-daily-calibrator-state.json", "commodities daily"),
    ("commodities-15m-calibrator-state.json", "commodities 15m"),
    ("kxbtc-daily-calibrator-state.json", "BTC daily"),
    ("kxbtc15m-calibrator-state.json", "BTC 15m"),
)


def bucket_of(price):
    for lo, hi in BUCKETS:
        if lo <= price < hi or (hi >= 1.0 and price >= lo):
            return "%.3f-%.3f" % (lo, hi)
    return None


def rows_from_records(records):
    """One row per settled contract: its final quoted price and its outcome.

    `event` is the clustering key. Records written after the ladder-identity
    change carry a real `event_ticker`; older ones fall back to the
    (spot, sqrt_minutes_left) proxy the ablation already uses. The proxy is
    conservative -- merging two genuinely distinct events only widens intervals.
    """
    out = []
    for record in records:
        observations = record.get("observations") or []
        if not observations:
            continue
        obs = observations[-1]
        mid = obs.get("yes_mid")
        if not isinstance(mid, (int, float)) or not (0.0 < mid < 1.0):
            continue
        recorded = record.get("event_ticker")
        # `books` is index-aligned with `observations`; take the quote that
        # belongs to the SAME observation the price came from, never the last
        # book with the first price. Records written before quote capture have
        # no entry, and their spread stays None rather than being guessed.
        books = record.get("books") or []
        book = books[len(observations) - 1] if len(books) >= len(observations) else {}
        bid = book.get("market_yes_bid") if isinstance(book, dict) else None
        ask = book.get("market_yes_ask") if isinstance(book, dict) else None
        spread = None
        if isinstance(bid, (int, float)) and isinstance(ask, (int, float)) and ask > bid:
            spread = float(ask) - float(bid)
        out.append({
            "p": float(mid),
            "outcome": 1.0 if record.get("outcome") else 0.0,
            "event": recorded if recorded else ab.event_key(obs),
            "event_from_record": bool(recorded),
            "bid": float(bid) if isinstance(bid, (int, float)) else None,
            "ask": float(ask) if isinstance(ask, (int, float)) else None,
            "spread": spread,
        })
    return out


def clustered_gap_interval(rows, seed=BOOTSTRAP_SEED, samples=BOOTSTRAP_SAMPLES):
    """CI on (realized frequency - mean price), in probability POINTS.

    Resamples EVENTS. Returns None with a reason when the bucket cannot support
    an interval, rather than a number that looks decisive and is not.
    """
    if len(rows) < 2:
        return {"lo": None, "hi": None, "n_events": len(rows),
                "note": "too few contracts to bound"}
    clusters = {}
    for row in rows:
        clusters.setdefault(row["event"], []).append(row)
    groups = list(clusters.values())
    if len(groups) < 2:
        return {"lo": None, "hi": None, "n_events": len(groups),
                "note": "too few independent events to bound"}
    outcomes = {row["outcome"] for row in rows}
    if len(outcomes) < 2:
        return {"lo": None, "hi": None, "n_events": len(groups),
                "note": "every outcome identical - interval would be degenerate"}
    rng = random.Random(seed)
    gaps = []
    for _ in range(samples):
        drawn = []
        for _ in range(len(groups)):
            drawn.extend(groups[rng.randrange(len(groups))])
        freq = sum(r["outcome"] for r in drawn) / len(drawn)
        price = sum(r["p"] for r in drawn) / len(drawn)
        gaps.append((freq - price) * 100.0)
    gaps.sort()
    return {"lo": gaps[int(0.025 * samples)], "hi": gaps[int(0.975 * samples)],
            "n_events": len(groups)}


def null_probability(rows, seed=BOOTSTRAP_SEED, samples=NULL_SAMPLES):
    """Under 'the market price IS the true probability', how surprising are the
    observed wins? Two-sided, and it works for an all-win or all-loss bucket
    where the bootstrap cannot.

    This is the test that matters at the tails: it needs no resampling of
    outcomes, only the prices, so a bucket where every contract won still gets a
    real answer instead of a degenerate interval.
    """
    if not rows:
        return None
    observed = sum(r["outcome"] for r in rows)
    expected = sum(r["p"] for r in rows)
    rng = random.Random(seed)
    at_least_as_extreme = 0
    for _ in range(samples):
        wins = 0
        for row in rows:
            if rng.random() < row["p"]:
                wins += 1
        if abs(wins - expected) >= abs(observed - expected) - 1e-9:
            at_least_as_extreme += 1
    return {
        "observed_wins": int(observed),
        "expected_wins": expected,
        "p_two_sided": at_least_as_extreme / samples,
    }


def measure(rows):
    by_bucket = {}
    for row in rows:
        key = bucket_of(row["p"])
        if key:
            by_bucket.setdefault(key, []).append(row)
    out = {}
    for key in sorted(by_bucket):
        group = by_bucket[key]
        mean_price = sum(r["p"] for r in group) / len(group)
        freq = sum(r["outcome"] for r in group) / len(group)
        out[key] = {
            "contracts": len(group),
            "events": len({r["event"] for r in group}),
            "mean_price": mean_price,
            "won": int(sum(r["outcome"] for r in group)),
            "realized_frequency": freq,
            "gap_points": (freq - mean_price) * 100.0,
            "clustered_ci": clustered_gap_interval(group),
            "under_correct_market_null": null_probability(group),
            "tradeability": tradeability(group, mean_price, freq),
        }
    return out


def tradeability(rows, mean_price, freq):
    """Does the gap survive the spread? The question that decides everything.

    A negative gap says the contract is overpriced. You harvest that by SELLING,
    and you sell at the bid, not the mid. So the honest edge is measured against
    the bid, not against the quoted midpoint:

        gap_vs_bid = realized_frequency - mean_bid

    On a thin book the bid can sit far below the mid, and a mispricing that
    looks like eleven points against the mid can be zero or negative against the
    price you could actually get. Until quotes were recorded beside the price
    this was unanswerable; where they are still missing this says so rather than
    substituting the mid and calling it an edge.
    """
    quoted = [r for r in rows if r.get("spread") is not None]
    if not quoted:
        return {"measurable": False,
                "reason": "no bid/ask recorded for these contracts - "
                          "gap is against the MID and may not be reachable",
                "contracts_with_quotes": 0}
    spreads = sorted(r["spread"] for r in quoted)
    mean_bid = sum(r["bid"] for r in quoted) / len(quoted)
    mean_ask = sum(r["ask"] for r in quoted) / len(quoted)
    # Both directions, because a gap can point either way and each is harvested
    # at the OPPOSITE side of the book.
    #   overpriced -> SELL, you receive the bid, edge = bid - realized
    #   underpriced -> BUY, you pay the ask,     edge = realized - ask
    # Neither uses the mid, because nobody trades at the mid.
    sell_edge = (mean_bid - freq) * 100.0
    buy_edge = (freq - mean_ask) * 100.0
    best = max(sell_edge, buy_edge)
    return {
        "measurable": True,
        "contracts_with_quotes": len(quoted),
        "coverage": len(quoted) / len(rows),
        "median_spread_points": spreads[len(spreads) // 2] * 100.0,
        "mean_bid": mean_bid,
        "mean_ask": mean_ask,
        "gap_vs_mid_points": (freq - mean_price) * 100.0,
        "sell_edge_points": sell_edge,
        "buy_edge_points": buy_edge,
        "survives_the_spread": best > 0.0,
        "direction": ("sell" if sell_edge >= buy_edge else "buy") if best > 0.0 else "neither",
    }


def load_families():
    families = {}
    for filename, label in STATE_FILES:
        path = evidence_file(filename)
        try:
            state = json.load(open(path, "r", encoding="utf-8"))
        except (OSError, ValueError):
            continue
        top = state.get("resolved_record") or []
        if top:
            families["%s :: %s" % (label, "all")] = top
        for family, slice_state in sorted((state.get("by_family") or {}).items()):
            records = slice_state.get("resolved_record") or []
            if records:
                families["%s :: %s" % (label, family)] = records
    return families


def build_report():
    import time
    families = load_families()
    per_family = {}
    pooled_rows = []
    identity = {"from_recorded_event_ticker": 0, "from_proxy": 0}
    for name, records in sorted(families.items()):
        rows = rows_from_records(records)
        if not rows:
            per_family[name] = {"skipped": "no scorable settled contracts"}
            continue
        for row in rows:
            identity["from_recorded_event_ticker" if row["event_from_record"]
                     else "from_proxy"] += 1
        per_family[name] = {
            "contracts": len(rows),
            "events": len({r["event"] for r in rows}),
            "buckets": measure(rows),
        }
        pooled_rows.extend(rows)
    return {
        "event": "tail_calibration",
        "advisory_only": True,
        "analysis_only": True,
        "generated_at_ms": int(time.time() * 1000),
        "question": ("is the market price itself biased, and where on the price range -- "
                     "NOT whether a model beats it"),
        "grid": ("Kalshi linear_cent: tick $0.01, floor $0.01, cap $0.99. Fair values "
                 "below $0.01 or above $0.99 are INEXPRESSIBLE, so both tails are "
                 "mechanically compressed toward the middle."),
        "sign_convention": ("gap = realized_frequency - mean_price, in probability POINTS. "
                            "NEGATIVE at a low-price bucket means the contract won LESS "
                            "often than its price: overpriced. POSITIVE at a high-price "
                            "bucket means it won MORE often than its price: underpriced. "
                            "The grid thesis predicts negative at the bottom AND positive "
                            "at the top."),
        "clustering": ("bootstrap resamples settlement EVENTS, never contracts; strikes on "
                       "one ladder are a single price path"),
        "event_identity": identity,
        "families": per_family,
        "pooled_diagnostic": {
            "warning": ("POOLED ACROSS FAMILIES - diagnostic only, never a finding. "
                        "Gold hourly and BTC 15-minute are different statistical problems. "
                        "Reported because the grid mechanism is a property of the EXCHANGE, "
                        "not of any underlier, so a shared sign across families is the "
                        "thing worth seeing."),
            "contracts": len(pooled_rows),
            "events": len({r["event"] for r in pooled_rows}),
            "buckets": measure(pooled_rows),
        } if pooled_rows else None,
    }


def main(argv=None):
    print(json.dumps(build_report(), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
