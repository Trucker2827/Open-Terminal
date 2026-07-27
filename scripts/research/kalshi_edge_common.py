#!/usr/bin/env python3
"""Shared, read-only loaders for the Kalshi edge autopsy (issue #169).

Every function here OPENS EVIDENCE READ-ONLY and writes nothing. The operator's
live ledgers, the sealed gate, the calibrator state and the bot's configuration
are inputs to this analysis and are never touched by it. In particular nothing
here imports or calls `spot_calibrator.run_once()`, which would rewrite
`spot-calibrator-state.json` and `calibrator.json` as a side effect of reading.

Three things in this module are judgement calls rather than recorded facts, and
each is labelled at its definition so the report can cite it as derived:

  1. `parse_ticker` reads the close time out of the ticker string as US/Eastern
     (EDT, UTC-4, for the July window analysed). This is validated empirically,
     not assumed — see `validate_settlement_rule`, which reproduces Kalshi's own
     recorded `expiration_value` from BRTI at that instant to within a dollar.
  2. `derive_outcome` resolves a threshold contract from the BRTI 60-second
     average versus the strike encoded in the ticker. Validated the same way,
     against every recorded settlement available.
  3. `implied_probability` is a Gaussian threshold model. It is a MODEL, used
     only to price the size of a quote-lag gap in Q1; no claim in the report
     rests on it being the true distribution.

Fee arithmetic mirrors `advisor_core.fee_per_contract` (a frozen v5 duel file —
reimplemented here rather than imported so that reading evidence can never pull
a frozen module's import side effects into an analysis process).
"""
import bisect
import datetime
import json
import math
import os
import re
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.abspath(os.path.join(_HERE, "..", "..",
                                                "openmarketterminal-qt", "scripts")))
from openterminal_paths import evidence_dir  # noqa: E402
# From the same directory as openterminal_paths above. Imported for the series'
# paths and constants only — nothing here runs the compactor, so reading
# evidence still writes nothing.
import kalshi_lag_series  # noqa: E402

# Kalshi rotates each evidence log at ~67 MB into a single `.1` sibling, which
# the next rotation overwrites. Retention is therefore a property of the file's
# write rate, not of time — see the report's data-inventory table.
ROTATIONS = ("{name}.1", "{name}")

# Issue #170: the retained lag series (`kalshi-lag-series/`) is a downsampled,
# TIME-bounded copy of these two streams, written by
# openmarketterminal-qt/scripts/kalshi_lag_series.py. It is read here as the
# OLDEST ROTATION of the same stream — the rows are in the source schema, so
# every reader above (q1 in particular) sees a longer history and needs no
# edit. The key is what each stream's own consumer already uses for time:
# `load_brti` reads BRTI's `time` (int), while a ticker row's `ts_ms` is the
# instant q1 bisects on.
RETAINED_TS_KEY = {kalshi_lag_series.SOURCE_TICKERS: "ts_ms",
                   kalshi_lag_series.SOURCE_BRTI: "time"}

MONTHS = {"JAN": 1, "FEB": 2, "MAR": 3, "APR": 4, "MAY": 5, "JUN": 6,
          "JUL": 7, "AUG": 8, "SEP": 9, "OCT": 10, "NOV": 11, "DEC": 12}
EASTERN_JULY = datetime.timezone(datetime.timedelta(hours=-4))   # EDT

_TICKER_DATE = re.compile(r"^(\d{2})([A-Z]{3})(\d{2})(\d{2})(\d{2})?$")
_TICKER_STRIKE = re.compile(r"-T(\d+(?:\.\d+)?)$")


def evidence_path(name):
    return os.path.join(evidence_dir(), name)


def iter_jsonl(name, rotations=ROTATIONS):
    """Yield every parseable record of an evidence log, oldest rotation first.

    A malformed trailing line (the writer was mid-append when we read) is
    skipped, and the count of skips is reported by `read_jsonl` so a truncated
    tail never silently shrinks a denominator.
    """
    for pattern in rotations:
        path = evidence_path(pattern.format(name=name))
        if not os.path.exists(path):
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                try:
                    yield json.loads(line)
                except ValueError:
                    continue


def _row_ts_ms(record, key):
    """The row's own instant, coerced — BRTI writes `ts_ms` as a string."""
    try:
        return int(float(record.get(key)))
    except (TypeError, ValueError):
        return None


