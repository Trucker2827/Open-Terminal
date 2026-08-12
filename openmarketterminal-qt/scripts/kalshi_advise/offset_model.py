#!/usr/bin/env python3
"""ANALYSIS ONLY. The market as an OFFSET rather than an ordinary feature.

    logit(p) = logit(p_market) + g(x)

Nothing here is read by the bot, the gate, or any admission path. It writes a
report and changes no behaviour.

WHY THE OFFSET FORM
-------------------
Production puts `yes_mid` in the feature vector, so the model must LEARN the
market's coefficient alongside everything else. The ablation (#250) measured
what that costs: `market_logit` -- a logistic fitted on the mid ALONE -- scored
significantly WORSE than the raw mid on both families (gold +0.0402, silver
+0.0819). The learner cannot reproduce the baseline it is handed.

As an offset the market is not learned at all:

    g = 0            -> p == p_market, exactly, by construction
    g > 0            -> raise the market's probability
    g < 0            -> lower it

So the question "does the model add value" becomes exactly "does g improve
held-out scoring", which is a question with an answer. This is NOT a claim that
the offset predicts better. It guarantees faithful reproduction at g = 0 and
nothing more; whether g earns its place is what this measures.

WHAT g IS ALLOWED TO SEE
------------------------
Residual information only: features that are not the market price and are not
0.0 stubs. `ENSEMBLE_FEATURES` are excluded entirely rather than passed as
zero-valued columns -- a column that is always 0.0 contributes nothing but
still consumes a weight and dilutes the regulariser.

Note `required_move_sigma` is NOT algebraically redundant with the other three
physics terms: it equals distance/(vol*sqrt_T) on only 31 of 264 gold rows, so
it carries information the naive ratio does not.
"""
from __future__ import annotations

import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ablation_matrix as ab
import spot_calibrator as sc
from openterminal_paths import evidence_file

# Probabilities are clipped ONLY so logit() is finite. The clip is reported in
# the output so a reader can see it was applied and how tight it is; it is not
# a modelling choice and must never be tuned to improve a score.
LOGIT_CLIP = 1e-6

# Residual features: physics minus the market price, minus every stub.
RESIDUAL_FEATURES = tuple(f for f in sc.PHYSICS_FEATURES
                          if f != "yes_mid" and f not in sc.ENSEMBLE_FEATURES)

# L2 pulling g's weights toward ZERO -- i.e. toward "accept the market". The
# prior is that the market is right, and g must overcome it with evidence.
L2 = 1e-2
LEARNING_RATE = 0.05

# Below this many settled contracts a family's residual model has no evidence,
# and the do-no-harm comparator falls back to the market instead of guessing.
MIN_RESIDUAL_CONTRACTS = 40


def logit(p):
    p = min(1.0 - LOGIT_CLIP, max(LOGIT_CLIP, p))
    return math.log(p / (1.0 - p))


def sigmoid(x):
    if x >= 0:
        z = math.exp(-x)
        return 1.0 / (1.0 + z)
    z = math.exp(x)
    return z / (1.0 + z)


class OffsetLogit:
    """Online logistic regression on an OFFSET.

    p = sigmoid(logit(market) + w . x_standardised)

    With all weights zero the prediction is the market's, exactly -- that is
    the identity the tests pin. Gradient is the same as ordinary logistic
    regression; only the starting point moves.
    """

    def __init__(self, features, lr=LEARNING_RATE, l2=L2):
        self.features = tuple(features)
        self.lr = lr
        self.l2 = l2
        n = len(self.features)
        self.w = [0.0] * n          # no bias: a bias would shift EVERY market
                                    # price by a constant, which is not a
                                    # residual claim about any contract.
        self.g2 = [0.0] * n
        self.n_seen = 0
        self.mean = [0.0] * n
        self.m2 = [0.0] * n

    def _standardise(self, obs):
        out = []
        for i, name in enumerate(self.features):
            raw = obs.get(name)
            raw = float(raw) if isinstance(raw, (int, float)) else 0.0
            if self.n_seen < 2:
                out.append(0.0)
                continue
            var = self.m2[i] / (self.n_seen - 1)
            sd = math.sqrt(var) if var > 0 else 0.0
            out.append(0.0 if sd == 0.0 else (raw - self.mean[i]) / sd)
        return out

    def adjustment(self, obs):
        """g(x) itself, in log-odds. Zero when untrained."""
        x = self._standardise(obs)
        return sum(w * xi for w, xi in zip(self.w, x))

    def predict(self, obs, market_p):
        return sigmoid(logit(market_p) + self.adjustment(obs))

    def update(self, obs, market_p, outcome):
        x = self._standardise(obs)
        p = sigmoid(logit(market_p) + sum(w * xi for w, xi in zip(self.w, x)))
        err = p - outcome
        for i in range(len(self.w)):
            grad = err * x[i] + self.l2 * self.w[i]
            self.g2[i] += grad * grad
            self.w[i] -= self.lr * grad / (math.sqrt(self.g2[i]) + 1e-8)
        # Running mean/variance AFTER the step, so standardisation never uses a
        # row's own value before that row has been scored.
        self.n_seen += 1
        for i, name in enumerate(self.features):
            raw = obs.get(name)
            raw = float(raw) if isinstance(raw, (int, float)) else 0.0
            delta = raw - self.mean[i]
            self.mean[i] += delta / self.n_seen
            self.m2[i] += delta * (raw - self.mean[i])


