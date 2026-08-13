#!/usr/bin/env python3
"""ANALYSIS ONLY. Are strikes OUTSIDE the default display window mispriced?

The hypothesis, from an operator who trades these by hand: Kalshi shows three
strikes by default, those three absorb most of the order flow, and the strikes
you only reach via "view more contracts" get less attention -- so if mispricing
survives anywhere on the ladder, it survives off-screen.

That is a claim about ATTENTION, and it is separable from the claim that deep
strikes are underpriced. Deep-strike pricing was already tested (218 settled
hourly BTC contracts, favourite bands landed at 23-40% under a correct-market
null -- not distinguishable from fair). This asks a different question: at the
SAME moneyness, does a strike priced with less attention behave differently?

WHAT "DISPLAYED" MEANS HERE
---------------------------
Kalshi's exact UI rule is not in any data we hold, so it is NOT assumed. What is
measurable is RANK BY DISTANCE FROM SPOT within a settlement event: rank 1 is
the strike nearest spot, rank 2 the next, and so on. The attention hypothesis
predicts pricing quality DEGRADES with rank, whichever cutoff the UI actually
uses. `DISPLAY_RANKS` is a proxy for the window and is reported as an
assumption, not a fact -- and the rank-by-rank table is printed so a reader can
see the shape without trusting the cutoff.

THE CONFOUND, STATED UP FRONT
-----------------------------
Rank and price are nearly collinear: high-rank strikes are far from spot and
therefore extreme in price, and calibration behaves differently at extremes for
reasons that have nothing to do with attention. Comparing raw rank buckets
would measure moneyness and call it attention. So the comparison is made
WITHIN price bands, and where a band lacks both displayed and hidden contracts
it is reported as such rather than pooled.

EVENTS, NOT CONTRACTS
---------------------
Strikes in one hourly event are one price path. They are clustered for every
interval, and the event count is published beside every number -- 218 contracts
across 175 events is a very different evidence base from 218 independent draws.
"""
from __future__ import annotations

import collections
import json
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")))

BOOTSTRAP_SEED = 20260813
BOOTSTRAP_SAMPLES = 4000

# Proxy for Kalshi's default window: the N strikes nearest spot. An ASSUMPTION.
DISPLAY_RANKS = 3

# Only contracts with real time left -- 15-minute races are a different
# instrument and the operator's own record shows them losing heavily.
MIN_SQRT_MINUTES = 4.0

PRICE_BANDS = ((0.00, 0.20), (0.20, 0.60), (0.60, 0.80), (0.80, 0.95), (0.95, 1.00))


def load_rows(path):
    """Settled contracts with a market price, a moneyness and an event key."""
    try:
        state = json.load(open(path, "r", encoding="utf-8"))
    except (OSError, ValueError):
        return []
    rows = []
    for record in state.get("resolved_record") or []:
        observations = record.get("observations") or []
        if not observations:
            continue
        obs = observations[-1]
        mid = obs.get("yes_mid")
        dist = obs.get("signed_distance_bps")
        left = obs.get("sqrt_minutes_left")
        vol = obs.get("per_min_vol_bps")
        if not isinstance(mid, (int, float)) or not (0.0 < mid < 1.0):
            continue
        if not isinstance(dist, (int, float)) or not isinstance(left, (int, float)):
            continue
        if left <= MIN_SQRT_MINUTES:
            continue
        # Prefer the RECORDED event id. The proxy below splits one real ladder
        # into many pseudo-events, because each strike's last observation lands
        # at a slightly different moment -- which is how a 215-contract sample
        # once reported 180 separate "events" and had nothing left to compare.
        event = record.get("event_ticker")
        exact = bool(event)
        if not exact:
            event = (round(float(left), 4),
                     round(float(vol), 4) if isinstance(vol, (int, float)) else None)
        rows.append({
            "mid": float(mid),
            "outcome": 1.0 if record.get("outcome") else 0.0,
            "distance_bps": float(dist),
            "event": event,
            "event_is_exact": exact,
        })
    return rows


def rank_within_events(rows):
    """Rank 1 = nearest spot. Ranks are per event, which is the only place the
    notion means anything."""
    events = collections.defaultdict(list)
    for row in rows:
        events[row["event"]].append(row)
    for group in events.values():
        for rank, row in enumerate(sorted(group, key=lambda r: abs(r["distance_bps"])), start=1):
            row["rank"] = rank
            row["displayed"] = rank <= DISPLAY_RANKS
            row["event_size"] = len(group)
    return rows


def calibration_gap(rows):
    """Realised frequency minus mean price, in probability points.

    Positive means the market UNDERPRICED this group (it won more often than it
    charged for).
    """
    if not rows:
        return None
    price = sum(r["mid"] for r in rows) / len(rows)
    freq = sum(r["outcome"] for r in rows) / len(rows)
    return (freq - price) * 100.0


