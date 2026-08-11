#!/usr/bin/env python3
"""Tests for shared Phase-3 outside-info features."""
import math
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


class MidPriorTiltTest(unittest.TestCase):
    def test_identity_when_private_equals_mid(self):
        p = oif.capped_mid_prior_tilt(0.55, 0.55)
        self.assertAlmostEqual(p, 0.55)

    def test_cap_binds_when_private_extreme(self):
        mid = 0.50
        uncapped_delta = oif.logit(0.99) - oif.logit(mid)
        self.assertGreater(uncapped_delta, oif.TILT_MAX_ABS_LOGIT)
        p = oif.capped_mid_prior_tilt(mid, 0.99)
        expected = oif.sigmoid(oif.logit(mid) + oif.TILT_MAX_ABS_LOGIT)
        self.assertAlmostEqual(p, expected)
        # ~±5¢ near 0.5 for default cap 0.20
        self.assertLess(abs(p - mid), 0.06)
        self.assertGreater(p, mid)

    def test_conflict_private_mid_stays_mid(self):
        # Caller encodes conflict by passing p_private=mid.
        self.assertAlmostEqual(oif.capped_mid_prior_tilt(0.62, 0.62), 0.62)

    def test_edge_probs_do_not_nan(self):
        for mid, priv in ((1e-12, 0.9), (1.0 - 1e-12, 0.1), (0.5, 0.0), (0.5, 1.0)):
            p = oif.capped_mid_prior_tilt(mid, priv)
            self.assertIsNotNone(p)
            self.assertTrue(math.isfinite(p))
            self.assertGreater(p, 0.0)
            self.assertLess(p, 1.0)

    def test_invalid_mid_fail_closed(self):
        self.assertIsNone(oif.capped_mid_prior_tilt(None, 0.6))
        self.assertIsNone(oif.capped_mid_prior_tilt(float("nan"), 0.6))

    def test_invalid_private_returns_mid(self):
        self.assertAlmostEqual(oif.capped_mid_prior_tilt(0.44, None), 0.44)
        self.assertAlmostEqual(oif.capped_mid_prior_tilt(0.44, float("nan")), 0.44)


if __name__ == "__main__":
    unittest.main()