def read_retained(name, before_ms):
    """(records, inventory) from the retained lag series, oldest day first.

    Only rows STRICTLY OLDER than `before_ms` — the first instant the live logs
    still hold — are returned, so the overlap between the series and the log it
    was distilled from is resolved in favour of the full-fidelity live rows and
    nothing is double counted. `before_ms` of None means the live log holds
    nothing (it just rotated), and then the whole series is in play.

    A row is attributed to its stream by the `source` field the compactor
    stamps on it; the per-day header row carries no `source` and is therefore
    invisible to every consumer.
    """
    key = RETAINED_TS_KEY.get(name)
    if key is None:
        return [], []
    records = []
    inventory = []
    for file_name in kalshi_lag_series.day_files():
        path = kalshi_lag_series.series_path(file_name)
        good = bad = skipped = 0
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                try:
                    record = json.loads(line)
                except ValueError:
                    bad += 1
                    continue
                if record.get("source") != name:
                    continue
                ts_ms = _row_ts_ms(record, key)
                if ts_ms is None:
                    continue
                if before_ms is not None and ts_ms >= before_ms:
                    skipped += 1
                    continue
                records.append(record)
                good += 1
        if good or bad or skipped:
            inventory.append({"file": os.path.join(kalshi_lag_series.SERIES_DIR_NAME,
                                                   file_name),
                              "bytes": os.path.getsize(path),
                              "rows": good, "unparseable_rows": bad,
                              "rows_superseded_by_live_log": skipped,
                              "retention_days": kalshi_lag_series.RETENTION_DAYS})
    return records, inventory


def read_jsonl(name, rotations=ROTATIONS):
    """(records, inventory) — inventory is the per-file audit for the report.

    For the two streams the lag series retains (issue #170), the series is read
    first and prepended: it is the same stream, recorded by the same writer,
    kept longer. Callers sort by timestamp anyway, and the inventory names every
    file the numbers came from — including which of them is the retained copy.
    """
    records = []
    inventory = []
    for pattern in rotations:
        path = evidence_path(pattern.format(name=name))
        if not os.path.exists(path):
            continue
        good = bad = 0
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                try:
                    records.append(json.loads(line))
                    good += 1
                except ValueError:
                    bad += 1
        inventory.append({"file": os.path.basename(path),
                          "bytes": os.path.getsize(path),
                          "rows": good, "unparseable_rows": bad})
    key = RETAINED_TS_KEY.get(name)
    if key is None:
        return records, inventory
    live_start = None
    for record in records:
        ts_ms = _row_ts_ms(record, key)
        if ts_ms is not None and (live_start is None or ts_ms < live_start):
            live_start = ts_ms
    retained, retained_inventory = read_retained(name, live_start)
    return retained + records, retained_inventory + inventory


def parse_ticker(ticker):
    """Decompose a Kalshi crypto ticker into family, close instant and strike.

    `KXBTCD-26JUL2311-T63999.99` -> hourly threshold, closes 2026-07-23 11:00 ET,
    settles YES if the index exceeds 63999.99.
    `KXBTC15M-26JUL270330-30`    -> 15-minute directional, closes 03:30 ET, no
    strike in the ticker (it settles against the window's own open, which the
    ticker does not carry) — `strike` is None and such contracts are resolvable
    only from recorded settlements, never derived.

    Returns None when the ticker does not match either shape; a caller that
    cannot parse a ticker must drop it and say so, not guess a close time.

    `kalshi_lag_series.parse_threshold_ticker` decomposes the same strings to
    decide what the retained series keeps. The two must agree about close time
    and strike or the series would retain a different population than this
    analysis asks about; `test_kalshi_lag_series.py` asserts they do.
    """
    parts = ticker.split("-")
    if len(parts) < 2:
        return None
    matched = _TICKER_DATE.match(parts[1])
    if not matched:
        return None
    yy, mon, dd, hh, mm = matched.groups()
    close = datetime.datetime(2000 + int(yy), MONTHS[mon], int(dd),
                              int(hh), int(mm or 0), tzinfo=EASTERN_JULY)
    strike_match = _TICKER_STRIKE.search(ticker)
    return {"ticker": ticker,
            "family": parts[0],
            "close_ms": int(close.timestamp() * 1000),
            "close_utc": close.astimezone(datetime.timezone.utc),
            "strike": float(strike_match.group(1)) if strike_match else None}


