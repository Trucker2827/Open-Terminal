#!/usr/bin/env python3
"""Unit tests for bot_bid_postmortem classification (no live evidence I/O)."""
import importlib.util
import unittest
from pathlib import Path

_SCRIPT = (
    Path(__file__).resolve().parents[1]
    / "scripts"
    / "kalshi_advise"
    / "bot_bid_postmortem.py"
)


def _load():
    spec = importlib.util.spec_from_file_location("bot_bid_postmortem", _SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


class BotBidPostmortemTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = _load()

    def test_joins_bid_and_marks_no_stop_loss_on_loss(self):
        records = [
            {
                "event": "kalshi_bot_decision",
                "action": "bid",
                "position_id": "KXBTC15M-26AUG060730-30@1",
                "ticker": "KXBTC15M-26AUG060730-30",
                "side": "NO",
                "price": 0.08,
                "ts_ms": 1,
                "ts": "t0",
                "quote_style": "cross",
                "calibrated_p": 0.81,
                "market_mid": 0.93,
                "side_edge": 0.11,
                "edge": -0.11,
                "runway_seconds": 120,
                "signal_trusted": True,
                "reason_code": "EDGE_CLEARS_THRESHOLD",
            },
            {
                "event": "kalshi_bot_paper_settlement",
                "position_id": "KXBTC15M-26AUG060730-30@1",
                "ticker": "KXBTC15M-26AUG060730-30",
                "side": "NO",
                "market_result": "YES",
                "won": False,
                "price": 0.08,
                "contracts": 25,
                "stake_usd": 2.0,
                "fee_usd": 0.13,
                "realized_pnl": -2.13,
                "ts_ms": 400_000,
                "ts": "t1",
            },
        ]
        rows = self.m.build_postmortems(records)
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertTrue(row["bid_joined"])
        self.assertEqual(row["family"], "kxbtc15m")
        self.assertTrue(row["cut_loss"]["available"])
        self.assertIn("held_to_settle", row["tags"])
        self.assertEqual(row["gamma_factor"], "possible_near_close_noise")
        self.assertEqual(row["primary_mode"], "cheap_no_crushed_by_yes")

    def test_summary_recommendations_present(self):
        rows = self.m.build_postmortems(
            [
                {
                    "event": "kalshi_bot_paper_settlement",
                    "position_id": "KX-1@1",
                    "ticker": "KXBTCD-26AUG0615-T64000",
                    "side": "YES",
                    "market_result": "YES",
                    "won": True,
                    "price": 0.7,
                    "contracts": 2,
                    "stake_usd": 1.4,
                    "fee_usd": 0.03,
                    "realized_pnl": 0.57,
                    "ts_ms": 2,
                }
            ]
        )
        summary = self.m.summarize(rows, ["kalshi-bot-decisions.jsonl"])
        self.assertEqual(summary["wins"], 1)
        self.assertEqual(summary["losses"], 0)
        self.assertTrue(summary["recommendations"])
        self.assertTrue(summary["cut_loss_policy"]["post_fill_exit_exists"])

    def test_early_exit_cashout_classified(self):
        rows = self.m.build_postmortems(
            [
                {
                    "event": "kalshi_bot_paper_settlement",
                    "position_id": "EXIT@1",
                    "ticker": "KXBTC15M-26AUG070000-00",
                    "side": "YES",
                    "resolution": "early_exit",
                    "close_reason": "LOCK_WIN",
                    "won": None,
                    "price": 0.7,
                    "exit_price": 0.9,
                    "contracts": 2,
                    "stake_usd": 1.4,
                    "fee_usd": 0.08,
                    "realized_pnl": 0.32,
                    "ts_ms": 3,
                    "calibrated_p": 0.9,
                    "market_mid": 0.88,
                    "quote_style": "cross",
                    "runway_seconds": 60,
                }
            ]
        )
        self.assertEqual(len(rows), 1)
        self.assertTrue(rows[0]["early_exit"])
        self.assertEqual(rows[0]["primary_mode"], "early_exit_lock_win")
        self.assertTrue(rows[0]["cut_loss"]["attempted"])
        self.assertEqual(rows[0]["gamma_factor"], "cashout_before_settle")

    def test_settlement_snapshot_marks_stable_wrong_thesis(self):
        rows = self.m.build_postmortems(
            [
                {
                    "event": "kalshi_bot_paper_settlement",
                    "position_id": "NOBID@1",
                    "ticker": "KXBTC15M-26AUG070000-00",
                    "side": "NO",
                    "market_result": "YES",
                    "won": False,
                    "price": 0.1,
                    "contracts": 20,
                    "stake_usd": 2.0,
                    "fee_usd": 0.1,
                    "realized_pnl": -2.1,
                    "ts_ms": 10,
                    "calibrated_p": 0.8,
                    "market_mid": 0.92,
                    "market_mid_at_settle": 0.94,
                    "side_edge": 0.12,
                    "quote_style": "cross",
                    "runway_seconds": 400,
                }
            ]
        )
        self.assertEqual(len(rows), 1)
        self.assertFalse(rows[0]["bid_joined"])
        self.assertTrue(rows[0]["snapshot_on_settlement"])
        self.assertEqual(rows[0]["gamma_factor"], "mid_stable_wrong_thesis")
        self.assertEqual(rows[0]["primary_mode"], "cheap_no_crushed_by_yes")

    def test_mid_path_marks_path_late_move(self):
        rows = self.m.build_postmortems(
            [
                {
                    "event": "kalshi_bot_paper_settlement",
                    "position_id": "PATH@1",
                    "ticker": "KXBTC15M-26AUG070000-00",
                    "side": "YES",
                    "market_result": "NO",
                    "won": False,
                    "price": 0.55,
                    "contracts": 2,
                    "stake_usd": 1.1,
                    "fee_usd": 0.02,
                    "realized_pnl": -1.12,
                    "ts_ms": 10,
                    "market_mid": 0.55,
                    "market_mid_at_settle": 0.40,
                    "runway_seconds": 600,
                    "mid_path": [
                        {"ts_ms": 1, "market_yes_mid": 0.55},
                        {"ts_ms": 2, "market_yes_mid": 0.54},
                        {"ts_ms": 3, "market_yes_mid": 0.53},
                        {"ts_ms": 4, "market_yes_mid": 0.40},
                    ],
                }
            ]
        )
        self.assertEqual(rows[0]["gamma_factor"], "path_late_move")

    def test_since_ms_and_post_gate_filter(self):
        records = [
            {
                "event": "kalshi_bot_decision",
                "action": "bid",
                "position_id": "OLD@1",
                "ticker": "KXBTC15M-A",
                "ts_ms": 100,
            },
            {
                "event": "kalshi_bot_paper_settlement",
                "position_id": "OLD@1",
                "ticker": "KXBTC15M-A",
                "side": "YES",
                "market_result": "YES",
                "won": True,
                "price": 0.5,
                "contracts": 1,
                "stake_usd": 0.5,
                "fee_usd": 0.0,
                "realized_pnl": 0.5,
                "ts_ms": 200,
            },
            {
                "event": "kalshi_bot_decision",
                "action": "bid",
                "position_id": "NEW@1",
                "ticker": "KXBTC15M-B",
                "ts_ms": 1000,
                "fade_ban_lifted": True,
                "fade_ban_lift_reason": "brti_avg60_below_open",
            },
            {
                "event": "kalshi_bot_paper_settlement",
                "position_id": "NEW@1",
                "ticker": "KXBTC15M-B",
                "side": "NO",
                "market_result": "NO",
                "won": True,
                "price": 0.2,
                "contracts": 2,
                "stake_usd": 0.4,
                "fee_usd": 0.0,
                "realized_pnl": 1.6,
                "ts_ms": 1100,
                "mid_path": [
                    {"ts_ms": 1000, "market_yes_mid": 0.9},
                    {"ts_ms": 1050, "market_yes_mid": 0.88},
                ],
            },
        ]
        self.assertEqual(self.m.detect_post_gate_ms(records), 1000)
        rows = self.m.build_postmortems(records, since_ms=1000)
        self.assertEqual(len(rows), 1)
        self.assertTrue(rows[0]["fade_ban_lifted"])
        self.assertEqual(rows[0]["fade_ban_lift_reason"], "brti_avg60_below_open")
        summary = self.m.summarize(
            rows,
            ["ledger"],
            gate_filter={"mode": "post-gate", "since_ms": 1000},
            cohort="current_rules",
        )
        self.assertEqual(summary["cohort"], "current_rules")
        self.assertEqual(summary["measurement"]["fade_ban_lifts"], 1)
        self.assertEqual(summary["measurement"]["settlements_with_mid_path"], 1)

    def test_early_exit_not_counted_as_family_loss(self):
        rows = self.m.build_postmortems(
            [
                {
                    "event": "kalshi_bot_paper_settlement",
                    "position_id": "EXIT@2",
                    "ticker": "KXBTCD-26AUG0615-T64000",
                    "side": "YES",
                    "resolution": "early_exit",
                    "close_reason": "LOCK_WIN",
                    "won": None,
                    "price": 0.7,
                    "exit_price": 0.9,
                    "contracts": 2,
                    "stake_usd": 1.4,
                    "fee_usd": 0.08,
                    "realized_pnl": 0.32,
                    "ts_ms": 3,
                }
            ]
        )
        summary = self.m.summarize(rows, ["ledger"], cohort="current_rules")
        self.assertEqual(summary["wins"], 0)
        self.assertEqual(summary["losses"], 0)
        self.assertEqual(summary["early_exits"], 1)
        fam = summary["by_family"]["threshold"]
        self.assertEqual(fam["wins"], 0)
        self.assertEqual(fam["losses"], 0)
        self.assertEqual(fam["early_exits"], 1)

    def test_current_gate_filter_detects_markers(self):
        records = [
            {"event": "kalshi_bot_paper_settlement", "ts_ms": 50, "won": True},
            {
                "event": "kalshi_bot_paper_settlement",
                "ts_ms": 500,
                "mid_path": [{"ts_ms": 500, "market_yes_mid": 0.5}],
            },
        ]
        since, filt, err = self.m._current_gate_filter(
            records, force_post_gate=False, since_ms=None
        )
        self.assertIsNone(err)
        self.assertEqual(since, 500)
        self.assertEqual(filt["mode"], "post-gate")
        since2, filt2, err2 = self.m._current_gate_filter(
            [{"event": "x", "ts_ms": 1}], force_post_gate=True, since_ms=None
        )
        self.assertIsNotNone(err2)
        self.assertIsNone(since2)
        self.assertIsNone(filt2)


if __name__ == "__main__":
    unittest.main()
