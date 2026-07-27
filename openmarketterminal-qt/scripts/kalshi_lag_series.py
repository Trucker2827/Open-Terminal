#!/usr/bin/env python3
"""Retain a downsampled paired spot + top-of-book series (issue #170).

WHY THIS EXISTS. The Kalshi edge autopsy (#169,
`docs/research/2026-07-27-kalshi-edge-autopsy.md`) found one structural edge
the market does not already price: the Kalshi book lags spot. After a >=3 sigma
BTC move the Kalshi mid drifts a further +2.07c over the next 15 seconds and
has caught up by 60. Half-spread is 0.53c and the fee is 1.55c, so the edge is
almost exactly fee-sized — and whether that is really +0.5c or really -0.5c
cannot be settled on 8 events. It cannot be settled on more events either,
because of RETENTION, not capture:

    kalshi-cf-benchmarks.jsonl  (BRTI spot)         2.6 s cadence   116.4 h
    kalshi-tickers.jsonl        (Kalshi top-of-book) 0.12 s cadence    8.0 h

Every evidence log rotates by SIZE (~67 MB) into a single `.1` sibling that the
next rotation overwrites, so retention is a function of write rate. Everything
this analysis needs already streams at sub-second resolution. Nothing new needs
recording — it needs KEEPING.

WHAT THIS KEEPS. A distillate of the two logs above, small enough that a
TIME bound (30 days) is affordable where a size bound is not:

  BRTI      at most one row per second (~1 Hz), the settlement index itself.
  Top-of-book  per market, on every change of (yes_bid, yes_ask), plus a
            heartbeat at least every HEARTBEAT_MS so that "unchanged" is
            distinguishable from "unobserved"; restricted to KXBTCD `-T`
            THRESHOLD contracts within MAX_SECONDS_TO_CLOSE of expiry.

The restriction is what makes the series small: measured over one full ticker
rotation, 93.9k in-band rows carried only 6.5k top-of-book changes (6.9%), and
the whole retained series costs ~27 MB/day — 0.8 GB for 30 days against a
source stream that spends 67 MB every 5.7 hours.

HONESTY RULES THIS FILE OBEYS.
  * Read-only against every existing evidence log. This process opens the
    operator's ledgers with mode "r" and writes only inside `kalshi-lag-series/`.
  * A one-sided book is retained AS one-sided. The recorded `yes_bid_dollars`
    and `yes_ask_dollars` strings are copied verbatim and are never completed
    into a midpoint from the NO side (`1 - no_bid`) or by treating a missing
    ask as 1.0. The autopsy dropped 107,955 one-sided quote rows for exactly
    this reason; a series that quietly repaired them would answer the question
    it was built to ask. `book_sided` names the state; it never invents a price.
  * A hole reads as a hole. If this job is down long enough for a source to
    rotate away, the missing span is recorded in the manifest's `gaps` rather
    than closed over — a 30-day series that looks continuous across a
    three-hour outage would silently corrupt any lag measurement spanning it.
  * Nothing is derived. Every retained row is a copy of a recorded one, plus
    provenance (`source`, `series_row`) and the two facts already implied by
    the ticker string (`book_sided`, `seconds_to_close`).

WHY THERE IS NO BACKFILL BEYOND THE LIVE ROTATIONS. This series can only start
from what is still on disk when it first runs, and no other evidence log can
honestly extend it. The one long-retained log carrying a `yes_bid`/`yes_ask`
pair for these markets, `kalshi-crypto-decisions.jsonl` (~10 days), was measured
against the ticker feed over their overlap: of 1,977 rows only 46 (2.3%) match
the recorded top of book, and the mismatch is systematic — its `yes_bid` is
`ask - 0.005`, i.e. the MIDPOINT, not a bid. Distilling it into a "top-of-book"
series would fabricate a book. `kalshi-venue-features.jsonl` (16 days) fails the
same way and was measured too: over 1,805 overlapping rows its
`kalshi_yes_price` equals the recorded bid 75 times (4.2%) and `1 -
kalshi_no_price` equals the recorded ask 88 times (4.9%) — it carries a midpoint
and its complement, so pairing the two into a spread would be exactly the
`1 - no_bid` completion this file refuses to perform. The window therefore grows
forwards only, at the rate the job runs.

SCHEMA. Retained rows are written in the SOURCE logs' own schemas (a field
subset plus the provenance keys above), so every existing reader works on them
unchanged — `scripts/research/kalshi_edge_common.read_jsonl` treats the series
as the oldest rotation of the same stream, and `q1_quote_lag.py` needs no edit
at all.

USAGE (read-only against evidence; writes only the series directory):

    python3 kalshi_lag_series.py compact           # distil new source rows
    python3 kalshi_lag_series.py status            # print the manifest
    python3 kalshi_lag_series.py status --json     # ... as JSON
    python3 kalshi_lag_series.py rebuild-manifest  # rescan the day files

Deployment: `deploy/org.openterminal.lag-series.plist` (documented, not
auto-loaded — live infrastructure is bounced deliberately). The job must run
more often than the source rotates: kalshi-tickers.jsonl turns over in ~5.7
hours at the current write rate, so the plist runs it every 15 minutes and any
longer outage shows up as a gap rather than as silence.
"""
import argparse
import datetime
import json
import os
import re
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
from openterminal_paths import evidence_dir  # noqa: E402