class BrtiSeries:
    """The CF BRTI index the Kalshi crypto contracts settle against.

    Carries both the instantaneous value and Kalshi's published 60-second
    average ending at that timestamp; `validate_settlement_rule` shows the
    latter is what settlement uses.
    """

    def __init__(self, samples):
        self.samples = sorted(samples)
        self.times = [s[0] for s in self.samples]

    def __len__(self):
        return len(self.samples)

    @property
    def span_ms(self):
        if not self.samples:
            return (None, None)
        return (self.times[0], self.times[-1])

    def _nearest_index(self, ts_ms):
        if not self.samples:
            return None
        idx = min(bisect.bisect_left(self.times, ts_ms), len(self.times) - 1)
        candidates = [i for i in (idx - 1, idx) if 0 <= i < len(self.times)]
        return min(candidates, key=lambda i: abs(self.times[i] - ts_ms))

    def nearest(self, ts_ms, max_gap_ms=5000):
        """(ts, spot, avg60) nearest `ts_ms`, or None when the feed has a hole.

        A gap wider than `max_gap_ms` reads as missing rather than as the last
        value carried forward: a stale spot is exactly the error this analysis
        is trying to measure in the Kalshi book, so it must not be introduced
        into the spot side by the reader.
        """
        idx = self._nearest_index(ts_ms)
        if idx is None or abs(self.times[idx] - ts_ms) > max_gap_ms:
            return None
        return self.samples[idx]

    def window(self, start_ms, end_ms):
        lo = bisect.bisect_left(self.times, start_ms)
        hi = bisect.bisect_right(self.times, end_ms)
        return self.samples[lo:hi]


def load_brti():
    """(BrtiSeries, inventory) over every retained rotation of the CF feed."""
    records, inventory = read_jsonl("kalshi-cf-benchmarks.jsonl")
    samples = []
    for record in records:
        if record.get("id") != "BRTI":
            continue
        try:
            ts_ms = int(record["time"])
            spot = float(record["value"])
        except (KeyError, TypeError, ValueError):
            continue
        avg = (record.get("avg_60s_data") or {}).get("value")
        try:
            avg60 = float(avg) if avg is not None else None
        except (TypeError, ValueError):
            avg60 = None
        samples.append((ts_ms, spot, avg60))
    return BrtiSeries(samples), inventory


def load_settlements(interesting=None):
    """market_id -> recorded settlement row, from the public settlement feed.

    `interesting` restricts the (large) feed to the tickers a caller cares
    about. Duplicate rows for one market are collapsed; a genuine conflict
    would be a data-integrity finding, so the count is returned rather than
    silently resolved.
    """
    rows = {}
    conflicts = 0
    total = 0
    for record in iter_jsonl("kalshi-settlements.jsonl", rotations=("{name}",)):
        market_id = record.get("kalshi_market_id")
        if not market_id:
            continue
        total += 1
        if interesting is not None and market_id not in interesting:
            continue
        previous = rows.get(market_id)
        if previous is not None and previous.get("result") != record.get("result"):
            conflicts += 1
        rows[market_id] = record
    return rows, {"settlement_rows_scanned": total, "conflicting_results": conflicts}


def derive_outcome(parsed, brti, max_gap_ms=5000):
    """True/False from the BRTI 60-second average at close versus the strike.

    Returns None — never a guess — when the contract carries no strike, when the
    close falls outside the retained BRTI window, or when the feed has a hole at
    that instant. The rule's fidelity against Kalshi's own recorded results is
    measured by `validate_settlement_rule`, and the report quotes that number
    beside every table this feeds.
    """
    if parsed is None or parsed["strike"] is None:
        return None
    sample = brti.nearest(parsed["close_ms"], max_gap_ms=max_gap_ms)
    if sample is None:
        return None
    _, spot, avg60 = sample
    value = avg60 if avg60 is not None else spot
    return value > parsed["strike"]


