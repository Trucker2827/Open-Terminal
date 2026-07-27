#!/usr/bin/env python3
"""Q1 of the Kalshi edge autopsy (issue #169): does the Kalshi book lag spot?

THE MEASUREMENT IS MODEL-FREE FIRST. The tempting way to size a quote-lag edge
is to price a Gaussian implied probability off fresh spot and call the distance
to the Kalshi mid an edge — but then the "edge" is mostly a statement about the
Gaussian. So the primary statistic here needs no model at all:

    after a significant BTC move that has ALREADY HAPPENED by t, does the
    Kalshi mid keep moving in that direction over the next few seconds?

If it does, the book was stale at t, the staleness is measurable in cents, and
the cents are directly comparable to the half-spread plus fee you would pay to
take it. If it does not, there is no lag to harvest regardless of what any
probability model says. A model-priced gap is reported too, but SECOND, and
labelled as model-dependent.

Sign convention: KXBTCD/KXBTC threshold contracts settle YES above the strike,
so an up-move in spot should push the YES mid up. Every mid change is therefore
multiplied by the sign of the triggering spot move, and a positive mean means
"the book moved the way the already-known spot move implied" — i.e. lag.

Events are detected on the CF BRTI index, the series Kalshi settles against, in
sigma units of the terminal's own trailing realized volatility (computed from
BRTI itself — no evidence file carries a ready-made vol number for this window,
so the estimator is stated rather than borrowed). Events are separated by a
cooldown so two triggers cannot describe the same move.

Read-only. Prints JSON to stdout; writes nothing.

  python3 scripts/research/q1_quote_lag.py
"""
import bisect
import collections
import math
import sys

import kalshi_edge_common as common
import forecast_history

TICKERS = "kalshi-tickers.jsonl"
EVENT_WINDOW_MS = 60_000          # a "move" is measured over 60 seconds
EVENT_GRID_MS = 10_000            # candidate triggers evaluated every 10s
EVENT_COOLDOWN_MS = 300_000       # events at least 5 minutes apart
SIGMA_THRESHOLDS = (2.0, 3.0)
HORIZONS_S = (5, 15, 30, 60, 120, 300)
QUOTE_STALENESS_MS = 15_000       # a quote older than this is missing, not stale
MIN_SECONDS_TO_CLOSE = 120
MAX_SECONDS_TO_CLOSE = 3600
CONTESTED_BAND = (0.05, 0.95)


def load_quote_series():
    """market_ticker -> sorted [(ts_ms, bid, ask, mid)] two-sided quotes only.

    A one-sided book has no midpoint, and inventing one (from `1 - no_bid`, or
    by treating a missing ask as 1.0) would fabricate exactly the quantity this
    question measures. Such rows are counted and dropped.
    """
    records, inventory = common.read_jsonl(TICKERS)
    series = collections.defaultdict(list)
    dropped = collections.Counter()
    for record in records:
        ticker = record.get("market_ticker")
        ts_ms = record.get("ts_ms")
        if not ticker or ts_ms is None:
            dropped["missing_identity"] += 1
            continue
        try:
            ts_ms = int(ts_ms)
            bid = float(record.get("yes_bid_dollars"))
            ask = float(record.get("yes_ask_dollars"))
        except (TypeError, ValueError):
            dropped["unparseable_quote"] += 1
            continue
        if not 0.0 < bid < ask < 1.0:
            dropped["not_two_sided"] += 1
            continue
        series[ticker].append((ts_ms, bid, ask, (bid + ask) / 2.0))
    for ticker in series:
        series[ticker].sort()
    return dict(series), inventory, dict(dropped)


