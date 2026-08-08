#!/usr/bin/env python3
"""Tests for shared Phase-3 outside-info features."""
import os
import sys
import unittest

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise"))
sys.path.insert(0, SCRIPTS)
import outside_info_features as oif


class SessionVolTest(unittest.TestCase):
    def test_vol_regime_buckets(self):
        self.assertEqual(oif.vol_regime(1.0), "quiet")
        self.assertEqual(oif.vol_regime(6.0), "normal")
        self.assertEqual(oif.vol_regime(20.0), "elevated")

    def test_session_regime_rth_weekday(self):
        # 2026-08-07 10:00 America/New_York (Friday) → rth
        import datetime, zoneinfo
        eastern = zoneinfo.ZoneInfo("America/New_York")
        dt = datetime.datetime(2026, 8, 7, 10, 0, tzinfo=eastern)
        ms = int(dt.timestamp() * 1000)
        self.assertEqual(oif.session_regime_at(ms), "rth")


class FuturesTapeTest(unittest.TestCase):
    def test_tape_confirms_same_direction(self):
        flags = oif.futures_tape_flags(4360.0, 4350.0, yahoo_change_bps=5.0)
        self.assertTrue(flags["tape_confirms"])
        self.assertFalse(flags["tape_conflicts"])

    def test_tape_conflicts_opposite(self):
        flags = oif.futures_tape_flags(4360.0, 4350.0, yahoo_change_bps=-5.0)
        self.assertTrue(flags["tape_conflicts"])
        self.assertFalse(flags["tape_confirms"])

    def test_series_change_bps(self):
        now = 1_000_000
        series = [(now - 180_000, 100.0), (now - 60_000, 100.1), (now, 100.2)]
        bps = oif.series_change_bps(series, now, lookback_ms=180_000)
        self.assertIsNotNone(bps)
        self.assertGreater(bps, 0.0)


class AblationTest(unittest.TestCase):
    def test_tape_confirm_near_close_clamps_without_confirm(self):
        p = oif.ablation_tape_confirm_near_close(
            0.8, 0.55, seconds_left=60, tape_confirms=False)
        self.assertAlmostEqual(p, 0.55)

    def test_tape_confirm_early_keeps_physics(self):
        p = oif.ablation_tape_confirm_near_close(
            0.8, 0.55, seconds_left=600, tape_confirms=False)
        self.assertAlmostEqual(p, 0.8)

    def test_vol_regime_quiet_clamps(self):
        p = oif.ablation_vol_regime_confirm(0.8, 0.55, per_min_vol_bps=1.0)
        self.assertAlmostEqual(p, 0.55)

    def test_vol_regime_normal_keeps_physics(self):
        p = oif.ablation_vol_regime_confirm(0.8, 0.55, per_min_vol_bps=8.0)
        self.assertAlmostEqual(p, 0.8)

    def test_select_best_trusted(self):
        board = {
            "physics": {"brier": 0.20, "beats_mid": False},
            "physics_vol_regime_confirm": {"brier": 0.10, "beats_mid": True},
            "physics_tape_confirm_near_close": {"brier": 0.09, "beats_mid": True},
        }
        self.assertEqual(
            oif.select_best_trusted(
                board, ("physics", "physics_tape_confirm_near_close",
                        "physics_vol_regime_confirm")),
            "physics_tape_confirm_near_close",
        )


if __name__ == "__main__":
    unittest.main()
