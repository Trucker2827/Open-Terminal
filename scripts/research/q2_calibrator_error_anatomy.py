#!/usr/bin/env python3
"""Q2 of the Kalshi edge autopsy (issue #169): where the calibrator's error is.

Slices the reconstructed per-forecast history (see `forecast_history`) by
time-to-expiry, time of day, volatility regime and distance from the strike in
sigmas, and in each slice compares three Brier scores on the SAME rows:

  calibrated_p        the calibrator's full model — what the bot acts on
  market_mid          the raw market midpoint — the honest market baseline
  calibrator's own    `brier_market_baseline`, reported for contrast only

That third one matters. `spot_calibrator.MARKET_FEATURES = ("yes_mid",)` means
the calibrator's "market baseline" is a TRAINED one-feature logistic regression
on the mid, not the mid itself. Beating a deliberately handicapped baseline is
not the same claim as beating the market, so this script computes the raw-mid
Brier directly and prints all of them side by side.

Every slice reports its contract count as well as its row count. Rows within a
contract share one outcome and move together, so the contract count is the
effective sample size; a slice with 900 rows and 11 contracts is an n=11 result
however impressive the row count looks.

Read-only. Prints JSON to stdout; writes nothing anywhere.

  python3 scripts/research/q2_calibrator_error_anatomy.py
"""
import collections
import json
import sys

import kalshi_edge_common as common
import forecast_history


def score(rows):
    """Brier of the calibrator and of the raw mid over the same rows."""
    if not rows:
        return None
    calibrated = common.brier([(r["calibrated_p"], r["outcome"]) for r in rows])
    market = common.brier([(r["market_mid"], r["outcome"]) for r in rows])
    contracts = {r["ticker"] for r in rows}
    return {
        "rows": len(rows),
        "contracts": len(contracts),
        "brier_calibrated": calibrated,
        "brier_market_mid": market,
        "delta_vs_market": (calibrated - market
                            if calibrated is not None and market is not None
                            else None),
        "beats_market": (calibrated is not None and market is not None
                         and calibrated < market),
        "yes_rate": sum(1 for r in rows if r["outcome"]) / len(rows),
        "mean_calibrated_p": sum(r["calibrated_p"] for r in rows) / len(rows),
        "mean_market_mid": sum(r["market_mid"] for r in rows) / len(rows),
    }


def sliced(rows, key, order=None):
    """score() per slice, with slices that cannot be computed left out loudly."""
    buckets = collections.defaultdict(list)
    unassigned = 0
    for row in rows:
        label = key(row)
        if label is None:
            unassigned += 1
            continue
        buckets[label].append(row)
    labels = order if order is not None else sorted(buckets)
    return {"unassigned_rows": unassigned,
            "slices": [dict(slice=label, **score(buckets[label]))
                       for label in labels if buckets.get(label)]}


TTE_BUCKETS = (("0-2m", 0, 120), ("2-5m", 120, 300), ("5-15m", 300, 900),
               ("15-30m", 900, 1800), ("30-60m", 1800, 3600),
               ("60m+", 3600, float("inf")))


def tte_bucket(row):
    for label, lo, hi in TTE_BUCKETS:
        if lo <= row["seconds_left"] < hi:
            return label
    return None


SIGMA_BUCKETS = (("<-2s", float("-inf"), -2.0), ("-2..-1s", -2.0, -1.0),
                 ("-1..0s", -1.0, 0.0), ("0..1s", 0.0, 1.0),
                 ("1..2s", 1.0, 2.0), (">2s", 2.0, float("inf")))


def sigma_bucket(row):
    value = row.get("distance_sigma")
    if value is None:
        return None
    for label, lo, hi in SIGMA_BUCKETS:
        if lo <= value < hi:
            return label
    return None


def vol_bucket(row, terciles):
    value = row.get("vol_per_min_bps")
    if value is None or terciles is None:
        return None
    low, high = terciles
    return "low_vol" if value <= low else ("mid_vol" if value <= high else "high_vol")