class QuoteBook:
    def __init__(self, series):
        self.series = series
        self.times = {t: [row[0] for row in rows] for t, rows in series.items()}

    def at(self, ticker, ts_ms, max_age_ms=QUOTE_STALENESS_MS):
        """Last quote at or before `ts_ms`, or None if there isn't a fresh one.

        Deliberately backward-looking: the question is what a taker could have
        seen at that instant, so a quote posted afterwards must not leak in.
        """
        times = self.times.get(ticker)
        if not times:
            return None
        idx = bisect.bisect_right(times, ts_ms) - 1
        if idx < 0:
            return None
        row = self.series[ticker][idx]
        if ts_ms - row[0] > max_age_ms:
            return None
        return row

    def span(self):
        lows = [times[0] for times in self.times.values() if times]
        highs = [times[-1] for times in self.times.values() if times]
        return (min(lows) if lows else None, max(highs) if highs else None)


def detect_events(brti, volatility, start_ms, end_ms, threshold_sigma):
    """Significant BRTI moves, in sigmas of trailing realized volatility."""
    events = []
    last_event_ms = None
    t = start_ms + EVENT_WINDOW_MS
    while t <= end_ms:
        before = brti.nearest(t - EVENT_WINDOW_MS, max_gap_ms=5000)
        after = brti.nearest(t, max_gap_ms=5000)
        sigma_bps = volatility.at(t)
        if before and after and sigma_bps and sigma_bps > 0.0 and before[1] > 0.0:
            move_bps = math.log(after[1] / before[1]) * 10000.0
            z = move_bps / sigma_bps          # sigma is already per-minute
            if abs(z) >= threshold_sigma:
                if last_event_ms is None or t - last_event_ms >= EVENT_COOLDOWN_MS:
                    events.append({"ts_ms": t, "z": z, "move_bps": move_bps,
                                   "sigma_per_min_bps": sigma_bps,
                                   "spot_before": before[1], "spot_after": after[1]})
                    last_event_ms = t
        t += EVENT_GRID_MS
    return events


def observe(events, book, brti, threshold_sigma):
    """Per (event, market) mid drift after the move, aligned to its direction."""
    samples = []
    considered = collections.Counter()
    for event in events:
        t = event["ts_ms"]
        direction = 1.0 if event["move_bps"] > 0 else -1.0
        for ticker in book.series:
            parsed = common.parse_ticker(ticker)
            if parsed is None:
                considered["unparseable_ticker"] += 1
                continue
            if parsed["strike"] is None:
                # `KXBTC-...-B65050` is a BAND market, not a threshold one:
                # exactly one $100-wide band settles YES per event (verified
                # against the settlement feed — the YES band is the one
                # containing `expiration_value`). Excluded deliberately, and
                # counted separately rather than lumped under a generic
                # "no strike": this statistic multiplies a mid change by the
                # SIGN of the spot move, which presumes YES is monotone in
                # spot. For a band it is not — a large up-move can push spot
                # straight through and OUT of the band, lowering its YES while
                # the move was upward. Applying the statistic here would not be
                # a smaller measurement, it would be a wrong one.
                considered["excluded_band_market"] += 1
                continue
            seconds_to_close = (parsed["close_ms"] - t) / 1000.0
            if not MIN_SECONDS_TO_CLOSE <= seconds_to_close <= MAX_SECONDS_TO_CLOSE:
                considered["out_of_expiry_band"] += 1
                continue
            base = book.at(ticker, t)
            if base is None:
                considered["no_fresh_quote_at_event"] += 1
                continue
            _, bid, ask, mid = base
            half_spread = (ask - bid) / 2.0
            fee = common.fee_per_contract(ask)
            drift = {}
            for horizon in HORIZONS_S:
                later = book.at(ticker, t + horizon * 1000)
                if later is None or later[0] <= base[0]:
                    continue
                drift[horizon] = direction * (later[3] - mid)
            if not drift:
                considered["no_quote_after_event"] += 1
                continue
            considered["kept"] += 1
            samples.append({
                "threshold_sigma": threshold_sigma,
                "event_ts_utc": common.iso(t),
                "z": event["z"], "move_bps": event["move_bps"],
                "ticker": ticker,
                "seconds_to_close": seconds_to_close,
                "mid_at_event": mid, "half_spread": half_spread, "fee": fee,
                "cost_to_take": half_spread + fee,
                "contested": CONTESTED_BAND[0] <= mid <= CONTESTED_BAND[1],
                "aligned_mid_drift": drift,
                "spot_after": event["spot_after"],
                "strike": parsed["strike"],
                "sigma_per_min_bps": event["sigma_per_min_bps"],
            })
    return samples, dict(considered)