def walk_forward_offset(records, fallback_below=None):
    """Same folds and same order as the ablation: score BEFORE training.

    `fallback_below` is the do-no-harm comparator: while the model has seen
    fewer than that many contracts it emits the market's own probability
    unchanged, so a family with no evidence cannot be harmed by a model that
    has not yet learned anything.
    """
    model = OffsetLogit(RESIDUAL_FEATURES)
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
        g = model.adjustment(obs)
        if fallback_below is not None and model.n_seen < fallback_below:
            p, applied = float(mid), 0.0
        else:
            p, applied = model.predict(obs, float(mid)), g
        rows.append({
            "p": min(1.0 - LOGIT_CLIP, max(LOGIT_CLIP, p)),
            "outcome": outcome,
            "mid": float(mid),
            "z": obs.get("required_move_sigma"),
            "event": ab.event_key(obs),
            "g": applied,
            "shift": p - float(mid),
        })
        model.update(obs, float(mid), outcome)
    return rows


def adjustment_summary(rows):
    """How often and by how much g moves the market. A model that never moves
    it is safe and useless; one that moves it constantly is making a strong
    claim that had better be earned."""
    if not rows:
        return None
    shifts = [abs(r["shift"]) for r in rows]
    moved = [s for s in shifts if s > 0.01]
    shifts_sorted = sorted(shifts)
    return {
        "contracts": len(rows),
        "moved_over_1pt": len(moved),
        "moved_fraction": len(moved) / len(rows),
        "median_abs_shift_points": shifts_sorted[len(shifts_sorted) // 2] * 100.0,
        "max_abs_shift_points": max(shifts) * 100.0,
        "mean_abs_g_logodds": sum(abs(r["g"]) for r in rows) / len(rows),
    }


def evaluate_family(records):
    """Offset vs market_raw and vs the current unconstrained market_physics,
    on the ablation's own frozen slices."""
    baseline = ab.walk_forward(records, None)                    # market_raw
    unconstrained = ab.walk_forward(records, ab.PHYSICS)         # market_physics
    offset = walk_forward_offset(records)
    guarded = walk_forward_offset(records, fallback_below=MIN_RESIDUAL_CONTRACTS)
    if not baseline:
        return None

    candidates = {
        "market_raw": baseline,
        "market_physics": unconstrained,
        "offset": offset,
        "offset_do_no_harm": guarded,
    }

    slice_index = {}
    for i, row in enumerate(baseline):
        for name in ab._slices(row["mid"], row["z"]):
            slice_index.setdefault(name, []).append(i)

    out = {
        "contracts": len(baseline),
        "adjustment": adjustment_summary(offset),
        "adjustment_do_no_harm": adjustment_summary(guarded),
        "slices": {},
    }
    for slice_name, idx in sorted(slice_index.items()):
        entry = {"n": len(idx), "models": {}}
        base_sub = [baseline[i] for i in idx]
        phys_sub = [unconstrained[i] for i in idx]
        for name, rows in candidates.items():
            sub = [rows[i] for i in idx]
            record = {"brier": ab.brier(sub), "log_loss": ab.log_loss(sub),
                      "calibration_error_points": ab.calibration_error(sub)}
            if name != "market_raw":
                record["vs_market_raw"] = ab.paired_bootstrap(sub, base_sub)
            if name not in ("market_raw", "market_physics"):
                record["vs_market_physics"] = ab.paired_bootstrap(sub, phys_sub)
            entry["models"][name] = record
        out["slices"][slice_name] = entry
    return out


def build_report(now_ms=None):
    import time
    now_ms = now_ms if now_ms is not None else int(time.time() * 1000)
    families = {}
    for path in (evidence_file("commodities-hourly-calibrator-state.json"),
                 evidence_file("commodities-15m-calibrator-state.json"),
                 evidence_file("commodities-daily-calibrator-state.json")):
        try:
            state = json.load(open(path, "r", encoding="utf-8"))
        except (OSError, ValueError):
            continue
        for family, slice_state in sorted((state.get("by_family") or {}).items()):
            records = slice_state.get("resolved_record") or []
            if len(records) < 30:
                families[family] = {"skipped": "only %d resolved contracts" % len(records)}
                continue
            result = evaluate_family(records)
            if result:
                families[family] = result
    return {
        "event": "offset_model_prototype",
        "advisory_only": True,
        "analysis_only": True,
        "generated_at_ms": now_ms,
        "form": "logit(p) = logit(p_market) + g(x)",
        "residual_features": list(RESIDUAL_FEATURES),
        "excluded": {
            "yes_mid": "it is the offset, not a feature",
            "ensemble_stubs": list(sc.ENSEMBLE_FEATURES),
        },
        "l2_toward_zero": L2,
        "logit_clip": LOGIT_CLIP,
        "do_no_harm_min_contracts": MIN_RESIDUAL_CONTRACTS,
        "promotion_rule": ("delta = Brier(market_raw) - Brier(model), so POSITIVE is improvement. "
                           "Analysis-only unless the ENTIRE vs_market_raw interval is above zero "
                           "on preregistered out-of-sample evidence, with enough independent "
                           "EVENTS to mean it; a win authorises that family x horizon ONLY, "
                           "never commodities as a class."),
        "families": families,
    }


def main(argv=None):
    print(json.dumps(build_report(), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