def null_probability(rows, seed=BOOTSTRAP_SEED, samples=200_000):
    """Under "the price IS the probability", how often would this many wins or
    more occur? Not a bootstrap: the null is fully specified by the prices, so
    it is simulated directly rather than resampled."""
    if not rows:
        return None
    rng = random.Random(seed)
    observed = sum(r["outcome"] for r in rows)
    hits = 0
    for _ in range(samples):
        wins = sum(1 for r in rows if rng.random() < r["mid"])
        if wins >= observed:
            hits += 1
    return hits / samples


def clustered_gap_ci(rows, seed=BOOTSTRAP_SEED, samples=BOOTSTRAP_SAMPLES):
    """CI on the calibration gap, resampling EVENTS.

    Returns lo/hi None when there are too few events to bound, or when every
    outcome is identical -- an all-win group produces a degenerate interval that
    looks decisive and means nothing, which is exactly the trap this note
    exists to prevent.
    """
    groups = collections.defaultdict(list)
    for row in rows:
        groups[row["event"]].append(row)
    clusters = list(groups.values())
    if len(clusters) < 2:
        return {"lo": None, "hi": None, "n_events": len(clusters),
                "note": "too few independent events to bound"}
    outcomes = {r["outcome"] for r in rows}
    if len(outcomes) < 2:
        return {"lo": None, "hi": None, "n_events": len(clusters),
                "note": "every outcome identical — interval would be degenerate"}
    rng = random.Random(seed)
    gaps = []
    for _ in range(samples):
        drawn = []
        for _ in range(len(clusters)):
            drawn.extend(clusters[rng.randrange(len(clusters))])
        gaps.append(calibration_gap(drawn))
    gaps.sort()
    return {"lo": gaps[int(0.025 * samples)], "hi": gaps[int(0.975 * samples)],
            "n_events": len(clusters)}


def summarise(rows):
    if not rows:
        return None
    return {
        "contracts": len(rows),
        "events": len({r["event"] for r in rows}),
        "mean_price": sum(r["mid"] for r in rows) / len(rows),
        "won_fraction": sum(r["outcome"] for r in rows) / len(rows),
        "calibration_gap_points": calibration_gap(rows),
        "p_under_correct_market": null_probability(rows),
        "clustered_ci": clustered_gap_ci(rows),
    }


def build_report(paths=None, now_ms=None):
    import time
    from openterminal_paths import evidence_file
    now_ms = now_ms if now_ms is not None else int(time.time() * 1000)
    paths = paths or [evidence_file("spot-calibrator-state.json")]

    rows = []
    for path in paths:
        rows.extend(load_rows(path))
    rank_within_events(rows)

    by_rank = {}
    for rank in sorted({r["rank"] for r in rows}):
        group = [r for r in rows if r["rank"] == rank]
        by_rank[rank] = summarise(group)

    # The comparison that matters: displayed vs hidden WITHIN a price band, so
    # the answer is about attention and not about moneyness.
    within_band = {}
    for lo, hi in PRICE_BANDS:
        band = [r for r in rows if lo <= r["mid"] < hi]
        shown = [r for r in band if r["displayed"]]
        hidden = [r for r in band if not r["displayed"]]
        entry = {"contracts": len(band),
                 "displayed": summarise(shown),
                 "hidden": summarise(hidden)}
        if not shown or not hidden:
            entry["comparable"] = False
            entry["reason"] = ("band contains only %s contracts — nothing to compare"
                               % ("displayed" if shown else "hidden"))
        else:
            entry["comparable"] = True
            entry["gap_difference_points"] = (calibration_gap(hidden) - calibration_gap(shown))
        within_band["%.2f-%.2f" % (lo, hi)] = entry

    return {
        "event": "strike_attention",
        "advisory_only": True,
        "analysis_only": True,
        "generated_at_ms": now_ms,
        "hypothesis": ("strikes outside the default display window get less order flow and may "
                       "therefore be mispriced, at the SAME moneyness"),
        "display_ranks_assumed": DISPLAY_RANKS,
        "display_rank_is_an_assumption": ("Kalshi's UI rule is not in any data held here; rank by "
                                          "distance from spot is the measurable proxy, and the "
                                          "per-rank table is reported so the cutoff need not be "
                                          "trusted"),
        "confound": ("rank and price are nearly collinear, so raw rank buckets would measure "
                     "moneyness and call it attention; the comparison is made WITHIN price bands"),
        "min_sqrt_minutes": MIN_SQRT_MINUTES,
        "total_contracts": len(rows),
        "total_events": len({r["event"] for r in rows}),
        "events_from_recorded_id": sum(1 for r in rows if r.get("event_is_exact")),
        "events_from_proxy": sum(1 for r in rows if not r.get("event_is_exact")),
        "proxy_warning": ("rows without a recorded event_ticker are grouped by a "
                          "(time-left, volatility) proxy that OVERSPLITS real ladders; "
                          "ranks and clustering on those rows are unreliable"),
        "by_rank": by_rank,
        "displayed_vs_hidden_within_price_band": within_band,
        "note": "ANALYSIS ONLY. No admission, sizing, model or sealed-parameter change.",
    }


def main(argv=None):
    print(json.dumps(build_report(), indent=2, sort_keys=True, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