def summarize(samples, label):
    """Mean aligned drift per horizon, against the cost of taking the quote."""
    if not samples:
        return {"label": label, "samples": 0,
                "verdict": "no observations in this stratum"}
    out = {"label": label, "samples": len(samples),
           "events": len({s["event_ts_utc"] for s in samples}),
           "markets": len({s["ticker"] for s in samples}),
           "mean_cost_to_take": sum(s["cost_to_take"] for s in samples) / len(samples),
           "horizons": []}
    for horizon in HORIZONS_S:
        drifts = [s["aligned_mid_drift"][horizon] for s in samples
                  if horizon in s["aligned_mid_drift"]]
        if not drifts:
            continue
        mean = sum(drifts) / len(drifts)
        variance = (sum((d - mean) ** 2 for d in drifts) / (len(drifts) - 1)
                    if len(drifts) > 1 else 0.0)
        stderr = math.sqrt(variance / len(drifts)) if drifts else 0.0
        contributing = [s for s in samples if horizon in s["aligned_mid_drift"]]
        mean_cost = sum(s["cost_to_take"] for s in contributing) / len(contributing)
        mean_half_spread = (sum(s["half_spread"] for s in contributing)
                            / len(contributing))
        mean_fee = sum(s["fee"] for s in contributing) / len(contributing)
        ordered = sorted(drifts)
        out["horizons"].append({
            "seconds": horizon, "n": len(drifts),
            "mean_aligned_mid_drift": mean,
            "stderr": stderr,
            "t_stat": mean / stderr if stderr > 0 else None,
            "median_aligned_mid_drift": ordered[len(ordered) // 2],
            "share_positive": sum(1 for d in drifts if d > 0) / len(drifts),
            "mean_half_spread": mean_half_spread,
            "mean_fee": mean_fee,
            "mean_cost_to_take": mean_cost,
            "net_of_cost_taking": mean - mean_cost,
            # A resting quote does not pay the half-spread; it pays the fee and
            # accepts non-fill and adverse selection instead. Reported so the
            # report can say WHICH cost the lag fails to clear, not just that
            # it fails — the two imply different follow-ups.
            "net_of_fee_only": mean - mean_fee,
            "profitable_after_taking_cost": mean - mean_cost > 0,
            "profitable_after_fee_only": mean - mean_fee > 0,
            "note": ("horizons >= 120s mix continued spot drift into the "
                     "measurement and are momentum, not lag"
                     if horizon >= 120 else None),
        })
    return out


def model_priced_gap(samples):
    """SECONDARY, MODEL-DEPENDENT: the Gaussian-implied gap at the event.

    Reported for scale only. Every number here inherits the assumption that the
    index is a driftless Gaussian random walk with the trailing realized
    volatility, which near expiry it demonstrably is not.
    """
    gaps = []
    for sample in samples:
        minutes_left = sample["seconds_to_close"] / 60.0
        implied = common.implied_probability(sample["spot_after"], sample["strike"],
                                             sample["sigma_per_min_bps"], minutes_left)
        if implied is None:
            continue
        gaps.append(abs(implied - sample["mid_at_event"]) - sample["cost_to_take"])
    if not gaps:
        return {"samples": 0}
    gaps.sort()
    return {"samples": len(gaps),
            "mean_net_gap": sum(gaps) / len(gaps),
            "median_net_gap": gaps[len(gaps) // 2],
            "share_positive": sum(1 for g in gaps if g > 0) / len(gaps),
            "caveat": ("Gaussian threshold model, not a recorded quantity; the "
                       "model-free aligned-drift table is the primary result")}


def main():
    brti, brti_inventory = common.load_brti()
    quotes, quote_inventory, quote_dropped = load_quote_series()
    if not quotes or not len(brti):
        common.emit({"as_of_utc": common.as_of(), "question": "Q1",
                     "verdict": "NO DATA"})
        return 1
    book = QuoteBook(quotes)
    quote_span = book.span()
    families = collections.Counter(t.split("-")[0] for t in quotes)
    brti_span = brti.span_ms
    start = max(quote_span[0], brti_span[0])
    end = min(quote_span[1], brti_span[1])
    paired_hours = (end - start) / 3_600_000.0

    volatility = forecast_history.VolatilityGrid(brti)
    result = {
        "as_of_utc": common.as_of(),
        "question": "Q1 — Kalshi quote lag versus spot",
        "command": "python3 scripts/research/q1_quote_lag.py",
        "data": {
            "brti_files": brti_inventory, "brti_samples": len(brti),
            "brti_span_utc": [common.iso(t) for t in brti_span],
            "quote_files": quote_inventory,
            "quote_markets": len(quotes),
            "quote_markets_by_family": dict(families),
            "quote_market_families": {
                "KXBTCD": "hourly THRESHOLD contracts (-T strike) — analysed",
                "KXBTC": ("hourly BAND contracts (-B level, $100 wide) — "
                          "EXCLUDED, see excluded_band_market below"),
            },
            "quote_rows_two_sided": sum(len(v) for v in quotes.values()),
            "quote_rows_dropped": quote_dropped,
            "quote_span_utc": [common.iso(t) for t in quote_span],
            "paired_window_utc": [common.iso(start), common.iso(end)],
            "paired_window_hours": paired_hours,
            "limiting_factor": ("the Kalshi ticker log, not the spot feed: it "
                                "rotates at ~67MB into a single .1 sibling that "
                                "the next rotation overwrites, so sub-second "
                                "book history retains only the most recent hours"),
        },
        "method": {
            "event": (f"|log return over {EVENT_WINDOW_MS // 1000}s| >= k sigma, "
                      f"k in {list(SIGMA_THRESHOLDS)}, sigma = trailing "
                      f"{forecast_history.VOL_LOOKBACK_MS // 60000}-minute realized "
                      "per-minute volatility of BRTI itself"),
            "cooldown_s": EVENT_COOLDOWN_MS // 1000,
            "markets": (f"KXBTCD -T THRESHOLD contracts closing in "
                        f"[{MIN_SECONDS_TO_CLOSE}, {MAX_SECONDS_TO_CLOSE}]s. "
                        "KXBTC -B band markets are excluded: the statistic "
                        "presumes YES is monotone in spot, which holds for a "
                        "threshold and not for a band"),
            "statistic": ("sign(spot move) * (Kalshi YES mid at t+h - mid at t); "
                          "positive means the book moved after the fact"),
            "cost": "half-spread at the event + Kalshi fee ceil(0.07*P*(1-P)*100)c",
        },
        "by_threshold": [],
    }

    for threshold in SIGMA_THRESHOLDS:
        events = detect_events(brti, volatility, start, end, threshold)
        samples, considered = observe(events, book, brti, threshold)
        contested = [s for s in samples if s["contested"]]
        result["by_threshold"].append({
            "threshold_sigma": threshold,
            "events_detected": len(events),
            "events_per_hour": len(events) / paired_hours if paired_hours else None,
            "pair_filter_counts": considered,
            "all_markets": summarize(samples, f"{threshold}sigma / all markets"),
            "contested_markets": summarize(
                contested, f"{threshold}sigma / mid in "
                           f"[{CONTESTED_BAND[0]}, {CONTESTED_BAND[1]}]"),
            "model_priced_gap_contested": model_priced_gap(contested),
        })
    common.emit(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
