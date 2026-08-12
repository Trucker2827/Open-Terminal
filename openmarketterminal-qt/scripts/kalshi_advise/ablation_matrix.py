#!/usr/bin/env python3
"""ANALYSIS ONLY. Isolates what each feature group contributes over the market.

Nothing here is read by the bot, the gate, or any admission path. It writes a
report and changes no behaviour. That separation is the point: the question
"does the model beat the market" cannot be answered by the same code that acts
on the answer.

WHY THIS EXISTS
---------------
`yes_mid` -- the market's own price -- is a member of PHYSICS_FEATURES, so the
production model receives the market probability as an ordinary input and is
then asked whether it beats the market. A model that learns w_mid ~ 1 and
zeroes everything else IS the market, and would score a Brier essentially equal
to it. Measured: brier_full 0.0806 vs brier_market_mid_raw 0.0780.

That does not make including the price wrong -- a model explicitly trying to
improve on a baseline may legitimately consume it. It makes the CURRENT
EXPERIMENT ambiguous: it cannot say what the non-market features contribute,
because it never runs without them.

THE SLICES ARE PREREGISTERED AND MODEL-INDEPENDENT
--------------------------------------------------
Every slice below is defined on the MARKET price or on z, never on a candidate
model's own output. The production `bet_eligible` slice is defined by
|p - mid| >= 0.10 -- the model's own disagreement -- which gives each candidate
a different test population and makes the comparison meaningless. That is the
mistake this file must not repeat.

FOLDS
-----
Walk-forward in settlement order, scored BEFORE training on each contract, and
the fold structure is the contract order itself -- so every model sees exactly
the same folds and exactly the same test rows.

Order comes from `resolved_record`'s append order. There is no timestamp in the
record; append order is settlement order, and that assumption is stated here
rather than hidden.
"""
from __future__ import annotations

import json
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import spot_calibrator as sc
from openterminal_paths import evidence_file

BOOTSTRAP_SEED = 20260812
BOOTSTRAP_SAMPLES = 4000

MARKET = ("yes_mid",)
PHYSICS_NO_MARKET = tuple(f for f in sc.PHYSICS_FEATURES if f != "yes_mid")
PHYSICS = tuple(sc.PHYSICS_FEATURES)
FULL = tuple(sc.FULL_FEATURES)
SIGNALS = tuple(sc.ENSEMBLE_FEATURES)

# The six models, in the order they answer the question.
MODELS = (
    ("market_raw", None),                  # the actual baseline: p = yes_mid
    ("market_logit", MARKET),              # does fitting/calibrating the mid alone change it?
    ("physics_no_market", PHYSICS_NO_MARKET),  # an INDEPENDENT structural forecast
    ("market_physics", PHYSICS),           # incremental value over the market
    ("market_physics_signals", FULL),      # the eventual complete model
    ("signals_only", SIGNALS),             # do the aux feeds carry standalone information?
)


def _slices(mid, z):
    """Preregistered, model-independent evaluation slices.

    Keyed on the MARKET price and on z only. `eval_band` is the fixed
    preregistered slice: the range where a bet is plausible at all, chosen
    before any model was run and not tuned to any result.
    """
    out = ["all"]
    if 0.10 <= mid <= 0.90:
        out.append("eval_band")
    if mid > 0.70:
        out.append("favorites_gt70")
    if mid < 0.30:
        out.append("longshots_lt30")
    if z is not None and abs(z) < 0.5:
        out.append("near_boundary")
    if z is not None and abs(z) >= 2.0:
        out.append("extreme_z")
    return out


def event_key(obs):
    """A proxy for "same settlement event", used to CLUSTER the bootstrap.

    Contracts in one hourly event are different strikes on ONE price path. They
    are not independent draws, and resampling them individually treats 264
    correlated contracts as 264 independent observations -- which makes every
    confidence interval far too narrow. Measured: gold's 264 contracts span 81
    groups, silver's 264 span 45, WTI's 55 span SEVEN.

    Contracts observed at the same spot and the same time-to-close belong to the
    same event. That is a proxy -- resolved_record carries no event id -- and it
    is deliberately conservative: if it merges two genuinely distinct events the
    interval only gets wider.
    """
    spot = obs.get("spot")
    left = obs.get("sqrt_minutes_left")
    return (round(float(spot), 2) if isinstance(spot, (int, float)) else None,
            round(float(left), 3) if isinstance(left, (int, float)) else None)


def _clip(p, eps=1e-6):
    return min(1.0 - eps, max(eps, p))


def walk_forward(records, features):
    """Score every contract before training on it. Returns per-contract rows.

    `features is None` means the raw market baseline: no model, no fitting,
    p = yes_mid. It still walks the same rows in the same order so its output
    aligns row-for-row with every fitted model.
    """
    model = sc.OnlineLogit(features) if features else None
    rows = []
    for record in records:
        observations = record.get("observations") or []
        if not observations:
            continue
        obs = observations[-1]
        mid = obs.get("yes_mid")
        if not isinstance(mid, (int, float)) or not (0.0 < mid < 1.0):
            continue
        outcome = 1.0 if record.get("outcome") else 0.0
        p = float(mid) if model is None else float(model.predict(obs))
        rows.append({
            "p": _clip(p),
            "outcome": outcome,
            "mid": float(mid),
            "z": obs.get("required_move_sigma"),
            "event": event_key(obs),
        })
        if model is not None:
            model.update(obs, outcome)
    return rows


def brier(rows):
    return sum((r["p"] - r["outcome"]) ** 2 for r in rows) / len(rows) if rows else None


def log_loss(rows):
    if not rows:
        return None
    return -sum(r["outcome"] * math.log(r["p"]) + (1 - r["outcome"]) * math.log(1 - r["p"])
                for r in rows) / len(rows)


