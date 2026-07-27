#!/usr/bin/env python3
"""Issue #170 — the retained paired spot + top-of-book series.

Every acceptance criterion of the issue has at least one test here, and the
last class is the one the issue's fifth criterion turns on: q1_quote_lag.py is
run TWICE against the same evidence directory, once with the retained series
present and once without, and the retained run must report a WIDER paired
window and MORE detected events. That is the mechanism the criterion asks for,
proved today against a file that in production takes days to accumulate.

Offline, deterministic, temp evidence dir: no test here reads or writes the
operator's real evidence.
"""
import datetime
import json
import os
import random
import subprocess
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.abspath(os.path.join(_HERE, "..", "scripts"))
RESEARCH = os.path.abspath(os.path.join(_HERE, "..", "..", "scripts", "research"))
sys.path.insert(0, SCRIPTS)
sys.path.insert(0, RESEARCH)

import kalshi_lag_series as series          # noqa: E402
import kalshi_edge_common as common         # noqa: E402
import q1_quote_lag                         # noqa: E402

Q1_PATH = os.path.join(RESEARCH, "q1_quote_lag.py")
HOUR_MS = 3_600_000


def ticker_for(close_ms, strike):
    """The ticker string Kalshi would use for an hourly threshold contract."""
    close = datetime.datetime.fromtimestamp(close_ms / 1000.0, series.EASTERN_EDT)
    months = {v: k for k, v in series.MONTHS.items()}
    return "KXBTCD-%02d%s%02d%02d-T%s" % (close.year % 100, months[close.month],
                                          close.day, close.hour, strike)


def quote_row(ticker, ts_ms, bid, ask, bid_size="100.00", ask_size="100.00"):
    return {"event": "kalshi_ticker", "market_ticker": ticker, "ts_ms": ts_ms,
            "yes_bid_dollars": bid, "yes_ask_dollars": ask,
            "yes_bid_size_fp": bid_size, "yes_ask_size_fp": ask_size,
            "received_ts": series.iso(ts_ms), "live_eligible": False}


def brti_row(ts_ms, value, avg=None):
    return {"event": "kalshi_cf_benchmark_value", "id": "BRTI", "index_id": "BRTI",
            "time": ts_ms, "ts_ms": str(ts_ms), "value": value,
            "avg_60s_data": {"value": "%.8f" % (avg if avg is not None else value),
                             "window_end_ts_exclusive": ts_ms, "window_size": 60},
            "live_eligible": False}


def write_jsonl(path, rows, mode="w"):
    with open(path, mode, encoding="utf-8") as handle:
        for row in rows:
            handle.write(json.dumps(row, sort_keys=True) + "\n")


def read_series_rows(directory):
    """Every retained data row (headers excluded), across all day files."""
    rows = []
    for name in sorted(os.listdir(directory)):
        if not name.startswith(series.FILE_PREFIX):
            continue
        with open(os.path.join(directory, name), "r", encoding="utf-8") as handle:
            for line in handle:
                record = json.loads(line)
                if record.get("source"):
                    rows.append(record)
    return rows