SCHEMA_VERSION = 1
SERIES_DIR_NAME = "kalshi-lag-series"
FILE_PREFIX = "kalshi-lag-series-"
MANIFEST_NAME = "manifest.json"
STATE_NAME = "state.json"

# TIME-BOUNDED, NEVER SIZE-BOUNDED. A size rotation is precisely the defect
# this series exists to fix: it makes retention a function of write rate, so
# the busiest (most informative) stream keeps the least history. Day files
# whose LAST row is older than this are deleted whole.
RETENTION_DAYS = 30

# One BRTI row per second at most. The source publishes every ~2.6 s, so today
# this cap removes nothing; it bounds the cost if the feed ever speeds up.
BRTI_MIN_INTERVAL_MS = 1_000

# RECIPROCAL CONSTRAINT — keep in step with `QUOTE_STALENESS_MS = 15_000` in
# scripts/research/q1_quote_lag.py, which treats a quote older than 15 s as
# MISSING rather than stale. On-change compaction alone would therefore silently
# delete samples: an unchanged book emits nothing, so a live quote could read as
# 5 minutes old and q1 would drop it. The heartbeat keeps every retained market
# no more than 10 s stale, which is what makes "q1 runs unchanged" true. If
# either number moves, the other must move with it — `test_kalshi_lag_series.py`
# imports both and asserts HEARTBEAT_MS < QUOTE_STALENESS_MS.
HEARTBEAT_MS = 10_000

# The lag question lives in the last hour before expiry; everything else is the
# bulk of the stream and none of the signal.
MIN_SECONDS_TO_CLOSE = 0
MAX_SECONDS_TO_CLOSE = 3600

SOURCE_TICKERS = "kalshi-tickers.jsonl"
SOURCE_BRTI = "kalshi-cf-benchmarks.jsonl"
ROTATIONS = ("{name}.1", "{name}")

# A source row older than its stream's cursor by more than this is evidence the
# log rotated away before we compacted it — i.e. a hole in the series.
GAP_TOLERANCE_MS = 60_000

BRTI_INDEX = "BRTI"

MONTHS = {"JAN": 1, "FEB": 2, "MAR": 3, "APR": 4, "MAY": 5, "JUN": 6,
          "JUL": 7, "AUG": 8, "SEP": 9, "OCT": 10, "NOV": 11, "DEC": 12}
EASTERN = datetime.timezone(datetime.timedelta(hours=-4))   # EDT
_TICKER_DATE = re.compile(r"^(\d{2})([A-Z]{3})(\d{2})(\d{2})(\d{2})?$")
_TICKER_STRIKE = re.compile(r"-T(\d+(?:\.\d+)?)$")

RETENTION_SENTENCE = (
    "time-bounded retention: a day file is deleted once its newest row is more "
    "than {days} days old. This series is NEVER rotated by size — a size "
    "rotation is the defect issue #170 exists to fix."
)


