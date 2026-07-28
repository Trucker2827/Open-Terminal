#!/usr/bin/env python3
"""F1's payoff (issue #179): Q1 regenerated on the retained lag-series window.

The #169 autopsy measured the Kalshi quote lag over **8.2 hours** — the most the
size-rotating ticker log could hold — and landed on a near-tie: after a 3-sigma
spot move the mid drifted +2.07c over 15 seconds while taking the quote cost
2.08c. F1 (#170) built a time-bounded retained series so that number could be
re-measured on a longer window. This script is that re-measurement.

WHAT IS REUSED VERBATIM AND WHY. `q1_quote_lag.py` is imported, not copied and
not edited: `detect_events`, `observe` and `load_quote_series` are called as
published, so the event definition, the alignment convention, the expiry band,
the 15-second staleness rule, the band-market exclusion and the fee arithmetic
are the SAME code that produced the published table. This script adds only what
#179 asks for on top of that population:

  1. a decomposition of the drift by MOVE SIZE (3s/4s/5s) and by TIME TO
     EXPIRY, which the published table pooled;
  2. CONFIDENCE INTERVALS clustered on the spot move. The published table's own
     caveat is that its t-statistics treat (event, market) pairs as independent
     when markets sharing an event are not, so "the effective n is closer to the
     event count than the sample count". A cluster-robust interval is that
     sentence, discharged: the naive standard error is reported beside it so
     the rows stay comparable to the 8.2-hour table.

PREREGISTERED BEFORE THE NUMBERS WERE READ (#179's second acceptance criterion
turns on it, so it is fixed here in code rather than chosen after the fact):

  * a "MOVE" is a distinct spot EVENT that contributed at least one kept
    (event, market) sample to the slice — not a sample, and not an event that
    was detected but filtered out. The two differ by an order of magnitude in
    this data, and which one is counted decides whether the follow-up gets
    filed, so it is named before it is counted.
  * the maker-side capture hypothesis is TESTABLE in a slice only when that
    slice holds >= MIN_MOVES_FOR_MAKER_TEST moves at the primary horizon, and
    it is POSITIVE when the mean drift net of the MAKER cost (the Kalshi fee
    alone — a resting quote does not cross the spread) is above zero. The
    interval is reported either way; the filing trigger #179 states is the
    point estimate, and whether the interval also excludes zero is reported
    beside it rather than substituted for it.
  * one detection pass at 3 sigma, PARTITIONED by |z|. That is not the same as
    three separate `detect_events` runs: the 5-minute cooldown is applied once,
    at k=3, so a 4.2-sigma move suppressed by a 3.1-sigma event five minutes
    earlier is absent here but would survive `detect_events(..., 4.0)`. The
    partition keeps the buckets disjoint and the cooldown honest across them;
    the per-threshold counts a separate `detect_events` run would give are
    reported alongside so the difference reads as a stated choice.

THE WINDOW IS REPORTED TWICE. Span hours (first quote to last) can hide a hole:
the analysed book is the retained series and the live log concatenated, and if
the recorder had stopped for longer than the live log's rotation the union would
still LOOK continuous. So the pooled two-sided quote stream is scanned for gaps
longer than the event cooldown, the gaps are listed, and covered hours are
reported beside span hours.

Read-only: opens evidence, writes nothing, never calls `spot_calibrator`.

  python3 scripts/research/f1_quote_lag_strata.py
"""
import collections
import math
import sys

import kalshi_edge_common as common
import forecast_history
import q1_quote_lag as q1