class EvidenceCase(unittest.TestCase):
    """Base: a temp evidence dir wired through OPENTERMINAL_EVIDENCE_DIR."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.evidence = self.tmp.name
        self._previous = os.environ.get("OPENTERMINAL_EVIDENCE_DIR")
        os.environ["OPENTERMINAL_EVIDENCE_DIR"] = self.evidence
        # A whole hour boundary in the recent past: recent enough that nothing
        # is pruned by the 30-day bound, fixed enough to be deterministic.
        now = int(datetime.datetime.now(datetime.timezone.utc).timestamp() * 1000)
        self.close_ms = (now // HOUR_MS) * HOUR_MS
        self.addCleanup(self.tmp.cleanup)

    def tearDown(self):
        if self._previous is None:
            os.environ.pop("OPENTERMINAL_EVIDENCE_DIR", None)
        else:
            os.environ["OPENTERMINAL_EVIDENCE_DIR"] = self._previous

    def path(self, name):
        return os.path.join(self.evidence, name)


class DownsamplingTest(EvidenceCase):
    """Criterion 1 — what the series keeps, and what it deliberately drops."""

    def test_keeps_threshold_contracts_near_expiry_only(self):
        ticker = ticker_for(self.close_ms, "64000.00")
        band = "KXBTC-%s-B64050" % ticker.split("-")[1]
        fifteen = "KXBTC15M-%s30-00" % ticker.split("-")[1]
        far = ticker_for(self.close_ms + 4 * HOUR_MS, "64000.00")
        base = self.close_ms - 30 * 60_000
        write_jsonl(self.path(series.SOURCE_TICKERS), [
            quote_row(ticker, base, "0.4500", "0.4700"),
            quote_row(band, base, "0.4500", "0.4700"),
            quote_row(fifteen, base, "0.4500", "0.4700"),
            # In band by ticker, but this row is 4 hours before its own close.
            quote_row(far, base, "0.4500", "0.4700"),
        ])
        series.compact()
        rows = read_series_rows(series.series_dir())
        self.assertEqual([row["market_ticker"] for row in rows], [ticker])

    def test_top_of_book_on_change_not_on_every_tick(self):
        ticker = ticker_for(self.close_ms, "64000.00")
        base = self.close_ms - 30 * 60_000
        rows = []
        # Ten repeats of one quote inside a heartbeat interval, then a change.
        for i in range(10):
            rows.append(quote_row(ticker, base + i * 500, "0.4500", "0.4700",
                                  bid_size="%d.00" % (100 + i)))
        rows.append(quote_row(ticker, base + 5_000, "0.4600", "0.4800"))
        write_jsonl(self.path(series.SOURCE_TICKERS), rows)
        series.compact()
        retained = read_series_rows(series.series_dir())
        self.assertEqual([row["series_row"] for row in retained],
                         ["change", "change"])
        self.assertEqual([row["yes_bid_dollars"] for row in retained],
                         ["0.4500", "0.4600"])

    def test_heartbeat_keeps_an_unchanged_book_observable(self):
        # An unchanged book must still surface within HEARTBEAT_MS, or q1's
        # 15 s staleness rule would read a live quote as missing.
        ticker = ticker_for(self.close_ms, "64000.00")
        base = self.close_ms - 30 * 60_000
        rows = [quote_row(ticker, base + i * 1_000, "0.4500", "0.4700")
                for i in range(60)]
        write_jsonl(self.path(series.SOURCE_TICKERS), rows)
        series.compact()
        retained = read_series_rows(series.series_dir())
        stamps = [row["ts_ms"] for row in retained]
        self.assertGreater(len(stamps), 1)
        self.assertEqual(retained[0]["series_row"], "change")
        self.assertTrue(all(row["series_row"] == "heartbeat" for row in retained[1:]))
        for earlier, later in zip(stamps, stamps[1:]):
            self.assertLessEqual(later - earlier, series.HEARTBEAT_MS)

    def test_brti_capped_at_one_sample_per_second(self):
        base = self.close_ms - 30 * 60_000
        write_jsonl(self.path(series.SOURCE_BRTI),
                    [brti_row(base + i * 200, 64000.0 + i) for i in range(50)])
        series.compact()
        retained = read_series_rows(series.series_dir())
        self.assertEqual(len(retained), 10)
        for earlier, later in zip(retained, retained[1:]):
            self.assertGreaterEqual(later["time"] - earlier["time"],
                                    series.BRTI_MIN_INTERVAL_MS)

    def test_heartbeat_is_shorter_than_q1_staleness_bound(self):
        # The reciprocal constraint that makes "q1 runs unchanged" true.
        self.assertLess(series.HEARTBEAT_MS, q1_quote_lag.QUOTE_STALENESS_MS)

    def test_ticker_parsing_agrees_with_the_analysis(self):
        strikes = ["64000.00", "57299.99", "66499.99"]
        for hours in (0, 5, 19):
            for strike in strikes:
                ticker = ticker_for(self.close_ms + hours * HOUR_MS, strike)
                mine = series.parse_threshold_ticker(ticker)
                theirs = common.parse_ticker(ticker)
                self.assertIsNotNone(mine)
                self.assertEqual(mine["close_ms"], theirs["close_ms"])
                self.assertEqual(mine["strike"], theirs["strike"])
        # And both refuse the same non-threshold shapes.
        self.assertIsNone(series.parse_threshold_ticker("KXBTC-26JUL2719-B64450"))
        self.assertIsNone(series.parse_threshold_ticker("garbage"))


class HonestyTest(EvidenceCase):
    """Criterion 4 — a one-sided book is retained as one-sided."""

    def test_one_sided_book_is_never_completed_to_a_midpoint(self):
        ticker = ticker_for(self.close_ms, "64000.00")
        base = self.close_ms - 30 * 60_000
        write_jsonl(self.path(series.SOURCE_TICKERS), [
            quote_row(ticker, base, "0.0000", "0.0100", bid_size="0.00"),
            quote_row(ticker, base + 20_000, "0.9900", "1.0000", ask_size="0.00"),
            quote_row(ticker, base + 40_000, "0.0000", "1.0000",
                      bid_size="0.00", ask_size="0.00"),
            quote_row(ticker, base + 60_000, "0.4500", "0.4700"),
        ])
        series.compact()
        retained = read_series_rows(series.series_dir())
        self.assertEqual([row["book_sided"] for row in retained],
                         ["ask_only", "bid_only", "empty", "both"])
        # Recorded verbatim: the missing side keeps the value the source wrote.
        self.assertEqual(retained[0]["yes_bid_dollars"], "0.0000")
        self.assertEqual(retained[1]["yes_ask_dollars"], "1.0000")
        # And no row anywhere carries a completed price.
        for row in retained:
            self.assertNotIn("mid", row)
            self.assertNotIn("yes_mid_dollars", row)
            self.assertNotIn("no_bid_dollars", row)

    def test_sources_are_opened_read_only(self):
        ticker = ticker_for(self.close_ms, "64000.00")
        base = self.close_ms - 30 * 60_000
        source = self.path(series.SOURCE_TICKERS)
        write_jsonl(source, [quote_row(ticker, base, "0.4500", "0.4700")])
        with open(source, "rb") as handle:
            before = (handle.read(), os.path.getmtime(source))
        series.compact()
        with open(source, "rb") as handle:
            after = (handle.read(), os.path.getmtime(source))
        self.assertEqual(before, after)
        # Everything this job writes lives inside the series directory.
        written = set(os.listdir(self.evidence)) - {series.SOURCE_TICKERS}
        self.assertEqual(written, {series.SERIES_DIR_NAME})

    def test_a_rotated_away_span_is_recorded_as_a_gap(self):
        ticker = ticker_for(self.close_ms, "64000.00")
        base = self.close_ms - 50 * 60_000
        source = self.path(series.SOURCE_TICKERS)
        write_jsonl(source, [quote_row(ticker, base, "0.4500", "0.4700")])
        series.compact()
        self.assertEqual(series.read_manifest()["gaps"], [])
        # The source rotated: its oldest surviving row is now 20 minutes after
        # the cursor, and those 20 minutes were never retained.
        write_jsonl(source, [quote_row(ticker, base + 20 * 60_000,
                                       "0.4600", "0.4800")])
        series.compact()
        gaps = series.read_manifest()["gaps"]
        self.assertEqual(len(gaps), 1)
        self.assertEqual(gaps[0]["stream"], series.SOURCE_TICKERS)
        self.assertEqual(gaps[0]["from_ts_ms"], base)
        self.assertAlmostEqual(gaps[0]["gap_seconds"], 1200.0, places=3)


class RetentionTest(EvidenceCase):
    """Criterion 2 — time-bounded retention, stated in the file."""

    def _seed_day(self, ts_ms, ticker=None):
        ticker = ticker or ticker_for(self.close_ms, "64000.00")
        write_jsonl(self.path(series.SOURCE_TICKERS),
                    [quote_row(ticker, ts_ms, "0.4500", "0.4700")])
        series.compact(now=ts_ms)

    def test_day_file_header_states_the_retention_bound(self):
        self._seed_day(self.close_ms - 30 * 60_000)
        name = series.day_files()[0]
        with open(series.series_path(name), "r", encoding="utf-8") as handle:
            header = json.loads(handle.readline())
        self.assertEqual(header["event"], "kalshi_lag_series_header")
        self.assertEqual(header["retention_days"], series.RETENTION_DAYS)
        # The close-time assumption is stated in the file too: it is a fixed
        # UTC-4 offset that goes wrong when US daylight time ends (issue #176),
        # and a series read months later must be able to see that from its own
        # header rather than from this repository's history.
        self.assertIn("2026-11-01", header["close_time_assumption"])
        self.assertIn("30 days", header["retention"])
        self.assertIn("NEVER rotated by size", header["retention"])

    def test_pruning_is_by_time_and_never_by_size(self):
        old_close = self.close_ms - 40 * 86_400_000
        old_ticker = ticker_for(old_close, "64000.00")
        self._seed_day(old_close - 30 * 60_000, ticker=old_ticker)
        self.assertEqual(len(series.day_files()), 1)
        # A run 40 days later drops the old day whole and keeps the new one.
        state = series.load_state()
        state["cursors"] = {}
        state["markets"] = {}
        series.write_json_atomic(series.series_path(series.STATE_NAME), state)
        self._seed_day(self.close_ms - 30 * 60_000)
        names = series.day_files()
        self.assertEqual(len(names), 1)
        self.assertNotIn(series.day_file_name(old_close - 30 * 60_000),
                         names)                      # the old day is gone
        self.assertIn(series.day_file_name(self.close_ms - 30 * 60_000), names)
        # No size rotation exists anywhere in the series.
        self.assertEqual([n for n in os.listdir(series.series_dir())
                          if n.endswith(".1")], [])
        self.assertIn("time", series.read_manifest()["retention"]["policy"])
        self.assertFalse(series.read_manifest()["retention"]["size_rotation"])


class ManifestTest(EvidenceCase):
    """Criterion 3 — span, row count and cadence without reading the rows."""

    def _seed(self):
        ticker = ticker_for(self.close_ms, "64000.00")
        base = self.close_ms - 45 * 60_000
        quotes = []
        for i in range(30):
            quotes.append(quote_row(ticker, base + i * 20_000,
                                    "0.45%02d" % i, "0.47%02d" % i))
        write_jsonl(self.path(series.SOURCE_TICKERS), quotes)
        write_jsonl(self.path(series.SOURCE_BRTI),
                    [brti_row(base + i * 2_500, 64000.0 + (i % 7))
                     for i in range(600)])
        series.compact()

    def test_manifest_matches_a_full_rescan(self):
        self._seed()
        manifest = series.read_manifest()
        rows = read_series_rows(series.series_dir())
        quotes = [r for r in rows if r["source"] == series.SOURCE_TICKERS]
        brti = [r for r in rows if r["source"] == series.SOURCE_BRTI]
        self.assertEqual(manifest["series"]["rows"], len(rows))
        self.assertEqual(manifest["series"]["quote_rows"], len(quotes))
        self.assertEqual(manifest["series"]["brti_rows"], len(brti))
        self.assertEqual(manifest["series"]["first_ts_ms"],
                         min(int(r.get("ts_ms", r.get("time"))) for r in quotes))
        self.assertEqual(manifest["cadence"]["quote_heartbeat_ms"],
                         series.HEARTBEAT_MS)
        self.assertEqual(manifest["cadence"]["brti_min_interval_ms"],
                         series.BRTI_MIN_INTERVAL_MS)
        self.assertIsNotNone(manifest["series"]["paired_window_hours"])

    def test_manifest_is_small_and_atomically_written(self):
        self._seed()
        # "Queryable without parsing the whole file": the manifest is orders of
        # magnitude smaller than the data it describes, and reading it costs
        # one json.load.
        manifest_bytes = os.path.getsize(series.series_path(series.MANIFEST_NAME))
        data_bytes = sum(os.path.getsize(series.series_path(n))
                         for n in series.day_files())
        self.assertLess(manifest_bytes * 5, data_bytes)
        self.assertEqual([n for n in os.listdir(series.series_dir())
                          if n.endswith(".tmp")], [])

    def test_status_renders_absence_as_absence(self):
        self.assertIn("UNAVAILABLE", series.render_status(None))
        self._seed()
        rendered = series.render_status(series.read_manifest())
        self.assertIn("retention   30 days", rendered)
        self.assertIn("gaps        none recorded", rendered)


class IncrementalTest(EvidenceCase):
    """A second run must not duplicate the first run's rows."""

    def test_compaction_is_incremental_and_idempotent(self):
        ticker = ticker_for(self.close_ms, "64000.00")
        base = self.close_ms - 30 * 60_000
        source = self.path(series.SOURCE_TICKERS)
        write_jsonl(source, [quote_row(ticker, base + i * 5_000,
                                       "0.45%02d" % i, "0.4700")
                             for i in range(5)])
        first = series.compact()
        self.assertEqual(first["rows_written"], 5)
        again = series.compact()
        self.assertEqual(again["rows_written"], 0)
        write_jsonl(source, [quote_row(ticker, base + 25_000, "0.4600", "0.4800")],
                    mode="a")
        third = series.compact()
        self.assertEqual(third["rows_written"], 1)
        rows = read_series_rows(series.series_dir())
        self.assertEqual(len(rows), 6)
        self.assertEqual(len({row["ts_ms"] for row in rows}), 6)