def calibration_error(rows, bins=10):
    """RMS calibration error in probability POINTS.

    This is the square root of Murphy's reliability term: the typical distance
    between what the model said and what actually happened at that confidence.
    A claimed edge smaller than this number is inside the model's own noise.
    """
    if not rows:
        return None
    buckets = {}
    for r in rows:
        k = min(bins - 1, int(r["p"] * bins))
        buckets.setdefault(k, []).append(r)
    total = 0.0
    for group in buckets.values():
        mean_p = sum(g["p"] for g in group) / len(group)
        freq = sum(g["outcome"] for g in group) / len(group)
        total += len(group) * (mean_p - freq) ** 2
    return math.sqrt(total / len(rows)) * 100.0


def paired_bootstrap(model_rows, base_rows, seed=BOOTSTRAP_SEED, samples=BOOTSTRAP_SAMPLES):
    """CI on the paired Brier delta, resampling EVENTS not contracts.

    Paired because both models scored the identical rows in the identical
    order. delta = Brier(baseline) - Brier(model), so POSITIVE means the model
    improved on the baseline. An interval containing zero means no established
    difference, whatever the point estimate says.

    CLUSTERED: the resampling unit is the settlement event, because contracts
    within one event are strikes on a single price path and carry one draw's
    worth of information between them. Resampling contracts individually would
    treat silver's 45 real events as 264 independent observations and report
    intervals several times too narrow. `n_events` is published beside every
    interval so a reader can see the evidence actually behind it.
    """
    n = len(model_rows)
    if n < 2 or n != len(base_rows):
        return None
    # SIGN CONVENTION, frozen: delta = Brier(baseline) - Brier(model).
    # POSITIVE means the model IMPROVED on the baseline. Promotion therefore
    # requires the whole interval ABOVE zero. Stated here once and used
    # everywhere, because a rule written in one sign and computed in the other
    # is a contradiction that survives every review.
    deltas = [(b["p"] - b["outcome"]) ** 2 - (m["p"] - m["outcome"]) ** 2
              for m, b in zip(model_rows, base_rows)]
    clusters = {}
    for i, row in enumerate(model_rows):
        clusters.setdefault(row.get("event"), []).append(deltas[i])
    groups = list(clusters.values())
    if len(groups) < 2:
        return {"delta": sum(deltas) / n, "lo": None, "hi": None,
                "n_events": len(groups),
                "note": "too few independent events to bound"}
    rng = random.Random(seed)
    means = []
    for _ in range(samples):
        drawn = []
        for _ in range(len(groups)):
            drawn.extend(groups[rng.randrange(len(groups))])
        means.append(sum(drawn) / len(drawn))
    means.sort()
    return {
        "delta": sum(deltas) / n,
        "lo": means[int(0.025 * samples)],
        "hi": means[int(0.975 * samples)],
        "n_events": len(groups),
    }


def evaluate_family(records):
    """Every model over the same rows, reported on every preregistered slice."""
    per_model = {name: walk_forward(records, feats) for name, feats in MODELS}
    baseline = per_model["market_raw"]
    if not baseline:
        return None

    slice_index = {}
    for i, row in enumerate(baseline):
        for name in _slices(row["mid"], row["z"]):
            slice_index.setdefault(name, []).append(i)

    out = {"contracts": len(baseline), "slices": {}}
    for slice_name, idx in sorted(slice_index.items()):
        entry = {"n": len(idx), "models": {}}
        base_sub = [baseline[i] for i in idx]
        for model_name, _ in MODELS:
            sub = [per_model[model_name][i] for i in idx]
            record = {
                "brier": brier(sub),
                "log_loss": log_loss(sub),
                "calibration_error_points": calibration_error(sub),
            }
            if model_name != "market_raw":
                record["vs_market_raw"] = paired_bootstrap(sub, base_sub)
            entry["models"][model_name] = record
        out["slices"][slice_name] = entry
    return out


def build_report(now_ms=None):
    import time
    now_ms = now_ms if now_ms is not None else int(time.time() * 1000)
    families = {}
    for path, label in (
        (evidence_file("commodities-hourly-calibrator-state.json"), "hourly"),
        (evidence_file("commodities-15m-calibrator-state.json"), "15m"),
        (evidence_file("commodities-daily-calibrator-state.json"), "daily"),
    ):
        try:
            state = json.load(open(path, "r", encoding="utf-8"))
        except (OSError, ValueError):
            continue
        for family, slice_state in sorted((state.get("by_family") or {}).items()):
            records = slice_state.get("resolved_record") or []
            if len(records) < 30:
                # Reported as skipped rather than silently absent: "too few to
                # measure" and "not measured" must not look identical.
                families[family] = {"skipped": "only %d resolved contracts" % len(records)}
                continue
            result = evaluate_family(records)
            if result:
                families[family] = result
    return {
        "event": "ablation_matrix",
        "advisory_only": True,
        "analysis_only": True,
        "generated_at_ms": now_ms,
        "note": ("Slices are preregistered and defined on the MARKET price or z only, never on a "
                 "candidate model's own output, so every model is scored on the identical "
                 "population. Walk-forward, scored before training, same folds for all models."),
        "models": [name for name, _ in MODELS],
        "slice_definitions": {
            "all": "every settled contract",
            "eval_band": "0.10 <= market mid <= 0.90 (fixed, preregistered)",
            "favorites_gt70": "market mid > 0.70",
            "longshots_lt30": "market mid < 0.30",
            "near_boundary": "|z| < 0.5",
            "extreme_z": "|z| >= 2.0",
        },
        "families": families,
    }


def main(argv=None):
    report = build_report()
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