# ── preregistered constants ──────────────────────────────────────────────────
DETECTION_SIGMA = 3.0             # one pass; buckets partition its output
SIGMA_BUCKETS = ((3.0, 4.0), (4.0, 5.0), (5.0, None))
# The published table carries a 2-sigma row as well as a 3-sigma one. It is
# regenerated here — with the same clustered interval — purely so the two rows
# stay comparable: a verdict that quotes the published 2-sigma t-statistic while
# discounting the 3-sigma one on clustering grounds would be reading the same
# data by two different rules. It is NOT one of #179's requested buckets, and it
# comes from its OWN detection pass, exactly as the published table's did.
REFERENCE_SIGMA = 2.0
EXPIRY_BUCKETS_S = ((120, 600), (600, 1800), (1800, 3600))
PRIMARY_HORIZON_S = 15            # the horizon the published verdict rests on
REPORTED_HORIZONS_S = tuple(h for h in q1.HORIZONS_S if h < 120)
MIN_MOVES_FOR_MAKER_TEST = 30
COVERAGE_GAP_MS = q1.EVENT_COOLDOWN_MS   # a hole this long could hide an event

# Two-sided 95% Student-t critical values. Exact for df 1..30 — the interval
# below is clustered on the spot move, and there are tens of moves, not
# thousands, so using 1.96 there would quietly narrow every interval this script
# exists to widen. Above 30 the largest tabulated df <= the actual one is used,
# which errs wide, and the asymptote is 1.96.
_T_CRITICAL_95 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447,
                  7: 2.365, 8: 2.306, 9: 2.262, 10: 2.228, 11: 2.201, 12: 2.179,
                  13: 2.160, 14: 2.145, 15: 2.131, 16: 2.120, 17: 2.110,
                  18: 2.101, 19: 2.093, 20: 2.086, 21: 2.080, 22: 2.074,
                  23: 2.069, 24: 2.064, 25: 2.060, 26: 2.056, 27: 2.052,
                  28: 2.048, 29: 2.045, 30: 2.042, 40: 2.021, 50: 2.009,
                  60: 2.000, 80: 1.990, 100: 1.984, 120: 1.980}


def t_critical_95(df):
    """Two-sided 95% t critical value; None below 1 degree of freedom."""
    if df is None or df < 1:
        return None
    if df in _T_CRITICAL_95:
        return _T_CRITICAL_95[df]
    tabulated = [k for k in _T_CRITICAL_95 if k <= df]
    return _T_CRITICAL_95[max(tabulated)] if tabulated else 1.960


def cluster_summary(values, clusters):
    """Mean with a 95% interval clustered on the spot move.

    The estimator is the standard one-way cluster-robust sandwich for a mean:
    residuals are summed WITHIN each cluster before being squared, so samples
    that move together stop counting as independent evidence. The finite-sample
    factor is G / (G - 1) with G the number of clusters, and the interval uses
    t on G - 1 degrees of freedom.

    A single cluster yields NO interval — one spot move cannot bound its own
    sampling error, and returning a naive interval there (which would shrink
    with the number of markets quoting that one move) is exactly the error the
    published table warned about. Missing reads as missing.
    """
    n = len(values)
    if n == 0:
        return None
    mean = sum(values) / n
    if n > 1:
        variance = sum((v - mean) ** 2 for v in values) / (n - 1)
        naive_stderr = math.sqrt(variance / n)
    else:
        naive_stderr = None
    residual_sums = collections.defaultdict(float)
    for value, cluster in zip(values, clusters):
        residual_sums[cluster] += value - mean
    groups = len(residual_sums)
    out = {"n": n, "moves": groups, "mean": mean,
           "naive_stderr": naive_stderr,
           "naive_t_stat": (mean / naive_stderr
                            if naive_stderr not in (None, 0.0) else None)}
    if groups < 2:
        out.update({"cluster_stderr": None, "cluster_t_stat": None,
                    "ci95": None, "df": None,
                    "interval_note": ("one spot move in this slice — a "
                                      "cluster-robust interval needs at least "
                                      "two, so none is reported")})
        return out
    meat = sum(total * total for total in residual_sums.values())
    cluster_stderr = math.sqrt(meat * (groups / (groups - 1.0)) / (n * n))
    df = groups - 1
    critical = t_critical_95(df)
    out.update({
        "cluster_stderr": cluster_stderr,
        "cluster_t_stat": (mean / cluster_stderr if cluster_stderr else None),
        "df": df,
        "ci95": ([mean - critical * cluster_stderr,
                  mean + critical * cluster_stderr]
                 if cluster_stderr is not None else None),
        "interval_note": None,
    })
    return out


