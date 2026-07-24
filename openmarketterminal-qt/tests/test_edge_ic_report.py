import json
import os
import sqlite3
import sys
import tempfile
import time
import unittest

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts"))
sys.path.insert(0, SCRIPTS)
import edge_ic_report as icr


class MathTest(unittest.TestCase):
    def test_pearson_perfect_and_degenerate(self):
        self.assertAlmostEqual(icr.pearson([1, 2, 3], [2, 4, 6]), 1.0)
        self.assertAlmostEqual(icr.pearson([1, 2, 3], [6, 4, 2]), -1.0)
        self.assertIsNone(icr.pearson([1, 1, 1], [1, 2, 3]))   # zero variance
        self.assertIsNone(icr.pearson([1, 2], [1, 2]))          # too few

    def test_spearman_is_rank_based(self):
        # Monotone but nonlinear: spearman 1.0, pearson < 1.
        xs, ys = [1, 2, 3, 4], [1, 10, 100, 1000]
        self.assertAlmostEqual(icr.spearman(xs, ys), 1.0)
        self.assertLess(icr.pearson(xs, ys), 1.0)

    def test_ranks_average_ties(self):
        self.assertEqual(icr.ranks([10, 20, 20, 30]), [1.0, 2.5, 2.5, 4.0])

    def test_parse_realized_move(self):
        reasons = "gate ok | scored: future=65100.00000000 move=42.50000000 breakeven=12.0"
        self.assertAlmostEqual(icr.parse_realized_move(reasons), 42.5)
        self.assertIsNone(icr.parse_realized_move("no score marker"))
        self.assertIsNone(icr.parse_realized_move(None))

    def test_quintiles_partition_all_rows(self):
        predicted = list(range(100))
        outcomes = [i >= 50 for i in range(100)]
        buckets = icr.quintile_win_rates(predicted, outcomes)
        self.assertEqual(sum(b["count"] for b in buckets), 100)
        self.assertEqual(buckets[0]["win_rate"], 0.0)
        self.assertEqual(buckets[-1]["win_rate"], 1.0)


class LoadRowsTest(unittest.TestCase):
    def _make_db(self, path):
        conn = sqlite3.connect(path)
        conn.execute("""CREATE TABLE edge_decision_journal (
            id TEXT, created_at INTEGER, resolved_at INTEGER, venue TEXT,
            raw_edge REAL, confidence REAL, reasons TEXT, outcome INTEGER)""")
        now = int(time.time() * 1000)
        rows = [
            ("a", now, now, "coinbase", 10.0, 0.8, "scored: future=1 move=12.0 breakeven=1", 1),
            ("b", now, now, "coinbase", 2.0, 0.5, "scored: future=1 move=-3.0 breakeven=1", 0),
            ("c", now, now, "kalshi", 9.0, 0.9, "scored: future=1 move=9.0 breakeven=1", 1),   # excluded venue
            ("d", now, now, "coinbase", 5.0, 0.5, "no score marker", 1),                        # unparseable
            ("e", now, None, "coinbase", 5.0, 0.5, "scored: future=1 move=1.0 breakeven=1", 1), # unresolved
            ("f", now - 90 * 24 * 3600 * 1000, now, "coinbase", 5.0, 0.5,
             "scored: future=1 move=1.0 breakeven=1", 1),                                       # too old
            ("g", now, now, "kraken", 4.0, 0.6, "scored: future=1 move=2.0 breakeven=1", 1),
        ]
        conn.executemany("INSERT INTO edge_decision_journal VALUES (?,?,?,?,?,?,?,?)", rows)
        conn.commit()
        conn.close()

    def test_filters_and_parses(self):
        with tempfile.TemporaryDirectory() as tmp:
            db = os.path.join(tmp, "j.db")
            self._make_db(db)
            rows = icr.load_rows(db, days=30)
            self.assertEqual(len(rows), 3)
            self.assertEqual(sorted(r[2] for r in rows), [-3.0, 2.0, 12.0])
            self.assertEqual(sorted(r[4] for r in rows), ["coinbase", "coinbase", "kraken"])
            report = icr.build_report(rows, 0)
            self.assertEqual(report["resolved_decisions"], 3)
            self.assertAlmostEqual(report["overall_win_rate"], 2.0 / 3.0)

    def test_venue_filter(self):
        with tempfile.TemporaryDirectory() as tmp:
            db = os.path.join(tmp, "j.db")
            self._make_db(db)
            self.assertEqual(len(icr.load_rows(db, venue="gemini", days=30)), 0)


class PublishTest(unittest.TestCase):
    ROWS = [
        (10.0, 0.8, 12.0, True, "coinbase"),
        (2.0, 0.5, -3.0, False, "coinbase"),
        (4.0, 0.6, 2.0, True, "kraken"),
    ]

    def test_per_venue_breakdown(self):
        payload = icr.build_publish_payload(self.ROWS, 1234, days=30)
        self.assertEqual(payload["event"], "edge_ic_report")
        self.assertEqual(payload["window_days"], 30)
        self.assertEqual(payload["overall"]["resolved_decisions"], 3)
        self.assertEqual(sorted(payload["venues"]), ["coinbase", "kraken"])
        self.assertEqual(payload["venues"]["coinbase"]["resolved_decisions"], 2)
        self.assertEqual(payload["venues"]["kraken"]["resolved_decisions"], 1)
        self.assertEqual(payload["venues"]["kraken"]["overall_win_rate"], 1.0)

    def test_empty_rows_read_missing_not_fabricated(self):
        payload = icr.build_publish_payload([], 1234, days=30)
        self.assertEqual(payload["overall"]["resolved_decisions"], 0)
        self.assertIsNone(payload["overall"]["ic_pearson_edge_vs_move"])
        self.assertIsNone(payload["overall"]["overall_win_rate"])
        self.assertEqual(payload["venues"], {})

    def test_publish_writes_edge_ic_json_atomically(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "edge-ic.json")
            path, payload = icr.publish(self.ROWS, 1234, days=30, out_path=out)
            self.assertEqual(path, out)
            self.assertFalse(os.path.exists(out + ".tmp"))
            with open(out, encoding="utf-8") as fh:
                on_disk = json.load(fh)
            self.assertEqual(on_disk, payload)
            self.assertEqual(on_disk["generated_at_ms"], 1234)

    def test_publish_default_path_respects_evidence_dir_env(self):
        with tempfile.TemporaryDirectory() as tmp:
            old = os.environ.get("OPENTERMINAL_EVIDENCE_DIR")
            os.environ["OPENTERMINAL_EVIDENCE_DIR"] = tmp
            try:
                path, _ = icr.publish(self.ROWS, 1234, days=30)
            finally:
                if old is None:
                    del os.environ["OPENTERMINAL_EVIDENCE_DIR"]
                else:
                    os.environ["OPENTERMINAL_EVIDENCE_DIR"] = old
            self.assertEqual(path, os.path.join(tmp, "edge-ic.json"))
            self.assertTrue(os.path.exists(path))


if __name__ == "__main__":
    unittest.main()
