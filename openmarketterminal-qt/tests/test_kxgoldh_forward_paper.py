import json
import os
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts", "research"))
import kxgoldh_forward_paper as trial


GOLD_YES = {
    "p_yes_full": 0.80,
    "market_yes_ask": 0.60,
    "market_yes_bid": 0.59,
}
GOLD_THIN = {
    "p_yes_full": 0.62,
    "market_yes_ask": 0.60,
    "market_yes_bid": 0.59,
}
SILVER = {
    "p_yes_full": 0.90,
    "market_yes_ask": 0.50,
    "market_yes_bid": 0.49,
}


def hourly_report(generated_at_ms, predictions, extra_families=None):
    families = {"KXGOLDH": {"predictions": predictions}}
    if extra_families:
        families.update(extra_families)
    return {"generated_at_ms": generated_at_ms, "by_family": families}


class GoldHourlyForwardTest(unittest.TestCase):
    def test_policy_is_gold_hourly_only(self):
        self.assertEqual(trial.POLICY["family"], "KXGOLDH")
        self.assertEqual(trial.POLICY["horizon"], "hourly")
        self.assertEqual(trial.POLICY["minimum_executable_edge"], 0.10)
        self.assertEqual(trial.POLICY["authority"], "paper_research_only_no_order_api")
        self.assertNotIn("15m", json.dumps(trial.POLICY))
        self.assertNotIn("SILVER", json.dumps(trial.POLICY))
        self.assertNotIn("WTI", json.dumps(trial.POLICY))

    def test_source_does_not_import_order_api(self):
        path = os.path.join(ROOT, "scripts", "research", "kxgoldh_forward_paper.py")
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        self.assertNotIn("submit_order", text)
        self.assertNotIn("trade submit", text)
        self.assertNotIn("kxbtc15m_underdog_cashout", text)

    def test_ignores_silver_wti_and_non_gold_tickers(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "state.json")
            trial.run_once(path, now_ms=1000, report=hourly_report(0, {}), outcomes={})
            report = hourly_report(1100, {
                "KXGOLDH-26AUG1717-T1": GOLD_YES,
            }, extra_families={
                "KXSILVERH": {"predictions": {"KXSILVERH-26AUG1717-T1": SILVER}},
                "KXWTIH": {"predictions": {"KXWTIH-26AUG1717-T1": SILVER}},
            })
            trial.run_once(path, now_ms=1200, report=report, outcomes={})
            with open(path, encoding="utf-8") as handle:
                state = json.load(handle)
            tickers = {row["ticker"] for row in state["records"].values()}
            self.assertEqual(tickers, {"KXGOLDH-26AUG1717-T1"})

    def test_skips_edge_below_ten_cents_and_writes_one_leg(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "state.json")
            trial.run_once(path, now_ms=1000, report=hourly_report(0, {}), outcomes={})
            report = hourly_report(1100, {
                "KXGOLDH-26AUG1717-T1": GOLD_THIN,
                "KXGOLDH-26AUG1717-T2": GOLD_YES,
            })
            trial.run_once(path, now_ms=1200, report=report, outcomes={})
            with open(path, encoding="utf-8") as handle:
                state = json.load(handle)
            self.assertEqual(len(state["records"]), 1)
            row = next(iter(state["records"].values()))
            self.assertEqual(row["ticker"], "KXGOLDH-26AUG1717-T2")
            self.assertEqual(row["family"], "KXGOLDH")
            self.assertGreaterEqual(row["executable_edge"], 0.10)

    def test_pre_freeze_reports_never_enter(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "state.json")
            trial.run_once(path, now_ms=5000, report=hourly_report(0, {}), outcomes={})
            report = hourly_report(4000, {"KXGOLDH-26AUG1717-T1": GOLD_YES})
            trial.run_once(path, now_ms=6000, report=report, outcomes={})
            with open(path, encoding="utf-8") as handle:
                state = json.load(handle)
            self.assertEqual(state["records"], {})

    def test_settlement_and_hash_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "state.json")
            trial.run_once(path, now_ms=1000, report=hourly_report(0, {}), outcomes={})
            report = hourly_report(1100, {"KXGOLDH-26AUG1717-T1": GOLD_YES})
            trial.run_once(path, now_ms=1200, report=report, outcomes={})
            out = trial.run_once(
                path, now_ms=1300, report=report,
                outcomes={"KXGOLDH-26AUG1717-T1": True})
            self.assertEqual(out["summary"]["completed"], 1)
            self.assertGreater(out["summary"]["net_pnl"], 0)
            with open(path, encoding="utf-8") as handle:
                state = json.load(handle)
            state["policy_sha256"] = "deadbeef"
            trial.atomic_write(path, state)
            with self.assertRaises(RuntimeError):
                trial.load_state(path, 1400)

    def test_postmortem_classifies_win_and_loss_paths(self):
        won = {
            "ticker": "KXGOLDH-26AUG1717-T2",
            "side": "YES",
            "entry_price": 0.60,
            "entry_ts_ms": 1000,
            "net_pnl": 1.0,
            "status": "completed",
            "won": True,
        }
        lost = {
            "ticker": "KXGOLDH-26AUG1718-T2",
            "side": "YES",
            "entry_price": 0.60,
            "entry_ts_ms": 1000,
            "net_pnl": -1.3,
            "status": "completed",
            "won": False,
        }
        quotes = {
            won["ticker"]: [(1000, 0.59, 0.60), (2000, 0.67, 0.68)],
            lost["ticker"]: [(1000, 0.59, 0.60), (2000, 0.50, 0.51)],
        }
        rows = trial.postmortem_trades([won, lost], quotes)
        self.assertEqual(rows[0]["classification"], "win_had_10pct_bid")
        self.assertEqual(rows[1]["classification"], "loss_never_green")


if __name__ == "__main__":
    unittest.main()