# ── paths ────────────────────────────────────────────────────────────────────
def series_dir():
    return os.path.join(evidence_dir(), SERIES_DIR_NAME)


def series_path(name):
    return os.path.join(series_dir(), name)


def source_path(name):
    return os.path.join(evidence_dir(), name)


def day_file_name(ts_ms):
    day = datetime.datetime.fromtimestamp(ts_ms / 1000.0, datetime.timezone.utc)
    return "%s%s.jsonl" % (FILE_PREFIX, day.strftime("%Y-%m-%d"))


def day_files():
    """Every retained day file, oldest first (the name sorts chronologically)."""
    directory = series_dir()
    if not os.path.isdir(directory):
        return []
    return sorted(name for name in os.listdir(directory)
                  if name.startswith(FILE_PREFIX) and name.endswith(".jsonl"))


# ── ticker parsing ───────────────────────────────────────────────────────────
def parse_threshold_ticker(ticker):
    """`KXBTCD-26JUL2719-T66499.99` -> close instant and strike, else None.

    Deliberately the same decomposition as
    `scripts/research/kalshi_edge_common.parse_ticker`, and cross-checked
    against it by `test_kalshi_lag_series.py`: if this filter and the analysis
    disagreed about when a contract closes, the series would retain a different
    population than the question is asked of. Returns None for anything that is
    not a `-T` THRESHOLD contract — `KXBTC-...-B65050` BAND markets are
    excluded here for the same reason q1 excludes them (YES is not monotone in
    spot for a band), and 15-minute directionals carry no strike at all.
    """
    parts = ticker.split("-")
    if len(parts) < 2:
        return None
    matched = _TICKER_DATE.match(parts[1])
    if not matched:
        return None
    strike = _TICKER_STRIKE.search(ticker)
    if not strike:
        return None
    yy, mon, dd, hh, mm = matched.groups()
    if mon not in MONTHS:
        return None
    close = datetime.datetime(2000 + int(yy), MONTHS[mon], int(dd),
                              int(hh), int(mm or 0), tzinfo=EASTERN)
    return {"family": parts[0],
            "close_ms": int(close.timestamp() * 1000),
            "strike": float(strike.group(1))}


def book_sided(bid, ask):
    """Name the book's sidedness without ever inventing the missing side.

    Kalshi quotes in whole cents between 1c and 99c: a `yes_bid_dollars` of
    0.0000 means nobody bids and a `yes_ask_dollars` of 1.0000 means nobody
    offers. Both are retained as recorded. There is no midpoint here and no
    completion from `1 - no_bid` — the ticker row does not even carry the NO
    side, and a future edit must not add one.
    """
    has_bid = bid > 0.0
    has_ask = ask < 1.0
    if has_bid and has_ask:
        return "both"
    if has_bid:
        return "bid_only"
    if has_ask:
        return "ask_only"
    return "empty"


# ── source reading (read-only) ───────────────────────────────────────────────
def _iter_source(name):
    """Yield (path, record) over every retained rotation, oldest first.

    Opened "r": this process never writes to an existing evidence log.
    """
    for pattern in ROTATIONS:
        path = source_path(pattern.format(name=name))
        if not os.path.exists(path):
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                try:
                    yield path, json.loads(line)
                except ValueError:
                    # A malformed trailing line means the writer was mid-append.
                    continue


def _as_ms(value):
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return None


