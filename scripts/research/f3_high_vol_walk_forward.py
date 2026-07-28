#!/usr/bin/env python3
"""F3 (issue #172): does the high-volatility exception survive out-of-sample?

The Kalshi edge autopsy (#169, `docs/research/2026-07-27-kalshi-edge-autopsy.md`)
found the calibrator loses to the raw market mid overall (Brier 0.1043 vs
0.0989) and in nearly every slice — with ONE non-trivial exception: the top
tercile of trailing per-minute realized volatility, where it scored 0.1151
against the mid's 0.1218 over 116 contracts. That is the only contested slice in
the whole report where the calibrator wins, and it is mechanistically consistent
with Q1's finding that the Kalshi quote is measurably stale in fast markets.

It is also one cut of one 3-day window, and it was not the hypothesis the slice
was designed to test. This script is the follow-up measurement that such a
result has to survive.

WHAT IS AND IS NOT OUT-OF-SAMPLE HERE. Nothing is fitted. Comparing a recorded
`calibrated_p` against a recorded `market_mid` estimates no parameters, so the
fold structure below is NOT protecting against model overfitting — there is no
model to overfit. It buys exactly two things, and the report is required to
claim only these:

  1. the volatility threshold is FROZEN before any fold is scored, so the cut
     point cannot refit itself to the data it is evaluated on (the original
     tercile boundary was, by construction, derived from the same rows it
     described);
  2. per-fold sign stability — whether the exception holds across sequential
     slices of time or lives in one lucky window.

THE SPLIT IS BY CONTRACT CLOSE TIME, NEVER BY ROW, for the reason the autopsy's
Q3 established: a contract contributes up to 56 forecast rows that all share one
outcome, so a row-wise split puts the same outcome on both sides of the boundary
and manufactures out-of-sample "improvement" out of nothing. Fold boundaries are
additionally snapped to close-time group edges, so no two contracts closing at
the same instant land in different folds and every fold is strictly in the
future of the one before it.

BOTH WEIGHTINGS ARE REPORTED. Pooled-row Brier is comparable to the autopsy's
published cell; contract-mean Brier gives each contract one vote. Rows inside a
contract are correlated, so the contract count — printed for every cell — is the
honest sample size, and the paired per-contract difference with its t and exact
sign test is what answers "at what n".

Read-only: opens evidence, writes nothing, and never calls
`spot_calibrator.run_once()`.

  python3 scripts/research/f3_high_vol_walk_forward.py
"""
import collections
import math
import sys

import kalshi_edge_common as common
import forecast_history

# ── the frozen threshold ─────────────────────────────────────────────────────
# 3.82 bps/min is the upper tercile boundary PUBLISHED BY #169 (report table
# "By volatility regime", as-of 2026-07-27T21:41Z). It is written here as a
# literal, fixed before this evaluation was run, and is never recomputed from
# the data this script scores — that is the whole point of the criterion. Its
# honest caveat, which the report states: #169 derived it from a window that
# overlaps this one, so it is frozen but not independent. The alternative —
# re-deriving a tercile from an in-time training prefix — was tried and is
# reported as `prefix_derived_threshold_diagnostic`, which shows why it was
# rejected rather than merely asserting a preference.
FROZEN_VOL_THRESHOLD_BPS = 3.82
FROZEN_THRESHOLD_SOURCE = ("upper tercile boundary published by issue #169, "
                           "docs/research/2026-07-27-kalshi-edge-autopsy.md, "
                           "as-of 2026-07-27T21:41Z; fixed before this run")

# Sensitivity grid. A result that exists at exactly one cut point is a finding
# about the cut point, so the frozen value is scored inside a sweep that spans
# the lower tercile boundary up to a cut that keeps only the fastest markets.
SENSITIVITY_THRESHOLDS_BPS = (2.45, 3.00, 3.40, 3.82, 4.25, 5.00, 6.00, 8.00)

WALK_FORWARD_FOLDS = 5
MIN_FOLD_CONTRACTS = 5        # below this a "fold" is an anecdote, not a slice
MIN_SUBSET_CONTRACTS = 25     # below this no walk-forward is attempted at all
PREFIX_FRACTION = 1.0 / 3.0   # for the rejected prefix-derived variant