class PricePathTest(EvidenceCase):
    """Downsampling is lossless for the quantity q1 actually measures.

    q1's statistic is sign(spot move) x (mid at t+h - mid at t), read through
    `QuoteBook.at()` — the last quote at or before an instant. Because a row is
    retained on every distinct (bid, ask) transition, that lookup returns the
    same book on the retained series as on the raw log it was distilled from,
    at every instant. What downsampling drops is size churn and repeated
    restatements of an unchanged quote; the heartbeat exists only to keep the
    15 s staleness rule from reading a live book as missing.
    """

    def test_retained_book_reproduces_the_raw_price_path(self):
        ticker = ticker_for(self.close_ms, "64000.00")
        base = self.close_ms - 45 * 60_000
        raw = []
        bid = 0.40
        for i in range(600):                   # 30 minutes at 3 s
            if i % 17 == 0:                    # the price moves now and then
                bid = 0.30 + ((i // 17) % 20) * 0.01
            raw.append(quote_row(ticker, base + i * 3_000, "%.4f" % bid,
                                 "%.4f" % (bid + 0.02),
                                 bid_size="%d.00" % (100 + i)))  # size churns
        write_jsonl(self.path(series.SOURCE_TICKERS), raw)
        series.compact()
        retained = read_series_rows(series.series_dir())
        self.assertLess(len(retained), len(raw) / 2)

        def book(rows, key):
            return sorted((int(r[key]), r["yes_bid_dollars"], r["yes_ask_dollars"])
                          for r in rows)

        def at(sorted_rows, ts_ms):
            best = None
            for row in sorted_rows:
                if row[0] > ts_ms:
                    break
                best = row
            return None if best is None else (best[1], best[2])

        raw_book, retained_book = book(raw, "ts_ms"), book(retained, "ts_ms")
        probes = range(base, base + 30 * 60_000, 1_000)
        self.assertTrue(any(at(raw_book, ts) for ts in probes))
        for ts in probes:
            self.assertEqual(at(retained_book, ts), at(raw_book, ts),
                             "book differs at %d" % ts)


class StateRecoveryTest(EvidenceCase):
    """A lost state file must resume, not silently write a second copy."""

    def _seed(self):
        ticker = ticker_for(self.close_ms, "64000.00")
        base = self.close_ms - 30 * 60_000
        write_jsonl(self.path(series.SOURCE_TICKERS),
                    [quote_row(ticker, base + i * 5_000, "0.45%02d" % i, "0.4700")
                     for i in range(8)])
        write_jsonl(self.path(series.SOURCE_BRTI),
                    [brti_row(base + i * 2_500, 64000.0 + i) for i in range(8)])
        return series.compact()

    def test_lost_state_resumes_from_the_retained_rows(self):
        first = self._seed()
        before = read_series_rows(series.series_dir())
        os.remove(series.series_path(series.STATE_NAME))
        again = series.compact()
        after = read_series_rows(series.series_dir())
        self.assertEqual(again["rows_written"], 0)
        self.assertEqual(len(after), len(before))
        self.assertEqual(len(after), first["rows_written"])
        stamps = [(row["source"], int(row.get("ts_ms", row.get("time"))))
                  for row in after]
        self.assertEqual(len(set(stamps)), len(stamps))
        self.assertEqual(series.read_manifest()["series"]["rows"], len(after))

    def test_unreadable_state_recovers_rather_than_restarting(self):
        self._seed()
        before = len(read_series_rows(series.series_dir()))
        with open(series.series_path(series.STATE_NAME), "w") as handle:
            handle.write("{ this is not json")
        series.compact()
        self.assertEqual(len(read_series_rows(series.series_dir())), before)

    def test_a_newer_schema_is_refused_not_overwritten(self):
        self._seed()
        path = series.series_path(series.STATE_NAME)
        with open(path, encoding="utf-8") as handle:
            state = json.load(handle)
        state["schema"] = series.SCHEMA_VERSION + 1
        series.write_json_atomic(path, state)
        with self.assertRaises(series.StateRefused):
            series.compact()
        self.assertEqual(series.main(["compact"]), 2)
        # And the newer writer's state is still there, untouched.
        with open(path, encoding="utf-8") as handle:
            self.assertEqual(json.load(handle)["schema"],
                             series.SCHEMA_VERSION + 1)


class ReaderTest(EvidenceCase):
    """The retained series is read as the oldest rotation of the same stream."""

    def _split_logs(self):
        """Retain an old span, then 'rotate' the live log to a newer one."""
        ticker = ticker_for(self.close_ms, "64000.00")
        source = self.path(series.SOURCE_TICKERS)
        old = [quote_row(ticker, self.close_ms - 50 * 60_000 + i * 10_000,
                         "0.45%02d" % i, "0.4700") for i in range(20)]
        write_jsonl(source, old)
        series.compact()
        new = [quote_row(ticker, self.close_ms - 20 * 60_000 + i * 10_000,
                         "0.46%02d" % i, "0.4800") for i in range(20)]
        write_jsonl(source, new)                      # the rotation
        return old, new

    def test_retained_rows_extend_the_live_log_without_duplicating_it(self):
        old, new = self._split_logs()
        records, inventory = common.read_jsonl(series.SOURCE_TICKERS)
        stamps = sorted(int(r["ts_ms"]) for r in records)
        self.assertEqual(len(stamps), len(old) + len(new))
        self.assertEqual(stamps[0], old[0]["ts_ms"])
        self.assertEqual(stamps[-1], new[-1]["ts_ms"])
        self.assertEqual(len(set(stamps)), len(stamps))
        self.assertTrue(any(series.SERIES_DIR_NAME in entry["file"]
                            for entry in inventory))

    def test_overlapping_rows_prefer_the_live_log(self):
        self._split_logs()
        # Now the live log ALSO holds the old span (nothing rotated after all):
        # every retained row is superseded and none may be injected twice.
        ticker = ticker_for(self.close_ms, "64000.00")
        write_jsonl(self.path(series.SOURCE_TICKERS),
                    [quote_row(ticker, self.close_ms - 50 * 60_000 + i * 10_000,
                               "0.45%02d" % i, "0.4700") for i in range(20)] +
                    [quote_row(ticker, self.close_ms - 20 * 60_000 + i * 10_000,
                               "0.46%02d" % i, "0.4800") for i in range(20)])
        records, inventory = common.read_jsonl(series.SOURCE_TICKERS)
        self.assertEqual(len(records), 40)
        retained = [e for e in inventory if series.SERIES_DIR_NAME in e["file"]]
        self.assertTrue(retained)
        self.assertEqual(sum(e["rows"] for e in retained), 0)
        self.assertGreater(sum(e["rows_superseded_by_live_log"] for e in retained), 0)

    def test_absent_series_and_absent_live_log_both_read_honestly(self):
        old, new = self._split_logs()
        os.rename(series.series_dir(), self.path("stashed"))
        records, _ = common.read_jsonl(series.SOURCE_TICKERS)
        self.assertEqual(len(records), len(new))       # exactly today's behaviour
        os.rename(self.path("stashed"), series.series_dir())
        os.remove(self.path(series.SOURCE_TICKERS))    # just rotated: log empty
        records, _ = common.read_jsonl(series.SOURCE_TICKERS)
        self.assertEqual(len(records), len(old))       # the series carries it

    def test_streams_the_series_does_not_retain_are_untouched(self):
        write_jsonl(self.path("kalshi-bot-decisions.jsonl"),
                    [{"ts_ms": self.close_ms, "ticker": "x"}])
        records, inventory = common.read_jsonl("kalshi-bot-decisions.jsonl",
                                               rotations=("{name}",))
        self.assertEqual(len(records), 1)
        self.assertEqual([e["file"] for e in inventory],
                         ["kalshi-bot-decisions.jsonl"])


class QuoteLagWindowTest(EvidenceCase):
    """Criterion 5 — q1_quote_lag.py, UNCHANGED, sees the wider window.

    The paired window q1 can measure is the intersection of the BRTI span and
    the quote span, and the quote side is what rotates away in hours. Here the
    live ticker log holds only the last 40 minutes while BRTI holds three
    hours; with the retained series present the quote side reaches back to the
    start, and both the window and the 3-sigma event count grow.
    """

    def _build(self):
        rng = random.Random(20260727)
        start = self.close_ms - 3 * HOUR_MS
        # BRTI at 2.5 s over three hours, with a large jump every ~20 minutes
        # so that the sigma-threshold detector has something to detect.
        brti = []
        value = 64000.0
        ts = start
        while ts <= self.close_ms:
            value *= 1.0 + rng.gauss(0.0, 0.00002)
            if (ts - start) % (20 * 60_000) == 0 and ts > start:
                value *= 1.0 + (0.004 if rng.random() < 0.5 else -0.004)
            brti.append(brti_row(ts, round(value, 2)))
            ts += 2_500
        write_jsonl(self.path(series.SOURCE_BRTI), brti)

        # One hourly threshold contract per hour, quoted every 5 s through its
        # final hour, two-sided and inside q1's contested band.
        quotes = []
        for hour in range(3):
            close = self.close_ms - (2 - hour) * HOUR_MS
            ticker = ticker_for(close, "64000.00")
            ts = close - 59 * 60_000
            step = 0
            while ts < close - 60_000:
                bid = 0.40 + (step % 9) * 0.01
                quotes.append(quote_row(ticker, ts, "%.4f" % bid,
                                        "%.4f" % (bid + 0.02)))
                ts += 5_000
                step += 1
        quotes.sort(key=lambda row: row["ts_ms"])
        return quotes

    def _run_q1(self):
        env = dict(os.environ, OPENTERMINAL_EVIDENCE_DIR=self.evidence)
        proc = subprocess.run([sys.executable, Q1_PATH], env=env,
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return json.loads(proc.stdout)

    def _paired(self, payload):
        return payload["data"]["paired_window_hours"]

    def test_window_and_events_grow_with_the_series(self):
        quotes = self._build()
        source = self.path(series.SOURCE_TICKERS)
        write_jsonl(source, quotes)
        series.compact()
        cutoff = self.close_ms - 40 * 60_000
        write_jsonl(source, [row for row in quotes if row["ts_ms"] >= cutoff])

        with_series = self._run_q1()
        os.rename(series.series_dir(), self.path("stashed"))
        without_series = self._run_q1()
        os.rename(self.path("stashed"), series.series_dir())

        self.assertLess(self._paired(without_series), 0.8)
        self.assertGreater(self._paired(with_series), 2.5)

        def events(payload, sigma):
            for entry in payload["by_threshold"]:
                if entry["threshold_sigma"] == sigma:
                    return entry["events_detected"]
            raise AssertionError("threshold %s missing" % sigma)

        self.assertGreater(events(with_series, 3.0), events(without_series, 3.0))
        self.assertGreater(events(with_series, 2.0), events(without_series, 2.0))
        # The samples the wider window buys are real observations, not repeats.
        narrow_rows = without_series["data"]["quote_rows_two_sided"]
        wide_rows = with_series["data"]["quote_rows_two_sided"]
        self.assertGreater(wide_rows, narrow_rows)
        # And q1 still names every file its numbers came from, including the
        # retained one.
        self.assertTrue(any(series.SERIES_DIR_NAME in entry["file"]
                            for entry in with_series["data"]["quote_files"]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