def collect_quotes(after_ms):
    """(rows, oldest_seen_ms) — in-band threshold quotes newer than the cursor.

    `oldest_seen_ms` is the oldest row the SOURCE still holds, in-band or not:
    it is what tells the caller whether the log rotated past the cursor.
    """
    rows = []
    oldest = None
    for _, record in _iter_source(SOURCE_TICKERS):
        ts_ms = _as_ms(record.get("ts_ms"))
        if ts_ms is None:
            continue
        oldest = ts_ms if oldest is None else min(oldest, ts_ms)
        if after_ms is not None and ts_ms <= after_ms:
            continue
        ticker = record.get("market_ticker")
        if not ticker:
            continue
        parsed = parse_threshold_ticker(ticker)
        if parsed is None:
            continue
        seconds_to_close = (parsed["close_ms"] - ts_ms) / 1000.0
        if not MIN_SECONDS_TO_CLOSE <= seconds_to_close <= MAX_SECONDS_TO_CLOSE:
            continue
        bid_raw = record.get("yes_bid_dollars")
        ask_raw = record.get("yes_ask_dollars")
        try:
            bid = float(bid_raw)
            ask = float(ask_raw)
        except (TypeError, ValueError):
            continue
        rows.append({
            "stream": SOURCE_TICKERS,
            "ts_ms": ts_ms,
            "ticker": ticker,
            "bid": bid,
            "ask": ask,
            "seconds_to_close": seconds_to_close,
            # Verbatim, in the source's own formatting.
            "row": {
                "event": "kalshi_ticker",
                "market_ticker": ticker,
                "ts_ms": ts_ms,
                "yes_bid_dollars": bid_raw,
                "yes_ask_dollars": ask_raw,
                "yes_bid_size_fp": record.get("yes_bid_size_fp"),
                "yes_ask_size_fp": record.get("yes_ask_size_fp"),
                "book_sided": book_sided(bid, ask),
                "seconds_to_close": round(seconds_to_close, 3),
                "source": SOURCE_TICKERS,
            },
        })
    return rows, oldest


def collect_brti(after_ms):
    """(rows, oldest_seen_ms) — BRTI index samples newer than the cursor."""
    rows = []
    oldest = None
    for _, record in _iter_source(SOURCE_BRTI):
        if record.get("id") != BRTI_INDEX:
            continue
        ts_ms = _as_ms(record.get("time"))
        if ts_ms is None:
            continue
        oldest = ts_ms if oldest is None else min(oldest, ts_ms)
        if after_ms is not None and ts_ms <= after_ms:
            continue
        try:
            value = float(record["value"])
        except (KeyError, TypeError, ValueError):
            continue
        rows.append({
            "stream": SOURCE_BRTI,
            "ts_ms": ts_ms,
            "row": {
                "event": "kalshi_cf_benchmark_value",
                "id": BRTI_INDEX,
                "index_id": BRTI_INDEX,
                "time": ts_ms,
                "ts_ms": str(ts_ms),
                "value": value,
                # Kalshi settles against the 60-second average, not the tick;
                # keeping it is what lets a future analysis reconstruct a
                # settlement without the raw feed.
                "avg_60s_data": record.get("avg_60s_data"),
                "source": SOURCE_BRTI,
            },
        })
    return rows, oldest


# ── state / manifest ─────────────────────────────────────────────────────────
def default_state():
    """Everything a run needs from its predecessor.

    `cursors` is the newest SOURCE row consumed per stream (the gap detector
    compares it against the oldest row the source still holds); `emitted` the
    newest RETAINED BRTI instant, so the 1 Hz cap survives a restart; `markets`
    the last quote emitted per market, so neither the change test nor the
    heartbeat clock resets at a run boundary.
    """
    return {"schema": SCHEMA_VERSION, "cursors": {}, "emitted": {},
            "markets": {}, "files": {}, "gaps": []}


def load_state():
    path = series_path(STATE_NAME)
    if not os.path.exists(path):
        return default_state()
    try:
        with open(path, "r", encoding="utf-8") as handle:
            state = json.load(handle)
    except ValueError:
        return default_state()
    base = default_state()
    base.update({k: state.get(k, v) for k, v in base.items()})
    return base


def write_json_atomic(path, payload):
    """Temp file + os.replace, so a reader never sees a half-written manifest."""
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")
    os.replace(tmp, path)