def sigma_bucket(z):
    """Which |z| bucket a move belongs to; None when below the lowest edge."""
    magnitude = abs(z)
    for low, high in SIGMA_BUCKETS:
        if magnitude >= low and (high is None or magnitude < high):
            return sigma_bucket_label(low, high)
    return None


def sigma_bucket_label(low, high):
    return ("[%.0fσ, %.0fσ)" % (low, high) if high is not None
            else ">= %.0fσ" % low)


def expiry_bucket(seconds_to_close):
    for low, high in EXPIRY_BUCKETS_S:
        if low <= seconds_to_close < high:
            return expiry_bucket_label(low, high)
    # q1's own filter admits [120, 3600]; a sample landing exactly on the upper
    # edge belongs to the last bucket rather than to nothing.
    if seconds_to_close == EXPIRY_BUCKETS_S[-1][1]:
        return expiry_bucket_label(*EXPIRY_BUCKETS_S[-1])
    return None


def expiry_bucket_label(low, high):
    return "%d–%d min to close" % (low // 60, high // 60)


def stratum(samples, label):
    """One slice: drift, maker net and taking net per horizon, with intervals.

    Every cell carries BOTH costs, because they answer different questions and
    the published report's verdict turns on the difference: a taker pays the
    half-spread and the fee, a resting maker pays the fee and accepts non-fill
    and adverse selection instead. Neither is netted silently — the gross drift
    is reported first.
    """
    out = {"label": label, "samples": len(samples),
           "moves": len({s["event_ts_utc"] for s in samples}),
           "markets": len({s["ticker"] for s in samples}),
           "horizons": []}
    for horizon in q1.HORIZONS_S:
        contributing = [s for s in samples if horizon in s["aligned_mid_drift"]]
        if not contributing:
            continue
        clusters = [s["event_ts_utc"] for s in contributing]
        drift = [s["aligned_mid_drift"][horizon] for s in contributing]
        maker_net = [s["aligned_mid_drift"][horizon] - s["fee"]
                     for s in contributing]
        taking_net = [s["aligned_mid_drift"][horizon] - s["cost_to_take"]
                      for s in contributing]
        row = {
            "seconds": horizon,
            "n": len(contributing),
            "moves": len(set(clusters)),
            "markets": len({s["ticker"] for s in contributing}),
            "mean_half_spread": (sum(s["half_spread"] for s in contributing)
                                 / len(contributing)),
            "mean_fee": sum(s["fee"] for s in contributing) / len(contributing),
            "share_positive_drift": (sum(1 for d in drift if d > 0)
                                     / len(drift)),
            "aligned_mid_drift": cluster_summary(drift, clusters),
            "net_of_maker_cost": cluster_summary(maker_net, clusters),
            "net_of_taking_cost": cluster_summary(taking_net, clusters),
            "note": ("horizons >= 120s mix continued spot drift into the "
                     "measurement and are momentum, not lag"
                     if horizon >= 120 else None),
        }
        out["horizons"].append(row)
    return out


def maker_verdict(strata):
    """#179's second criterion, evaluated against the preregistered rule."""
    considered = []
    for slice_name, rows in strata:
        for row in rows:
            primary = next((h for h in row["horizons"]
                            if h["seconds"] == PRIMARY_HORIZON_S), None)
            if primary is None:
                continue
            net = primary["net_of_maker_cost"]
            testable = primary["moves"] >= MIN_MOVES_FOR_MAKER_TEST
            ci = net.get("ci95")
            considered.append({
                "family": slice_name,
                "slice": row["label"],
                "horizon_s": PRIMARY_HORIZON_S,
                "moves": primary["moves"],
                "samples": primary["n"],
                "testable": testable,
                "mean_net_of_maker_cost": net["mean"],
                "ci95": ci,
                "point_estimate_positive": net["mean"] > 0.0,
                "interval_excludes_zero": bool(ci) and ci[0] > 0.0,
            })
    testable = [row for row in considered if row["testable"]]
    triggered = [row for row in testable if row["point_estimate_positive"]]
    if not testable:
        verdict = ("NOT TESTABLE — no slice reaches %d moves at the %ds "
                   "horizon; the largest holds %d" %
                   (MIN_MOVES_FOR_MAKER_TEST, PRIMARY_HORIZON_S,
                    max((row["moves"] for row in considered), default=0)))
    elif triggered:
        verdict = ("TESTABLE AND POSITIVE in %d slice(s) — #179 requires the "
                   "strategy follow-up issue to be filed citing these numbers"
                   % len(triggered))
    else:
        verdict = ("TESTABLE AND NEGATIVE — %d slice(s) reach %d moves and "
                   "none has positive mean drift net of the maker cost"
                   % (len(testable), MIN_MOVES_FOR_MAKER_TEST))
    return {
        "rule": ("a slice is testable at >= %d MOVES (distinct spot events "
                 "contributing kept samples) at the %ds horizon; positive when "
                 "mean aligned drift minus the Kalshi fee exceeds zero. Both "
                 "halves fixed in code before the numbers were read."
                 % (MIN_MOVES_FOR_MAKER_TEST, PRIMARY_HORIZON_S)),
        "maker_cost": ("the Kalshi fee alone. A resting quote does not cross "
                       "the spread; it pays the fee and accepts non-fill and "
                       "adverse selection, neither of which this statistic "
                       "measures — so a positive result here would be a "
                       "hypothesis to test, never a strategy"),
        "verdict": verdict,
        "slices": considered,
    }


def coverage(quotes, start_ms, end_ms, gap_ms=COVERAGE_GAP_MS):
    """Span hours versus hours actually covered by two-sided quotes.

    The analysed book is the retained series concatenated with the live log. A
    span computed from first and last quote would read as continuous across a
    hole where the recorder was down and the live log had already rotated, so
    the pooled quote stream is scanned and every hole longer than the event
    cooldown is listed rather than averaged away.
    """
    instants = sorted({row[0] for rows in quotes.values() for row in rows
                       if start_ms <= row[0] <= end_ms})
    span_hours = (end_ms - start_ms) / 3_600_000.0
    gaps = []
    for earlier, later in zip(instants, instants[1:]):
        if later - earlier > gap_ms:
            gaps.append({"from_utc": common.iso(earlier),
                         "to_utc": common.iso(later),
                         "hours": (later - earlier) / 3_600_000.0})
    lost = sum(gap["hours"] for gap in gaps)
    return {"span_hours": span_hours,
            "covered_hours": span_hours - lost,
            "gap_threshold_s": gap_ms // 1000,
            "gaps": gaps,
            "quote_instants": len(instants)}


def live_log_start_ms(name=q1.TICKERS):
    """First instant the LIVE rotations still hold, streamed, not materialised.

    `kalshi_edge_common.read_jsonl` uses exactly this instant as the boundary
    below which retained rows are used, so recomputing it here is what lets the
    report state where the retained half ends and the live half begins.
    """
    key = common.RETAINED_TS_KEY.get(name)
    earliest = None
    for record in common.iter_jsonl(name):
        ts_ms = common._row_ts_ms(record, key)
        if ts_ms is not None and (earliest is None or ts_ms < earliest):
            earliest = ts_ms
    return earliest


def main():
    brti, brti_inventory = common.load_brti()
    quotes, quote_inventory, quote_dropped = q1.load_quote_series()
    if not quotes or not len(brti):
        common.emit({"as_of_utc": common.as_of(), "question": "F1/#179",
                     "verdict": "NO DATA"})
        return 1
    book = q1.QuoteBook(quotes)
    quote_span = book.span()
    brti_span = brti.span_ms
    start = max(quote_span[0], brti_span[0])
    end = min(quote_span[1], brti_span[1])
    window = coverage(quotes, start, end)
    live_start = live_log_start_ms()

    volatility = forecast_history.VolatilityGrid(brti)
    events = q1.detect_events(brti, volatility, start, end, DETECTION_SIGMA)
    samples, considered = q1.observe(events, book, brti, DETECTION_SIGMA)
    contested = [s for s in samples if s["contested"]]

    # What separate detection passes would have counted, so the partition's
    # cooldown choice is visible rather than implied.
    separate_passes = []
    for low, _ in SIGMA_BUCKETS:
        detected = q1.detect_events(brti, volatility, start, end, low)
        separate_passes.append({"threshold_sigma": low,
                                "events_detected_own_pass": len(detected)})

    reference_events = q1.detect_events(brti, volatility, start, end,
                                        REFERENCE_SIGMA)
    reference_samples, reference_considered = q1.observe(
        reference_events, book, brti, REFERENCE_SIGMA)
    reference_contested = [s for s in reference_samples if s["contested"]]

    populations = [("contested markets (mid in [%.2f, %.2f]) — the published "
                    "table's population" % q1.CONTESTED_BAND, contested),
                   ("all markets", samples)]
    result = {
        "as_of_utc": common.as_of(),
        "question": ("F1/#179 — Q1's quote-lag table regenerated on the "
                     "retained lag-series window, decomposed by move size and "
                     "time to expiry, with move-clustered intervals"),
        "command": "python3 scripts/research/f1_quote_lag_strata.py",
        "reuses": ("q1_quote_lag.detect_events / observe / load_quote_series / "
                   "QuoteBook, imported unmodified"),
        "published_comparison": {
            "report": "docs/research/2026-07-27-kalshi-edge-autopsy.md §Q1",
            "published_paired_window_hours": 8.2,
            "published_3sigma_15s_drift": 0.0207,
            "published_3sigma_15s_cost_to_take": 0.0208,
            "published_3sigma_events": 8,
        },
        "data": {
            "brti_files": brti_inventory, "brti_samples": len(brti),
            "brti_span_utc": [common.iso(t) for t in brti_span],
            "quote_files": quote_inventory,
            "quote_markets": len(quotes),
            "quote_rows_two_sided": sum(len(v) for v in quotes.values()),
            "quote_rows_dropped": quote_dropped,
            "quote_span_utc": [common.iso(t) for t in quote_span],
            "paired_window_utc": [common.iso(start), common.iso(end)],
            "paired_window_hours": window["span_hours"],
            "covered_hours": window["covered_hours"],
            "coverage_gaps": window["gaps"],
            "coverage_gap_threshold_s": window["gap_threshold_s"],
            "live_log_first_row_utc": common.iso(live_start),
            "retained_series_supplies": (
                "every analysed quote before the live log's first row; the "
                "overlap is resolved in favour of the live rows by "
                "kalshi_edge_common.read_jsonl, and coverage_gaps is what "
                "proves the two halves meet without a hole"),
        },
        "method": {
            "detection": ("one q1.detect_events pass at %.0f sigma, partitioned "
                          "by |z| into %s. The 5-minute cooldown is applied "
                          "ONCE at %.0f sigma, so these counts are not what "
                          "separate passes give — see "
                          "events_detected_own_pass."
                          % (DETECTION_SIGMA,
                             [sigma_bucket_label(lo, hi)
                              for lo, hi in SIGMA_BUCKETS], DETECTION_SIGMA)),
            "expiry_buckets": [expiry_bucket_label(lo, hi)
                               for lo, hi in EXPIRY_BUCKETS_S],
            "interval": ("95% t interval on the mean, clustered on the spot "
                         "move (G - 1 degrees of freedom, G/(G-1) correction). "
                         "The naive independent-sample stderr is reported "
                         "beside it for comparability with the published "
                         "table, which had only the naive one"),
            "statistic": ("q1's, unmodified: sign(spot move) * (Kalshi YES mid "
                          "at t+h - mid at t)"),
        },
        "events": {
            "threshold_sigma": DETECTION_SIGMA,
            "events_detected": len(events),
            "events_per_hour": (len(events) / window["span_hours"]
                                if window["span_hours"] else None),
            "events_by_bucket": dict(collections.Counter(
                sigma_bucket(event["z"]) for event in events)),
            "separate_pass_counts": separate_passes,
            "pair_filter_counts": considered,
        },
        "by_population": [],
    }

    maker_input = []
    for label, population in populations:
        by_move = [stratum([s for s in population
                            if sigma_bucket(s["z"]) == sigma_bucket_label(lo, hi)],
                           sigma_bucket_label(lo, hi))
                   for lo, hi in SIGMA_BUCKETS]
        by_expiry = [stratum([s for s in population
                              if expiry_bucket(s["seconds_to_close"])
                              == expiry_bucket_label(lo, hi)],
                             expiry_bucket_label(lo, hi))
                     for lo, hi in EXPIRY_BUCKETS_S]
        crossed = []
        for lo, hi in SIGMA_BUCKETS:
            for elo, ehi in EXPIRY_BUCKETS_S:
                cell = [s for s in population
                        if sigma_bucket(s["z"]) == sigma_bucket_label(lo, hi)
                        and expiry_bucket(s["seconds_to_close"])
                        == expiry_bucket_label(elo, ehi)]
                crossed.append(stratum(cell, "%s × %s"
                                       % (sigma_bucket_label(lo, hi),
                                          expiry_bucket_label(elo, ehi))))
        result["by_population"].append({
            "population": label,
            "samples": len(population),
            "moves": len({s["event_ts_utc"] for s in population}),
            "pooled": stratum(population, "pooled (all moves >= %.0f sigma)"
                              % DETECTION_SIGMA),
            "by_move_size": by_move,
            "by_time_to_expiry": by_expiry,
            "by_move_size_x_time_to_expiry": crossed,
        })
        if population is contested:
            maker_input = [("pooled", [result["by_population"][-1]["pooled"]]),
                           ("move size", by_move),
                           ("time to expiry", by_expiry),
                           ("move size x expiry", crossed)]

    result["reference_row"] = {
        "threshold_sigma": REFERENCE_SIGMA,
        "why": ("the published table's other row, regenerated under the same "
                "clustered interval so the two are read by one rule. Its own "
                "detection pass, its own cooldown — not a partition of the "
                "%.0f-sigma pass above" % DETECTION_SIGMA),
        "events_detected": len(reference_events),
        "pair_filter_counts": reference_considered,
        "contested": stratum(reference_contested,
                             "%.0fσ / contested (published table's row)"
                             % REFERENCE_SIGMA),
    }
    # The reference row is evaluated for maker capture ALONGSIDE #179's own
    # buckets. It is not one of the requested slices, but it is a slice of this
    # analysis, and dropping the one slice that reaches the move threshold —
    # because it was not on the request list — would be selection, not scope
    # discipline. Including it can only make the follow-up more likely to be
    # filed, never less.
    maker_input.append(("reference (%.0f sigma)" % REFERENCE_SIGMA,
                        [result["reference_row"]["contested"]]))
    result["maker_side_capture"] = maker_verdict(maker_input)
    result["reported_horizons_s"] = list(REPORTED_HORIZONS_S)
    common.emit(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