# An hour with no selected row separates one volatility EPISODE from the next.
# Contracts inside a single episode are priced off one BTC path, so they are not
# independent observations of "the calibrator in fast markets" — they are one
# observation seen many times. The episode count, not the contract count, is the
# honest denominator for the verdict, which is why it is computed rather than
# left for a reader to notice.
EPISODE_GAP_MS = 60 * 60_000


# ── subset construction ──────────────────────────────────────────────────────
def filter_by_vol(contracts, threshold_bps, regime):
    """Contracts carrying at least one row in `regime` relative to `threshold`.

    A row is selected purely by its own recorded `vol_per_min_bps` against the
    passed threshold — never by where it sits in the surrounding distribution.
    That is what makes the threshold frozen rather than adaptive: the same row
    with the same volatility is selected identically no matter what else is in
    the dataset or which fold it is being scored in.

    Rows whose volatility could not be computed (the BRTI window under the grid
    point held too few samples) are DROPPED and counted, never assumed to be on
    one side of the cut.
    """
    if regime not in ("high", "low_mid"):
        raise ValueError("regime must be 'high' or 'low_mid'")
    kept = []
    dropped_rows_without_vol = 0
    for contract in contracts:
        rows = []
        for row in contract["rows"]:
            vol = row.get("vol_per_min_bps")
            if vol is None:
                dropped_rows_without_vol += 1
                continue
            if (vol > threshold_bps) if regime == "high" else (vol <= threshold_bps):
                rows.append(row)
        if rows:
            entry = dict(contract)
            entry["rows"] = rows
            entry["observations"] = len(rows)
            kept.append(entry)
    kept.sort(key=lambda c: (c["close_ms"], c["ticker"]))
    return kept, dropped_rows_without_vol


# ── scoring ──────────────────────────────────────────────────────────────────
def _contract_brier(contract, field):
    rows = contract["rows"]
    return sum((row[field] - (1.0 if row["outcome"] else 0.0)) ** 2
               for row in rows) / len(rows)


def _binomial_two_sided_p(wins, n):
    """Exact two-sided sign-test p under H0: p=0.5. Stdlib only, no scipy."""
    if n <= 0:
        return None
    tail = min(wins, n - wins)
    cumulative = sum(math.comb(n, k) for k in range(tail + 1))
    return min(1.0, 2.0 * cumulative / (2.0 ** n))