def header_row(day):
    """First line of every day file: what it is and how long it is kept."""
    return {
        "event": "kalshi_lag_series_header",
        "schema": SCHEMA_VERSION,
        "day_utc": day,
        "issue": 170,
        "written_by": "openmarketterminal-qt/scripts/kalshi_lag_series.py",
        "retention_days": RETENTION_DAYS,
        "retention": RETENTION_SENTENCE.format(days=RETENTION_DAYS),
        "brti_rule": "BRTI index samples, at most one per %d ms" % BRTI_MIN_INTERVAL_MS,
        "quote_rule": ("KXBTCD -T threshold contracts within %d s of close: "
                       "every top-of-book change, plus a heartbeat at least "
                       "every %d ms per market"
                       % (MAX_SECONDS_TO_CLOSE, HEARTBEAT_MS)),
        "one_sided_rule": ("a one-sided book is retained as one-sided "
                           "(book_sided); no midpoint is ever completed from "
                           "1 - no_bid or from a missing side"),
        "sources": [SOURCE_TICKERS, SOURCE_BRTI],
    }


def iso(ts_ms):
    if ts_ms is None:
        return None
    return datetime.datetime.fromtimestamp(
        ts_ms / 1000.0, datetime.timezone.utc).isoformat()


def now_ms():
    return int(datetime.datetime.now(datetime.timezone.utc).timestamp() * 1000)