def validate_settlement_rule(settlements, brti):
    """Score `derive_outcome` against every recorded settlement it can reach.

    This is the honesty gate for using derived outcomes at all: the report
    quotes `agreement`/`compared` and the dollar error of the reconstructed
    settlement value, and no derived-outcome table is published without it.
    """
    compared = agreement = 0
    abs_errors = []
    for market_id, row in settlements.items():
        parsed = parse_ticker(market_id)
        recorded = {"yes": True, "no": False}.get(row.get("result"))
        if parsed is None or parsed["strike"] is None or recorded is None:
            continue
        derived = derive_outcome(parsed, brti)
        if derived is None:
            continue
        compared += 1
        agreement += 1 if derived == recorded else 0
        try:
            expiration = float(row.get("expiration_value") or 0.0)
        except (TypeError, ValueError):
            expiration = 0.0
        sample = brti.nearest(parsed["close_ms"])
        if expiration > 0.0 and sample is not None and sample[2] is not None:
            abs_errors.append(abs(sample[2] - expiration))
    abs_errors.sort()
    return {"compared": compared, "agreement": agreement,
            "mean_abs_value_error_usd": (sum(abs_errors) / len(abs_errors)
                                         if abs_errors else None),
            "max_abs_value_error_usd": (abs_errors[-1] if abs_errors else None),
            "value_error_samples": len(abs_errors)}


# ── scoring ──────────────────────────────────────────────────────────────────
def brier(pairs):
    """Mean squared error of (probability, outcome) pairs; None when empty."""
    if not pairs:
        return None
    return sum((p - (1.0 if y else 0.0)) ** 2 for p, y in pairs) / len(pairs)


def logit(p, eps=1e-6):
    p = min(1.0 - eps, max(eps, p))
    return math.log(p / (1.0 - p))


def sigmoid(z):
    return 1.0 / (1.0 + math.exp(-max(-30.0, min(30.0, z))))


def _mean_logloss(xs, ys, a, b):
    total = 0.0
    for x, y in zip(xs, ys):
        q = min(1.0 - 1e-12, max(1e-12, sigmoid(a * x + b)))
        total += -(y * math.log(q) + (1.0 - y) * math.log(1.0 - q))
    return total / len(xs)


def platt_fit(pairs, iters=50, ridge=1e-6):
    """p' = sigmoid(a*logit(p) + b) by damped Newton on log-loss.

    Same estimator as `arena_report.platt_fit` (PR #110) — reimplemented rather
    than imported so this read-only analysis has no dependency on the arena
    module's import-time behaviour, and deliberately identical so the two
    overlays remain comparable.
    """
    a, b = 1.0, 0.0
    if not pairs:
        return a, b
    xs = [logit(p) for p, _ in pairs]
    ys = [1.0 if y else 0.0 for _, y in pairs]
    loss = _mean_logloss(xs, ys, a, b)
    for _ in range(iters):
        g_a = g_b = h_aa = h_ab = h_bb = 0.0
        for x, y in zip(xs, ys):
            q = sigmoid(a * x + b)
            err = q - y
            w = max(q * (1.0 - q), 1e-12)
            g_a += err * x
            g_b += err
            h_aa += w * x * x
            h_ab += w * x
            h_bb += w
        h_aa += ridge
        h_bb += ridge
        det = h_aa * h_bb - h_ab * h_ab
        if det <= 0.0:
            break
        da = (g_a * h_bb - g_b * h_ab) / det
        db = (g_b * h_aa - g_a * h_ab) / det
        step, improved = 1.0, False
        for _ in range(30):
            trial = _mean_logloss(xs, ys, a - step * da, b - step * db)
            if trial < loss:
                a, b, loss, improved = a - step * da, b - step * db, trial, True
                break
            step *= 0.5
        if not improved or (abs(step * da) < 1e-10 and abs(step * db) < 1e-10):
            break
    return a, b


def platt_apply(a, b, p):
    return sigmoid(a * logit(p) + b)


def isotonic_fit(pairs):
    """Pool-adjacent-violators isotonic regression on (probability, outcome).

    Returns the fitted step function as a list of (x_upper, value) blocks;
    `isotonic_apply` interpolates nothing — it returns the containing block's
    value, which is what makes the fit honest about its own resolution.
    """
    if not pairs:
        return []
    ordered = sorted(pairs, key=lambda pair: pair[0])
    blocks = [[p, 1.0 if y else 0.0, 1] for p, y in ordered]   # [x_hi, sum, n]
    merged = []
    for block in blocks:
        merged.append(block)
        while len(merged) > 1 and (merged[-2][1] / merged[-2][2]
                                   > merged[-1][1] / merged[-1][2]):
            last = merged.pop()
            merged[-1][0] = last[0]
            merged[-1][1] += last[1]
            merged[-1][2] += last[2]
    return [(x_hi, total / count) for x_hi, total, count in merged]