def score_cell(contracts):
    """Every number a cell of this report is allowed to print.

    Returns None for an empty cell rather than a zero: a Brier of 0.0 is a
    perfect forecast, which is exactly the wrong thing for "no data" to look
    like.
    """
    if not contracts:
        return None
    rows = [row for contract in contracts for row in contract["rows"]]
    pooled_cal = common.brier([(r["calibrated_p"], r["outcome"]) for r in rows])
    pooled_mid = common.brier([(r["market_mid"], r["outcome"]) for r in rows])

    per_contract_cal = [_contract_brier(c, "calibrated_p") for c in contracts]
    per_contract_mid = [_contract_brier(c, "market_mid") for c in contracts]
    diffs = [cal - mid for cal, mid in zip(per_contract_cal, per_contract_mid)]
    n = len(diffs)
    mean_diff = sum(diffs) / n
    contract_mean_cal = sum(per_contract_cal) / n
    contract_mean_mid = sum(per_contract_mid) / n

    t_stat = standard_error = None
    if n >= 2:
        variance = sum((d - mean_diff) ** 2 for d in diffs) / (n - 1)
        standard_error = math.sqrt(variance / n)
        if standard_error > 0.0:
            t_stat = mean_diff / standard_error

    wins = sum(1 for d in diffs if d < 0.0)     # calibrator beat the mid
    ties = sum(1 for d in diffs if d == 0.0)
    decided = n - ties

    observations = sorted(len(c["rows"]) for c in contracts)
    return {
        "contracts": n,
        "rows": len(rows),
        "observations_per_contract": {
            "min": observations[0],
            "median": observations[len(observations) // 2],
            "mean": sum(observations) / len(observations),
            "max": observations[-1],
        },
        "pooled_row_brier_calibrated": pooled_cal,
        "pooled_row_brier_market_mid": pooled_mid,
        "pooled_row_delta": pooled_cal - pooled_mid,
        "contract_mean_brier_calibrated": contract_mean_cal,
        "contract_mean_brier_market_mid": contract_mean_mid,
        "contract_mean_delta": contract_mean_cal - contract_mean_mid,
        "paired_by_contract": {
            "n": n,
            "mean_delta": mean_diff,
            "standard_error": standard_error,
            "t": t_stat,
            "contracts_calibrator_better": wins,
            "contracts_market_better": decided - wins,
            "contracts_tied": ties,
            "sign_test_two_sided_p": _binomial_two_sided_p(wins, decided),
        },
        # Negative delta = the calibrator beat the raw mid on this cell.
        "calibrator_beats_mid_pooled": pooled_cal < pooled_mid,
        "calibrator_beats_mid_contract_mean": contract_mean_cal < contract_mean_mid,
    }


# ── folds ────────────────────────────────────────────────────────────────────
def fold_slices(contracts, folds):
    """Sequential index ranges over close-time-ordered contracts.

    Boundaries are SNAPPED FORWARD to close-time group edges so a set of
    contracts sharing one close instant is never split across two folds. That
    makes every fold strictly later than its predecessor — "the test set is in
    the future of the training set" is then a property of the partition, not a
    hope about it. Folds that would fall below `MIN_FOLD_CONTRACTS` are merged
    into their predecessor rather than reported as a slice.
    """
    total = len(contracts)
    if total == 0 or folds < 1:
        return []
    target = total / folds
    bounds = []
    cursor = 0
    for index in range(1, folds):
        edge = int(round(index * target))
        edge = max(edge, cursor)
        # snap forward past any contract sharing the boundary's close time
        while 0 < edge < total and (contracts[edge]["close_ms"]
                                    == contracts[edge - 1]["close_ms"]):
            edge += 1
        if edge <= cursor or edge >= total:
            continue
        bounds.append(edge)
        cursor = edge
    edges = [0] + bounds + [total]
    slices = []
    for start, end in zip(edges, edges[1:]):
        if end - start < MIN_FOLD_CONTRACTS and slices:
            slices[-1] = (slices[-1][0], end)
        elif end > start:
            slices.append((start, end))
    return slices


def walk_forward(contracts, folds=WALK_FORWARD_FOLDS):
    """Per-fold and pooled scores over sequential slices of contract close time.

    Because nothing is fitted, the pooled figure is by construction identical to
    scoring the whole subset at once — the folds add temporal resolution, not a
    separate estimate, and the report says so rather than presenting `pooled`
    as if it were a held-out number it is not.
    """
    if len(contracts) < MIN_SUBSET_CONTRACTS:
        return {
            "folds": [],
            "pooled": None,
            "note": (f"INSUFFICIENT DATA — {len(contracts)} contracts, "
                     f"{MIN_SUBSET_CONTRACTS} required for a walk-forward"),
        }
    results = []
    for index, (start, end) in enumerate(fold_slices(contracts, folds), start=1):
        fold_contracts = contracts[start:end]
        cell = score_cell(fold_contracts)
        if cell is None:
            continue
        cell["fold"] = index
        cell["close_span_utc"] = [common.iso(fold_contracts[0]["close_ms"]),
                                  common.iso(fold_contracts[-1]["close_ms"])]
        cell["tickers"] = sorted({c["ticker"] for c in fold_contracts})
        results.append(cell)

    pooled = score_cell(contracts)
    folds_won = sum(1 for f in results if f["pooled_row_delta"] < 0.0)
    return {
        "folds": [{k: v for k, v in f.items() if k != "tickers"} for f in results],
        "leakage_check": _leakage_check(results),
        "pooled": pooled,
        "folds_scored": len(results),
        "folds_calibrator_better_pooled_row": folds_won,
        "folds_calibrator_better_contract_mean": sum(
            1 for f in results if f["contract_mean_delta"] < 0.0),
        "pooled_equals_full_subset_by_construction": True,
    }


def _leakage_check(folds):
    """No ticker may appear in two folds, and folds must not overlap in time."""
    seen = collections.Counter()
    for fold in folds:
        seen.update(fold.get("tickers", []))
    repeated = sorted(t for t, count in seen.items() if count > 1)
    ordered = True
    for earlier, later in zip(folds, folds[1:]):
        if earlier["close_span_utc"][1] > later["close_span_utc"][0]:
            ordered = False
    return {"tickers_in_more_than_one_fold": len(repeated),
            "examples": repeated[:5],
            "folds_strictly_time_ordered": ordered,
            "clean": not repeated and ordered}


# ── how many independent things are actually being measured ─────────────────
def episode_structure(contracts, gap_ms=EPISODE_GAP_MS):
    """Cluster the selected rows into volatility episodes and score each.

    A walk-forward over contract close time answers "is this stable across
    time?" only to the extent that the subset IS spread across time. High
    volatility does not arrive uniformly — it arrives in bursts — so a subset
    of 116 contracts can be a handful of bursts wearing a large-n costume.
    This function measures which of the two it is, and the report leads with
    the answer rather than with the contract count.
    """
    rows = sorted((row for contract in contracts for row in contract["rows"]),
                  key=lambda r: r["ts_ms"])
    if not rows:
        return {"episodes": [], "episode_count": 0,
                "note": "no selected row — nothing to cluster"}
    clusters = [[rows[0]]]
    for row in rows[1:]:
        if row["ts_ms"] - clusters[-1][-1]["ts_ms"] > gap_ms:
            clusters.append([row])
        else:
            clusters[-1].append(row)

    episodes = []
    for index, cluster in enumerate(clusters, start=1):
        tickers = {row["ticker"] for row in cluster}
        calibrated = common.brier([(r["calibrated_p"], r["outcome"]) for r in cluster])
        market = common.brier([(r["market_mid"], r["outcome"]) for r in cluster])
        episodes.append({
            "episode": index,
            "start_utc": common.iso(cluster[0]["ts_ms"]),
            "end_utc": common.iso(cluster[-1]["ts_ms"]),
            "hours": (cluster[-1]["ts_ms"] - cluster[0]["ts_ms"]) / 3_600_000.0,
            "rows": len(cluster),
            "contracts": len(tickers),
            "pooled_row_brier_calibrated": calibrated,
            "pooled_row_brier_market_mid": market,
            "pooled_row_delta": calibrated - market,
        })

    by_day = collections.Counter(common.iso(row["ts_ms"])[:10] for row in rows)
    largest = max(episodes, key=lambda e: e["rows"])
    total_rows = len(rows)
    return {
        "episode_count": len(episodes),
        "episodes": episodes,
        "rows_by_utc_day": dict(sorted(by_day.items())),
        "largest_episode_share_of_rows": largest["rows"] / total_rows,
        "largest_episode": largest["episode"],
        "note": ("contracts inside one episode share a single BTC price path, "
                 "so the effective sample size for 'the calibrator wins in fast "
                 "markets' is the EPISODE count, not the contract count"),
    }


# ── the rejected alternative, reported rather than asserted ──────────────────
def prefix_derived_threshold_diagnostic(contracts, fraction=PREFIX_FRACTION):
    """Would an in-time prefix have produced a usable 'high volatility' cut?

    The stricter-sounding freeze is to re-derive the upper tercile from a
    training prefix of contracts and evaluate only on the remainder. It is
    reported here with the number that decides whether it is meaningful: the
    SHARE OF LATER ROWS the prefix boundary admits. Per-minute BTC volatility
    is strongly non-stationary across this window, so a boundary learned on a
    calm prefix is not a tercile of the evaluation period — if it sweeps in most
    later rows it has stopped isolating a high-volatility regime, and scoring it
    would be measuring almost everything while calling it the exception.
    """
    cut = int(len(contracts) * fraction)
    if cut < MIN_SUBSET_CONTRACTS or cut >= len(contracts):
        return {"available": False,
                "reason": f"prefix of {cut} contracts is too small to derive a tercile"}
    prefix, remainder = contracts[:cut], contracts[cut:]
    prefix_vols = sorted(row["vol_per_min_bps"] for c in prefix for row in c["rows"]
                         if row.get("vol_per_min_bps") is not None)
    if len(prefix_vols) < 3:
        return {"available": False, "reason": "prefix carries too few volatility rows"}
    threshold = prefix_vols[int(len(prefix_vols) * 2 / 3)]
    later_rows = [row for c in remainder for row in c["rows"]
                  if row.get("vol_per_min_bps") is not None]
    admitted = [row for row in later_rows if row["vol_per_min_bps"] > threshold]
    selected, _ = filter_by_vol(remainder, threshold, "high")
    share = len(admitted) / len(later_rows) if later_rows else None
    return {
        "available": True,
        "prefix_contracts": len(prefix),
        "prefix_close_span_utc": [common.iso(prefix[0]["close_ms"]),
                                  common.iso(prefix[-1]["close_ms"])],
        "derived_threshold_bps": threshold,
        "frozen_threshold_bps": FROZEN_VOL_THRESHOLD_BPS,
        "later_rows": len(later_rows),
        "later_rows_admitted": len(admitted),
        "share_of_later_rows_admitted": share,
        "expected_share_if_a_true_tercile": 1.0 / 3.0,
        "degenerate": share is not None and share > 0.5,
        "scored_if_used": score_cell(selected),
        "why_rejected": ("a prefix-derived boundary admits far more than a "
                         "tercile of later rows because per-minute volatility "
                         "rose across the window; it would score 'high vol' on "
                         "most of the sample, which is not the exception #169 "
                         "found"),
    }


# ── sensitivity ──────────────────────────────────────────────────────────────
def sensitivity(contracts, thresholds=SENSITIVITY_THRESHOLDS_BPS):
    """The same walk-forward at every cut point, so one lucky cut cannot hide."""
    out = []
    for threshold in thresholds:
        subset, _ = filter_by_vol(contracts, threshold, "high")
        cell = score_cell(subset)
        result = walk_forward(subset)
        out.append({
            "threshold_bps": threshold,
            "is_frozen_threshold": threshold == FROZEN_VOL_THRESHOLD_BPS,
            "contracts": cell["contracts"] if cell else 0,
            "rows": cell["rows"] if cell else 0,
            "pooled_row_brier_calibrated": cell["pooled_row_brier_calibrated"] if cell else None,
            "pooled_row_brier_market_mid": cell["pooled_row_brier_market_mid"] if cell else None,
            "pooled_row_delta": cell["pooled_row_delta"] if cell else None,
            "contract_mean_delta": cell["contract_mean_delta"] if cell else None,
            "paired_t": cell["paired_by_contract"]["t"] if cell else None,
            "folds_scored": result.get("folds_scored", 0),
            "folds_calibrator_better_pooled_row":
                result.get("folds_calibrator_better_pooled_row"),
            "walk_forward_note": result.get("note"),
        })
    return out


def verdict(high, control, sensitivity_rows, episodes=None):
    """A plain sentence, derived from the numbers rather than chosen for them."""
    if high is None or high["pooled"] is None:
        return ("NO VERDICT — the high-volatility subset is too small for a "
                "walk-forward at this evidence retention")
    pooled = high["pooled"]
    n = pooled["contracts"]
    paired = pooled["paired_by_contract"]
    row_wins = pooled["pooled_row_delta"] < 0.0
    contract_wins = pooled["contract_mean_delta"] < 0.0
    folds_won = high["folds_calibrator_better_pooled_row"]
    folds = high["folds_scored"]
    cuts_won = sum(1 for row in sensitivity_rows
                   if row["pooled_row_delta"] is not None
                   and row["pooled_row_delta"] < 0.0)
    control_sign = (control["pooled"]["pooled_row_delta"]
                    if control and control["pooled"] else None)

    if row_wins and contract_wins:
        headline = ("The exception REPRODUCES on both weightings "
                    f"(pooled-row {pooled['pooled_row_delta']:+.4f}, "
                    f"contract-mean {pooled['contract_mean_delta']:+.4f})")
    elif row_wins or contract_wins:
        headline = ("The exception is WEIGHTING-DEPENDENT — it survives on "
                    f"{'pooled rows' if row_wins else 'contract means'} and "
                    f"reverses on the other "
                    f"(pooled-row {pooled['pooled_row_delta']:+.4f}, "
                    f"contract-mean {pooled['contract_mean_delta']:+.4f})")
    else:
        headline = ("The exception DOES NOT SURVIVE — the calibrator trails "
                    "the raw mid on both weightings "
                    f"(pooled-row {pooled['pooled_row_delta']:+.4f}, "
                    f"contract-mean {pooled['contract_mean_delta']:+.4f})")
    parts = [f"{headline} at n={n} contracts"]
    if episodes and episodes.get("episode_count"):
        parts.append(f"but those contracts are only "
                     f"{episodes['episode_count']} volatility episode(s), the "
                     f"largest carrying "
                     f"{100 * episodes['largest_episode_share_of_rows']:.0f}% "
                     f"of the rows — the effective n")
    if paired["t"] is not None:
        beyond = "" if abs(paired["t"]) >= 2.0 else "NOT "
        parts.append(f"paired-by-contract t={paired['t']:.2f}, which is "
                     f"{beyond}beyond |t|=2")
    parts.append(f"{folds_won}/{folds} folds favour the calibrator")
    parts.append(f"{cuts_won}/{len(sensitivity_rows)} sensitivity cut points "
                 f"favour the calibrator")
    if control_sign is not None:
        parts.append(f"the low/mid control reads {control_sign:+.4f} "
                     f"(positive = calibrator worse, the direction #169 found)")
    return "; ".join(parts) + "."


def main():
    history = forecast_history.build()
    contracts = sorted(history["contracts"],
                       key=lambda c: (c["close_ms"], c["ticker"]))
    if not contracts:
        common.emit({"as_of_utc": common.as_of(), "question": "F3",
                     "verdict": "NO DATA — no contract could be resolved",
                     "audit": history["audit"]})
        return 1

    high, dropped_no_vol = filter_by_vol(contracts, FROZEN_VOL_THRESHOLD_BPS, "high")
    control, _ = filter_by_vol(contracts, FROZEN_VOL_THRESHOLD_BPS, "low_mid")
    recorded_high = [c for c in high if c["outcome_source"] == "recorded"]

    high_result = walk_forward(high)
    control_result = walk_forward(control)
    sensitivity_rows = sensitivity(contracts)
    episodes = episode_structure(high)

    common.emit({
        "as_of_utc": common.as_of(),
        "question": "F3 — does the high-volatility exception survive out-of-sample?",
        "command": "python3 scripts/research/f3_high_vol_walk_forward.py",
        "issue": 172,
        "audit": history["audit"],
        "method": {
            "frozen_threshold_bps": FROZEN_VOL_THRESHOLD_BPS,
            "frozen_threshold_source": FROZEN_THRESHOLD_SOURCE,
            "split_policy": ("contracts ordered by close time, fold boundaries "
                             "snapped to close-time groups; a contract is wholly "
                             "in one fold, never split by row"),
            "nothing_is_fitted": ("no parameter is estimated anywhere in this "
                                  "script; the folds establish that the "
                                  "threshold was frozen a priori and show "
                                  "per-fold sign stability, and claim nothing "
                                  "about model overfitting"),
            "rows_dropped_without_volatility": dropped_no_vol,
            "contracts_total": len(contracts),
        },
        "high_vol_walk_forward": high_result,
        "high_vol_episode_structure": episodes,
        "low_mid_control_walk_forward": control_result,
        "recorded_settlements_only": {
            "contracts": len(recorded_high),
            "scored": score_cell(recorded_high),
            "walk_forward": walk_forward(recorded_high),
        },
        "sensitivity_by_threshold": sensitivity_rows,
        "prefix_derived_threshold_diagnostic":
            prefix_derived_threshold_diagnostic(contracts),
        "verdict": verdict(high_result, control_result, sensitivity_rows,
                           episodes),
    })
    return 0


if __name__ == "__main__":
    sys.exit(main())