def file_stats(path):
    """Rescan one day file into the stats the manifest publishes for it.

    Per-stream spans are kept separately because the PAIRED window — the only
    one a lag measurement can use — is the intersection of the two, and a
    series that quoted a single combined span would overstate it.
    """
    stats = {"rows": 0, "quote_rows": 0, "brti_rows": 0,
             "first_ts_ms": None, "last_ts_ms": None,
             "quote_first_ts_ms": None, "quote_last_ts_ms": None,
             "brti_first_ts_ms": None, "brti_last_ts_ms": None,
             "markets": 0}
    markets = set()
    brti_times = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except ValueError:
                continue
            source = record.get("source")
            if source == SOURCE_TICKERS:
                ts_ms = _as_ms(record.get("ts_ms"))
                stats["quote_rows"] += 1
                markets.add(record.get("market_ticker"))
                prefix = "quote"
            elif source == SOURCE_BRTI:
                ts_ms = _as_ms(record.get("time"))
                stats["brti_rows"] += 1
                if ts_ms is not None:
                    brti_times.append(ts_ms)
                prefix = "brti"
            else:
                continue                       # the header row carries no data
            stats["rows"] += 1
            if ts_ms is None:
                continue
            for lo_key, hi_key in (("first_ts_ms", "last_ts_ms"),
                                   (prefix + "_first_ts_ms", prefix + "_last_ts_ms")):
                if stats[lo_key] is None or ts_ms < stats[lo_key]:
                    stats[lo_key] = ts_ms
                if stats[hi_key] is None or ts_ms > stats[hi_key]:
                    stats[hi_key] = ts_ms
    stats["markets"] = len([m for m in markets if m])
    brti_times.sort()
    gaps = sorted(b - a for a, b in zip(brti_times, brti_times[1:]))
    stats["brti_median_gap_ms"] = gaps[len(gaps) // 2] if gaps else None
    stats["bytes"] = os.path.getsize(path)
    return stats


def build_manifest(state):
    """The series' own span, row counts and cadence, without reading the rows.

    This file is the answer to "state your window" — a future analysis quotes
    it instead of scanning 30 days of JSONL, and an absent or stale entry reads
    as absent rather than as zero.
    """
    files = []
    total = {"rows": 0, "quote_rows": 0, "brti_rows": 0}
    first = last = None
    quote_first = quote_last = None
    brti_first = brti_last = None
    median_gaps = []
    markets = 0
    for name in day_files():
        stats = dict(state["files"].get(name) or {})
        if not stats:
            stats = file_stats(series_path(name))
        entry = {"file": name}
        entry.update(stats)
        files.append(entry)
        for key in total:
            total[key] += stats.get(key) or 0
        markets = max(markets, stats.get("markets") or 0)
        if stats.get("brti_median_gap_ms"):
            median_gaps.append(stats["brti_median_gap_ms"])
        if stats.get("first_ts_ms") is not None:
            first = (stats["first_ts_ms"] if first is None
                     else min(first, stats["first_ts_ms"]))
        if stats.get("last_ts_ms") is not None:
            last = (stats["last_ts_ms"] if last is None
                    else max(last, stats["last_ts_ms"]))
        if stats.get("quote_first_ts_ms") is not None:
            quote_first = (stats["quote_first_ts_ms"] if quote_first is None
                           else min(quote_first, stats["quote_first_ts_ms"]))
        if stats.get("quote_last_ts_ms") is not None:
            quote_last = (stats["quote_last_ts_ms"] if quote_last is None
                          else max(quote_last, stats["quote_last_ts_ms"]))
        if stats.get("brti_first_ts_ms") is not None:
            brti_first = (stats["brti_first_ts_ms"] if brti_first is None
                          else min(brti_first, stats["brti_first_ts_ms"]))
        if stats.get("brti_last_ts_ms") is not None:
            brti_last = (stats["brti_last_ts_ms"] if brti_last is None
                         else max(brti_last, stats["brti_last_ts_ms"]))
    median_gaps.sort()

    def hours(lo, hi):
        if lo is None or hi is None:
            return None
        return (hi - lo) / 3_600_000.0

    paired_lo = None if None in (quote_first, brti_first) else max(quote_first, brti_first)
    paired_hi = None if None in (quote_last, brti_last) else min(quote_last, brti_last)
    return {
        "schema": SCHEMA_VERSION,
        "issue": 170,
        "generated_at_utc": iso(now_ms()),
        "written_by": "openmarketterminal-qt/scripts/kalshi_lag_series.py",
        "retention": {
            "policy": "time",
            "days": RETENTION_DAYS,
            "note": RETENTION_SENTENCE.format(days=RETENTION_DAYS),
            "size_rotation": False,
        },
        "cadence": {
            "brti_min_interval_ms": BRTI_MIN_INTERVAL_MS,
            "brti_median_gap_ms": (median_gaps[len(median_gaps) // 2]
                                   if median_gaps else None),
            "quote_heartbeat_ms": HEARTBEAT_MS,
            "quote_rule": ("top-of-book change, plus a heartbeat at least every "
                           "%d ms per in-band market" % HEARTBEAT_MS),
        },
        "series": {
            "days": len(files),
            "rows": total["rows"],
            "quote_rows": total["quote_rows"],
            "brti_rows": total["brti_rows"],
            "markets_max_per_day": markets,
            "first_ts_ms": first, "last_ts_ms": last,
            "first_utc": iso(first), "last_utc": iso(last),
            "span_hours": hours(first, last),
            "quote_span_utc": [iso(quote_first), iso(quote_last)],
            "quote_span_hours": hours(quote_first, quote_last),
            "brti_span_utc": [iso(brti_first), iso(brti_last)],
            "brti_span_hours": hours(brti_first, brti_last),
            "paired_window_utc": [iso(paired_lo), iso(paired_hi)],
            "paired_window_hours": hours(paired_lo, paired_hi),
        },
        "sources": [SOURCE_TICKERS, SOURCE_BRTI],
        "cursors": dict(state["cursors"]),
        # A hole reads as a hole: every span the compactor could not cover
        # because the source rotated away before it ran.
        "gaps": list(state["gaps"]),
        "files": files,
    }


# ── compaction ───────────────────────────────────────────────────────────────
def note_gap(state, stream, oldest_available_ms):
    """Record a span the source rotated away before this job reached it."""
    cursor = state["cursors"].get(stream)
    if cursor is None or oldest_available_ms is None:
        return None
    if oldest_available_ms <= cursor + GAP_TOLERANCE_MS:
        return None
    gap = {"stream": stream,
           "from_ts_ms": cursor, "to_ts_ms": oldest_available_ms,
           "from_utc": iso(cursor), "to_utc": iso(oldest_available_ms),
           "gap_seconds": (oldest_available_ms - cursor) / 1000.0,
           "reason": ("the source log rotated past the cursor before this job "
                      "ran: those rows were never retained and are gone")}
    state["gaps"].append(gap)
    return gap


def select_quote_rows(rows, state):
    """On-change plus heartbeat, in timestamp order, carrying state across runs.

    `state["markets"]` remembers the last quote emitted per market so a restart
    does not re-emit every book, and so the heartbeat clock survives the run
    boundary. Entries for markets that have gone quiet are pruned by the caller.
    """
    kept = []
    remembered = state["markets"]
    for row in rows:
        ticker = row["ticker"]
        previous = remembered.get(ticker)
        changed = (previous is None
                   or previous.get("bid") != row["bid"]
                   or previous.get("ask") != row["ask"])
        due = (previous is None
               or row["ts_ms"] - (previous.get("ts_ms") or 0) >= HEARTBEAT_MS)
        if not (changed or due):
            continue
        emitted = dict(row["row"])
        emitted["series_row"] = "change" if changed else "heartbeat"
        kept.append((row["ts_ms"], emitted))
        remembered[ticker] = {"ts_ms": row["ts_ms"],
                              "bid": row["bid"], "ask": row["ask"]}
    return kept


def select_brti_rows(rows, state):
    """At most one BRTI sample per BRTI_MIN_INTERVAL_MS, in timestamp order."""
    kept = []
    last = state["emitted"].get("brti_ms")
    for row in rows:
        if last is not None and row["ts_ms"] - last < BRTI_MIN_INTERVAL_MS:
            continue
        emitted = dict(row["row"])
        emitted["series_row"] = "sample"
        kept.append((row["ts_ms"], emitted))
        last = row["ts_ms"]
    if last is not None:
        state["emitted"]["brti_ms"] = last
    return kept


def prune_market_state(state, horizon_ms):
    """Forget markets whose last quote is older than the in-band horizon."""
    cutoff = (state["cursors"].get(SOURCE_TICKERS) or 0) - horizon_ms
    state["markets"] = {ticker: entry for ticker, entry in state["markets"].items()
                        if (entry.get("ts_ms") or 0) >= cutoff}


def append_rows(rows, state):
    """Append (ts_ms, record) pairs to their UTC day files, updating stats."""
    if not rows:
        return {}
    os.makedirs(series_dir(), exist_ok=True)
    written = {}
    handles = {}
    try:
        for ts_ms, record in rows:
            name = day_file_name(ts_ms)
            handle = handles.get(name)
            if handle is None:
                path = series_path(name)
                fresh = not os.path.exists(path)
                handle = handles[name] = open(path, "a", encoding="utf-8")
                if fresh:
                    day = name[len(FILE_PREFIX):-len(".jsonl")]
                    handle.write(json.dumps(header_row(day), sort_keys=True) + "\n")
            handle.write(json.dumps(record, sort_keys=True) + "\n")
            written[name] = written.get(name, 0) + 1
    finally:
        for handle in handles.values():
            handle.close()
    for name in written:
        state["files"][name] = file_stats(series_path(name))
    return written


def prune(state, now=None):
    """Delete day files whose newest row is older than the retention bound."""
    cutoff = (now if now is not None else now_ms()) - RETENTION_DAYS * 86_400_000
    removed = []
    for name in day_files():
        stats = state["files"].get(name) or file_stats(series_path(name))
        last = stats.get("last_ts_ms")
        if last is None or last >= cutoff:
            continue
        os.remove(series_path(name))
        state["files"].pop(name, None)
        removed.append(name)
    return removed


def compact(now=None):
    """One pass: read what is new, downsample it, append it, prune, publish."""
    os.makedirs(series_dir(), exist_ok=True)
    state = load_state()
    quote_cursor = state["cursors"].get(SOURCE_TICKERS)
    brti_cursor = state["cursors"].get(SOURCE_BRTI)

    quotes, quotes_oldest = collect_quotes(quote_cursor)
    brti, brti_oldest = collect_brti(brti_cursor)
    gaps = [gap for gap in (note_gap(state, SOURCE_TICKERS, quotes_oldest),
                            note_gap(state, SOURCE_BRTI, brti_oldest)) if gap]

    quotes.sort(key=lambda row: row["ts_ms"])
    brti.sort(key=lambda row: row["ts_ms"])
    selected = select_quote_rows(quotes, state) + select_brti_rows(brti, state)
    selected.sort(key=lambda pair: pair[0])

    if quotes:
        state["cursors"][SOURCE_TICKERS] = max(row["ts_ms"] for row in quotes)
    if brti:
        state["cursors"][SOURCE_BRTI] = max(row["ts_ms"] for row in brti)

    written = append_rows(selected, state)
    prune_market_state(state, MAX_SECONDS_TO_CLOSE * 1000)
    removed = prune(state, now=now)
    write_json_atomic(series_path(STATE_NAME), state)
    manifest = build_manifest(state)
    write_json_atomic(series_path(MANIFEST_NAME), manifest)
    return {"quote_rows_read": len(quotes), "brti_rows_read": len(brti),
            "rows_written": sum(written.values()), "files_written": written,
            "files_pruned": removed, "gaps_recorded": gaps,
            "manifest": manifest}


def rebuild_manifest():
    """Rescan every day file (recovers stats after an out-of-band deletion)."""
    state = load_state()
    state["files"] = {name: file_stats(series_path(name)) for name in day_files()}
    write_json_atomic(series_path(STATE_NAME), state)
    manifest = build_manifest(state)
    write_json_atomic(series_path(MANIFEST_NAME), manifest)
    return manifest


def read_manifest():
    path = series_path(MANIFEST_NAME)
    if not os.path.exists(path):
        return None
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def render_status(manifest):
    """Human summary. An absent manifest reads as absent, never as zero."""
    if manifest is None:
        return ("LAG SERIES UNAVAILABLE · %s has never been written; nothing "
                "has been retained yet" % series_path(MANIFEST_NAME))
    series = manifest["series"]
    lines = [
        "KALSHI LAG SERIES · %s" % series_dir(),
        "  retention   %d days (time-bounded, never size-rotated)"
        % manifest["retention"]["days"],
        "  span        %s -> %s (%s h over %d day files)"
        % (series["first_utc"], series["last_utc"],
           "%.2f" % series["span_hours"] if series["span_hours"] is not None else "n/a",
           series["days"]),
        "  rows        %d total · %d quotes · %d BRTI"
        % (series["rows"], series["quote_rows"], series["brti_rows"]),
        "  paired      %s h (%s -> %s)"
        % ("%.2f" % series["paired_window_hours"]
           if series["paired_window_hours"] is not None else "n/a",
           series["paired_window_utc"][0], series["paired_window_utc"][1]),
        "  cadence     BRTI <= 1 row / %d ms (median gap %s ms) · quotes on "
        "change + %d ms heartbeat"
        % (manifest["cadence"]["brti_min_interval_ms"],
           manifest["cadence"]["brti_median_gap_ms"],
           manifest["cadence"]["quote_heartbeat_ms"]),
    ]
    if manifest["gaps"]:
        lines.append("  gaps        %d recorded (source rotated before "
                     "compaction):" % len(manifest["gaps"]))
        for gap in manifest["gaps"][-5:]:
            lines.append("              %s  %s -> %s (%.0f s)"
                         % (gap["stream"], gap["from_utc"], gap["to_utc"],
                            gap["gap_seconds"]))
    else:
        lines.append("  gaps        none recorded")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", nargs="?", default="compact",
                        choices=("compact", "status", "rebuild-manifest"))
    parser.add_argument("--json", action="store_true",
                        help="print JSON instead of the human summary")
    args = parser.parse_args(argv)

    if args.command == "status":
        manifest = read_manifest()
        if args.json:
            print(json.dumps(manifest, indent=2, sort_keys=True))
        else:
            print(render_status(manifest))
        return 0 if manifest is not None else 1

    if args.command == "rebuild-manifest":
        manifest = rebuild_manifest()
    else:
        result = compact()
        manifest = result["manifest"]
        if not args.json:
            print("compacted %d quote rows + %d BRTI rows read -> %d retained"
                  % (result["quote_rows_read"], result["brti_rows_read"],
                     result["rows_written"]))
            for gap in result["gaps_recorded"]:
                print("GAP %s %s -> %s (%.0f s): %s"
                      % (gap["stream"], gap["from_utc"], gap["to_utc"],
                         gap["gap_seconds"], gap["reason"]))
            for name in result["files_pruned"]:
                print("pruned %s (older than %d days)" % (name, RETENTION_DAYS))
    if args.json:
        print(json.dumps(manifest, indent=2, sort_keys=True))
    else:
        print(render_status(manifest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