def tail_confidence(rows):
    """Is the calibrator overconfident where it is most certain?

    The Gaussian-tail suspicion in the issue: a threshold model built on a
    normal approximation puts too little mass in the tails, so extreme
    probabilities should come back too extreme. Compared against the market
    mid on the same rows so a common shortfall (both wrong) is distinguishable
    from a calibrator-specific one.
    """
    bands = (("p<0.02", 0.0, 0.02), ("0.02-0.05", 0.02, 0.05),
             ("0.05-0.10", 0.05, 0.10), ("0.10-0.90", 0.10, 0.90),
             ("0.90-0.95", 0.90, 0.95), ("0.95-0.98", 0.95, 0.98),
             ("p>0.98", 0.98, 1.01))
    out = []
    for label, lo, hi in bands:
        band = [r for r in rows if lo <= r["calibrated_p"] < hi]
        if not band:
            continue
        predicted = sum(r["calibrated_p"] for r in band) / len(band)
        observed = sum(1 for r in band if r["outcome"]) / len(band)
        mid_band = [r for r in rows if lo <= r["market_mid"] < hi]
        market_gap = None
        if mid_band:
            market_gap = (sum(r["market_mid"] for r in mid_band) / len(mid_band)
                          - sum(1 for r in mid_band if r["outcome"]) / len(mid_band))
        out.append({"band": label, "rows": len(band),
                    "contracts": len({r["ticker"] for r in band}),
                    "mean_predicted": predicted, "observed_rate": observed,
                    "calibration_gap": predicted - observed,
                    "market_mid_gap_same_band": market_gap})
    return out