def isotonic_apply(blocks, p):
    if not blocks:
        return p
    for x_hi, value in blocks:
        if p <= x_hi:
            return value
    return blocks[-1][1]


def reliability_bins(pairs, n_bins=10):
    """Equal-width reliability bins; an empty bin reads missing, never zero."""
    bins = [{"lo": i / n_bins, "hi": (i + 1) / n_bins, "count": 0,
             "mean_predicted": None, "observed_rate": None}
            for i in range(n_bins)]
    sums = [[0.0, 0.0] for _ in range(n_bins)]
    for p, y in pairs:
        idx = min(int(p * n_bins), n_bins - 1)
        bins[idx]["count"] += 1
        sums[idx][0] += p
        sums[idx][1] += 1.0 if y else 0.0
    for idx, entry in enumerate(bins):
        if entry["count"]:
            entry["mean_predicted"] = sums[idx][0] / entry["count"]
            entry["observed_rate"] = sums[idx][1] / entry["count"]
    return bins


def fee_per_contract(price):
    """Kalshi's published trading fee, ceil(0.07 * P * (1-P) * 100) cents.

    Identical arithmetic to `advisor_core.fee_per_contract` and to the C++
    dispatcher's `fee_cents` (CommandDispatch.cpp); duplicated deliberately —
    see the module docstring on not importing frozen duel files.
    """
    p = min(1.0, max(0.0, price))
    return math.ceil(0.07 * p * (1.0 - p) * 100.0) / 100.0


def normal_cdf(z):
    return 0.5 * (1.0 + math.erf(z / math.sqrt(2.0)))


def implied_probability(spot, strike, sigma_per_min_bps, minutes_left):
    """MODEL (not a recorded quantity): Gaussian threshold probability.

    P(index > strike at close) under a driftless random walk whose per-minute
    volatility is `sigma_per_min_bps`. Used ONLY to convert a spot move into
    the probability move it implies, so a quote-lag gap can be sized in cents.
    Returns None when the inputs cannot support a number.
    """
    if spot <= 0.0 or sigma_per_min_bps <= 0.0 or minutes_left <= 0.0:
        return None
    sigma = spot * (sigma_per_min_bps / 10000.0) * math.sqrt(minutes_left)
    if sigma <= 0.0:
        return None
    return normal_cdf((spot - strike) / sigma)


def realized_vol_per_min_bps(samples):
    """Per-minute realized volatility in bps from a BRTI window.

    Computed from log returns between consecutive samples and rescaled to one
    minute by the observed mean sampling interval — the feed is nominally 1 Hz
    but its actual cadence varies by rotation, so the scaling uses the cadence
    the data shows rather than the cadence it is supposed to have. Returns None
    below 30 usable returns instead of a noisy number.
    """
    usable = [(ts, spot) for ts, spot, _ in samples if spot and spot > 0.0]
    if len(usable) < 31:
        return None
    returns = []
    spans = []
    for (t0, s0), (t1, s1) in zip(usable, usable[1:]):
        dt = t1 - t0
        if dt <= 0:
            continue
        returns.append(math.log(s1 / s0))
        spans.append(dt)
    if len(returns) < 30:
        return None
    mean_dt_ms = sum(spans) / len(spans)
    if mean_dt_ms <= 0:
        return None
    variance = sum(r * r for r in returns) / len(returns)     # driftless
    per_sample_bps = math.sqrt(variance) * 10000.0
    return per_sample_bps * math.sqrt(60000.0 / mean_dt_ms)


def iso(ts_ms):
    if ts_ms is None:
        return None
    return datetime.datetime.fromtimestamp(ts_ms / 1000.0,
                                           datetime.timezone.utc).isoformat()


def as_of():
    """Stamp printed by every script so a table can never drift undated."""
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def emit(payload):
    print(json.dumps(payload, indent=2, sort_keys=True, default=str))
