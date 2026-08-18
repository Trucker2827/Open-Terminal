import json
import os
import sqlite3
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts", "research"))
import btc1h_chronos_shadow as trial


def make_db(path):
    connection = sqlite3.connect(path)
    connection.executescript("""
      CREATE TABLE edge_decision_journal (
        id TEXT, created_at INTEGER, direction TEXT, side TEXT, gate TEXT,
        model_probability REAL, confidence REAL, features_json TEXT,
        source TEXT, symbol TEXT, horizon TEXT);
      CREATE TABLE edge_prediction_raw_ticks (
        id TEXT, symbol TEXT, source TEXT, price REAL,
        exchange_ts INTEGER, received_ts INTEGER);
    """)
    return connection


def signal(connection, ident, ts, source, horizon, direction):
    connection.execute(
        "INSERT INTO edge_decision_journal VALUES (?,?,?,?,?,?,?,?,?,?,?)",
        (ident, ts, direction, direction, "pass", .6 if direction == "up" else .4,
         .6, "{}", source, "BTC-USD", horizon))


class BtcChronosShadowTest(unittest.TestCase):
    def test_policy_is_shadow_only_and_hash_locked(self):
        self.assertEqual(trial.POLICY["authority"], "paper_research_only_no_order_api")
        self.assertEqual(trial.POLICY["horizon_ms"], 3_600_000)
        self.assertEqual(trial.POLICY["cohorts"],
                         ["chronos_alone", "control_alone", "agreement", "conflict"])
        with open(os.path.join(ROOT, "scripts", "research", "btc1h_chronos_shadow.py"),
                  encoding="utf-8") as handle:
            source = handle.read()
        self.assertNotIn("submit_order", source)
        self.assertNotIn("KalshiRestClient", source)

    def test_pairs_once_and_settles_all_four_views(self):
        with tempfile.TemporaryDirectory() as directory:
            db = os.path.join(directory, "test.db")
            state = os.path.join(directory, "state.json")
            connection = make_db(db)
            base = 7_200_000
            signal(connection, "c1", base + 10_000, "chronos2-forecast", "1h", "up")
            signal(connection, "e1", base + 20_000, "edge crypto-recommend", "3600s", "up")
            signal(connection, "c2", base + 3_610_000, "chronos2-forecast", "1h", "down")
            signal(connection, "e2", base + 3_620_000, "edge crypto-recommend", "3600s", "up")
            connection.executemany("INSERT INTO edge_prediction_raw_ticks VALUES (?,?,?,?,?,?)", [
                ("t1", "BTC-USD", "test", 100.0, base + 20_000, base + 20_000),
                ("t2", "BTC-USD", "test", 102.0, base + 3_620_000, base + 3_620_000),
                ("t3", "BTC-USD", "test", 99.0, base + 7_220_000, base + 7_220_000),
            ])
            connection.commit(); connection.close()
            trial.run_once(state, db, now_ms=base - 1)
            out = trial.run_once(state, db, now_ms=base + 8_000_000)
            cohorts = out["summary"]["cohorts"]
            self.assertEqual(cohorts["chronos_alone"]["completed"], 2)
            self.assertEqual(cohorts["control_alone"]["completed"], 2)
            self.assertEqual(cohorts["agreement"]["completed"], 1)
            self.assertEqual(cohorts["conflict"]["completed"], 1)
            with open(state, encoding="utf-8") as handle:
                saved = json.load(handle)
            self.assertIn("postmortem", saved["records"]["2"])

    def test_raw_btc_tick_symbol_adapter_and_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            db = os.path.join(directory, "test.db")
            state = os.path.join(directory, "state.json")
            connection = make_db(db)
            base = 7_200_000
            signal(connection, "c1", base + 10_000, "chronos2-forecast", "1h", "up")
            signal(connection, "e1", base + 20_000, "edge crypto-recommend", "3600s", "up")
            connection.executemany("INSERT INTO edge_prediction_raw_ticks VALUES (?,?,?,?,?,?)", [
                ("t1", "BTC", "test", 100.0, base + 20_000, base + 20_000),
                ("t2", "BTC", "test", 101.0, base + 3_620_000, base + 3_620_000),
            ])
            connection.commit(); connection.close()
            trial.run_once(state, db, now_ms=base - 1)
            out = trial.run_once(state, db, now_ms=base + 4_000_000)
            self.assertEqual(out["summary"]["paired_completed"], 1)
            self.assertEqual(out["summary"]["diagnostics"]["last_run"]["opened"], 1)

    def test_pre_freeze_rows_do_not_enter_and_tampering_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            db = os.path.join(directory, "test.db")
            state_path = os.path.join(directory, "state.json")
            connection = make_db(db)
            signal(connection, "old-c", 1000, "chronos2-forecast", "1h", "up")
            signal(connection, "old-e", 1001, "edge crypto-recommend", "3600s", "up")
            connection.commit(); connection.close()
            trial.run_once(state_path, db, now_ms=2000)
            trial.run_once(state_path, db, now_ms=9_000_000)
            with open(state_path, encoding="utf-8") as handle:
                state = json.load(handle)
            self.assertEqual(state["records"], {})
            state["policy_sha256"] = "changed"
            trial.atomic_write(state_path, state)
            with self.assertRaises(RuntimeError):
                trial.load_state(state_path, 9_000_001)


if __name__ == "__main__":
    unittest.main()