def calibrator_self_report():
    """What the calibrator says about itself, and the caveat that goes with it.

    Read from the state file only — `spot_calibrator.run_once()` is never
    called, because running it would rewrite the operator's live calibrator
    state as a side effect of an analysis that is supposed to be read-only.
    """
    try:
        with open(common.evidence_path("spot-calibrator-state.json"),
                  "r", encoding="utf-8") as handle:
            state = json.load(handle)
    except (OSError, ValueError) as exc:
        return {"available": False, "error": str(exc)}

    # Issue #171 fixed the defect this whole block was written to expose, so the
    # block now says which schema it is looking at instead of `.get`-degrading
    # to zeros against the new one. The schema-1 branch below is kept verbatim:
    # it is the autopsy's published evidence and must stay reproducible against
    # an archived schema-1 state file.
    schema = int(state.get("schema") or 1)
    if schema >= 2:
        scored = state.get("contract_scores_full") or []
        mean = (lambda values: sum(values) / len(values) if values else None)
        return {
            "available": True,
            "state_schema": schema,
            "resolved_contracts_lifetime": state.get("resolved"),
            "skipped_unmodeled": state.get("skipped_unmodeled"),
            "scored_contracts": len(scored),
            "training_observations": (state.get("full") or {}).get("n_seen"),
            "brier_full": mean(scored),
            "brier_market_mid_raw": mean(state.get("contract_scores_market_mid_raw") or []),
            "brier_market_trained_logit":
                mean(state.get("contract_scores_market_trained_logit") or []),
            "discarded_observation_pairs": state.get("discarded_observation_pairs"),
            "note": ("schema 2 scores one number per CONTRACT (mean squared error "
                     "over that contract's observations, predicted before "
                     "training on it), so scored_contracts IS the effective "
                     "sample and no per-observation correction applies; "
                     "brier_market_mid_raw is the untrained mid, "
                     "brier_market_trained_logit the handicapped 1-feature fit"),
        }

    full = [(p, y) for p, y in state.get("brier_full", [])]
    market = [(p, y) for p, y in state.get("brier_market", [])]

    # How many observations a contract actually contributes is not a constant
    # to be asserted — it is recorded, in the calibrator's own `pending` map,
    # which holds the obs list it will train on when each ticker settles. That
    # distribution is the honest denominator for turning 500 stored pairs into
    # an effective contract count, so it is measured here rather than assumed
    # from MAX_OBS_PER_TICKER.
    pending_obs = sorted(len(entry.get("obs") or [])
                         for entry in (state.get("pending") or {}).values())
    effective = None
    if pending_obs:
        mean_obs = sum(pending_obs) / len(pending_obs)
        effective = {
            "pending_contracts_measured": len(pending_obs),
            "obs_per_contract_min": pending_obs[0],
            "obs_per_contract_median": pending_obs[len(pending_obs) // 2],
            "obs_per_contract_mean": mean_obs,
            "obs_per_contract_max": pending_obs[-1],
            "effective_contracts_at_mean_obs": len(full) / mean_obs if mean_obs else None,
            "effective_contracts_at_max_obs": (len(full) / pending_obs[-1]
                                               if pending_obs[-1] else None),
        }
    return {
        "available": True,
        "state_schema": schema,
        "resolved_contracts_lifetime": state.get("resolved"),
        "skipped_unmodeled": state.get("skipped_unmodeled"),
        "brier_full": common.brier(full),
        "brier_market_baseline": common.brier(market),
        "stored_pairs": len(full),
        "pairs_are_observations_not_contracts": True,
        "max_obs_per_contract_constant": 60,
        "effective_sample": effective,
        # The theoretical bound, kept alongside the measured figure so the two
        # are never confused: one pair per contract at worst, 60 at best.
        "implied_contract_count_bound": [len(full) // 60 if full else 0,
                                         len(full)],
        "caveat": ("brier_full holds one pair per OBSERVATION (up to 60 per "
                   "contract) truncated to the last 500, so 'training_samples: "
                   "500' is a far smaller number of contracts — see "
                   "effective_sample, measured from this state file's own "
                   "pending obs distribution; brier_market_baseline is a "
                   "trained 1-feature logit on the mid, not the mid itself"),
    }


def main():
    history = forecast_history.build()
    rows = history["rows"]
    if not rows:
        common.emit({"as_of_utc": common.as_of(), "question": "Q2",
                     "verdict": "NO DATA — no forecast row could be resolved",
                     "audit": history["audit"]})
        return 1

    vols = sorted(r["vol_per_min_bps"] for r in rows
                  if r.get("vol_per_min_bps") is not None)
    terciles = ((vols[len(vols) // 3], vols[2 * len(vols) // 3])
                if len(vols) >= 3 else None)

    recorded = [r for r in rows if r["outcome_source"] == "recorded"]

    common.emit({
        "as_of_utc": common.as_of(),
        "question": "Q2 — calibrator error anatomy",
        "command": "python3 scripts/research/q2_calibrator_error_anatomy.py",
        "audit": history["audit"],
        "calibrator_self_report": calibrator_self_report(),
        "overall": score(rows),
        "overall_recorded_settlements_only": score(recorded),
        "by_time_to_expiry": sliced(rows, tte_bucket,
                                    order=[b[0] for b in TTE_BUCKETS]),
        "by_hour_utc": sliced(rows, lambda r: r["hour_utc"]),
        "by_vol_regime": sliced(rows, lambda r: vol_bucket(r, terciles),
                                order=["low_vol", "mid_vol", "high_vol"]),
        "vol_terciles_per_min_bps": terciles,
        "by_distance_sigma": sliced(rows, sigma_bucket,
                                    order=[b[0] for b in SIGMA_BUCKETS]),
        "by_family": sliced(rows, lambda r: r["family"]),
        "by_outcome_source": sliced(rows, lambda r: r["outcome_source"]),
        "tail_confidence": tail_confidence(rows),
        "reliability_calibrated": common.reliability_bins(
            [(r["calibrated_p"], r["outcome"]) for r in rows]),
        "reliability_market_mid": common.reliability_bins(
            [(r["market_mid"], r["outcome"]) for r in rows]),
    })
    return 0


if __name__ == "__main__":
    sys.exit(main())
