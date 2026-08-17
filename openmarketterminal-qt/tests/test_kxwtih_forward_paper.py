import json
import os
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts", "research"))
import kxwtih_forward_paper as trial


OIL_YES = {
    "p_yes_full": 0.80,
    "market_yes_ask": 0.60,
    "market_yes_bid": 0.59,
}
OIL_THIN = {
    "p_yes_full": 0.62,
    "market_yes_ask": 0.60,
    "market_yes_bid": 0.59,
}
GOLD = {
    "p_yes_full": 0.90,
    "market_yes_ask": 0.50,
    "market_yes_bid": 0.49,
}


def hourly_report(generated_at_ms, predictions, extra_families=None):
    families = {"KXWTIH": {"predictions": predictions}}
    if extra_families:
        families.update(extra_families)
    return {"generated_at_ms": generated_at_ms, "by_family": families}


class WtiHourlyForwardTest(unittest.TestCase):
    def test_policy_is_wti_hourly_only(self):
        self.assertEqual(trial.POLICY["family"], "KXWTIH")
        self.assertEqual(trial.POLICY["horizon"], "hourly")
        self.assertEqual(trial.POLICY["minimum_executable_edge"], 0.10)
        self.assertEqual(trial.POLICY["authority"], "paper_research_only_no_order_api")
        self.assertNotIn("15m", json.dumps(trial.POLICY))
        self.assertNotIn("GOLD", json.dumps(trial.POLICY))
        self.assertNotIn("SILVER", json.dumps(trial.POLICY))
        self.assertNotEqual(trial.policy_hash(),
                            "3295b765f10c59f492a1fe2dfdfaa9af59d457233d7b376ca25de6c55ae66509")

    def test_source_does_not_import_order_api_or_gold_trial(self):
        path = os.path.join(ROOT, "scripts", "research", "kxwtih_forward_paper.py")
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        self.assertNotIn("submit_order", text)
        self.assertNotIn("trade submit", text)
        self.assertNotIn("kxgoldh_forward_paper", text)
        self.assertNotIn("kxbtc15m_underdog_cashout", text)

    def test_ignores_gold_silver_and_wti_15m(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "state.json")
            trial.run_once(path, now_ms=1000, report=hourly_report(0, {}), outcomes={})
            report = hourly_report(1100, {
                "KXWTIH-26AUG1717-T1": OIL_YES,
            }, extra_families={
                "KXGOLDH": {"predictions": {"KXGOLDH-26AUG1717-T1": GOLD}},
                "KXSILVERH": {"predictions": {"KXSILVERH-26AUG1717-T1": GOLD}},
                "KXWTI15M": {"predictions": {"KXWTI15M-26AUG171730-30": GOLD}},
            })
            trial.run_once(path, now_ms=1200, report=report, outcomes={})
            with open(path, encoding="utf-8") as handle:
                state = json.load(handle)
            tickers = {row["ticker"] for row in state["records"].values()}
            self.assertEqual(tickers, {"KXWTIH-26AUG1717-T1"})

    def test_skips_edge_below_ten_cents_and_writes_one_leg(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "state.json")
            trial.run_once(path, now_ms=1000, report=hourly_report(0, {}), outcomes={})
            report = hourly_report(1100, {
                "KXWTIH-26AUG1717-T1": OIL_THIN,
                "KXWTIH-26AUG1717-T2": OIL_YES,
            })
            trial.run_once(path, now_ms=1200, report=report, outcomes={})
            with open(path, encoding="utf-8") as handle:
                state = json.load(handle)
            self.assertEqual(len(state["records"]), 1)
            row = next(iter(state["records"].values()))
            self.assertEqual(row["ticker"], "KXWTIH-26AUG1717-T2")
            self.assertEqual(row["family"], "KXWTIH")
            self.assertGreaterEqual(row["executable_edge"], 0.10)

    def test_pre_freeze_reports_never_enter(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "state.json")
            trial.run_once(path, now_ms=5000, report=hourly_report(0, {}), outcomes={})
            report = hourly_report(4000, {"KXWTIH-26AUG1717-T1": OIL_YES})
            trial.run_once(path, now_ms=6000, report=report, outcomes={})
            with open(path, encoding="utf-8") as handle:
                state = json.load(handle)
            self.assertEqual(state["records"], {})

    def test_settlement_and_hash_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "state.json")
            trial.run_once(path, now_ms=1000, report=hourly_report(0, {}), outcomes={})
            report = hourly_report(1100, {"KXWTIH-26AUG1717-T1": OIL_YES})
            trial.run_once(path, now_ms=1200, report=report, outcomes={})
            out = trial.run_once(
                path, now_ms=1300, report=report,
                outcomes={"KXWTIH-26AUG1717-T1": True})
            self.assertEqual(out["summary"]["completed"], 1)
            self.assertGreater(out["summary"]["net_pnl"], 0)
            with open(path, encoding="utf-8") as handle:
                state = json.load(handle)
            state["policy_sha256"] = "deadbeef"
            trial.atomic_write(path, state)
            with self.assertRaises(RuntimeError):
                trial.load_state(path, 1400)

    def test_outcomes_keep_family_filter(self):
        hourly = {"by_family": {"KXWTIH": {"resolved_record": [
            {"ticker": "KXWTIH-26AUG1718-T70", "outcome": False, "observations": []},
        ]}}}
        settlements = [
            {"kalshi_market_id": "KXWTIH-26AUG1717-T69.99", "result": "yes"},
            {"kalshi_market_id": "KXGOLDH-26AUG1701-T4389.99", "result": "no"},
        ]
        out = trial.outcomes_from_hourly_state(state=hourly, settlements=settlements)
        self.assertEqual(out["KXWTIH-26AUG1718-T70"], False)
        self.assertEqual(out["KXWTIH-26AUG1717-T69.99"], True)
        self.assertNotIn("KXGOLDH-26AUG1701-T4389.99", out)


if __name__ == "__main__":
    unittest.main()
