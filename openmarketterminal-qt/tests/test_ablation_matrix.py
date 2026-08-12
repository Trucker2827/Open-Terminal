#!/usr/bin/env python3
"""Tests for the analysis-only ablation matrix."""
import math
import os
import sys
import unittest

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise"))
sys.path.insert(0, SCRIPTS)
import ablation_matrix as ab


def record(mid, outcome, z=1.0):
    return {"observations": [{"yes_mid": mid, "required_move_sigma": z,
                              "signed_distance_bps": 10.0, "per_min_vol_bps": 3.0,
                              "sqrt_minutes_left": 5.0, "realized_move_bps": 0.0,
                              "book_imbalance": 0.0, "trade_flow": 0.0, "spot_drift": 0.0,
                              "news_forecast": 0.0, "event_pressure": 0.0}],
            "outcome": outcome}


class SliceIndependenceTest(unittest.TestCase):
    """The invariant that makes the whole comparison valid.

    The production `bet_eligible` slice is defined by |p - mid| >= 0.10 — the
    MODEL's own disagreement — so each candidate would be scored on a different
    population and the numbers could not be compared. Every slice here must key
    on the market price or on z, and on nothing a model produced.
    """

    def test_slices_depend_only_on_market_price_and_z(self):
        # Same market price and z => same slices, whatever any model would say.
        self.assertEqual(ab._slices(0.5, 0.1), ab._slices(0.5, 0.1))
        self.assertIn("eval_band", ab._slices(0.5, 3.0))
        self.assertIn("favorites_gt70", ab._slices(0.80, 1.0))
        self.assertIn("longshots_lt30", ab._slices(0.20, 1.0))
        self.assertIn("near_boundary", ab._slices(0.5, 0.3))
        self.assertIn("extreme_z", ab._slices(0.5, -2.5))
        # Mutually exclusive where they must be.
        self.assertNotIn("favorites_gt70", ab._slices(0.20, 1.0))
        self.assertNotIn("near_boundary", ab._slices(0.5, 2.5))

    def test_a_missing_z_still_yields_price_slices(self):
        """A row without z must not silently vanish from the price slices."""
        got = ab._slices(0.80, None)
        self.assertIn("all", got)
        self.assertIn("favorites_gt70", got)
        self.assertNotIn("near_boundary", got)
        self.assertNotIn("extreme_z", got)


class WalkForwardTest(unittest.TestCase):
    def test_the_raw_market_baseline_is_the_mid_itself(self):
        rows = ab.walk_forward([record(0.25, False), record(0.75, True)], None)
        self.assertEqual([r["p"] for r in rows], [0.25, 0.75])

    def test_every_model_scores_the_identical_rows_in_order(self):
        """Same folds for every model, or the paired bootstrap is meaningless."""
        records = [record(0.3, i % 2 == 0) for i in range(20)]
        base = ab.walk_forward(records, None)
        for _, features in ab.MODELS:
            rows = ab.walk_forward(records, features)
            self.assertEqual(len(rows), len(base))
            self.assertEqual([r["outcome"] for r in rows], [r["outcome"] for r in base])
            self.assertEqual([r["mid"] for r in rows], [r["mid"] for r in base])

    def test_a_contract_is_scored_before_it_trains_the_model(self):
        """No look-ahead: the first contract is scored by an untrained model,
        so its prediction cannot depend on its own outcome."""
        records = [record(0.4, True) for _ in range(5)]
        a = ab.walk_forward(records, ab.MARKET)
        flipped = [record(0.4, False)] + records[1:]
        b = ab.walk_forward(flipped, ab.MARKET)
        self.assertAlmostEqual(a[0]["p"], b[0]["p"],
                               msg="the first score must not know its own outcome")

    def test_an_unusable_mid_is_dropped_not_guessed(self):
        rows = ab.walk_forward([record(0.0, True), record(1.0, False), record(0.5, True)], None)
        self.assertEqual(len(rows), 1)


class MetricsTest(unittest.TestCase):
    def test_a_perfect_forecaster_scores_zero_brier(self):
        rows = [{"p": 1.0 - 1e-9, "outcome": 1.0}, {"p": 1e-9, "outcome": 0.0}]
        self.assertAlmostEqual(ab.brier(rows), 0.0, places=6)

    def test_calibration_error_is_zero_when_frequency_matches_confidence(self):
        # Ten rows at p=0.5, five of which win: perfectly calibrated.
        rows = [{"p": 0.5, "outcome": 1.0}] * 5 + [{"p": 0.5, "outcome": 0.0}] * 5
        self.assertAlmostEqual(ab.calibration_error(rows), 0.0, places=6)

    def test_calibration_error_is_reported_in_probability_points(self):
        # Claims 0.9, wins half the time: 40 points out.
        rows = [{"p": 0.9, "outcome": 1.0}] * 5 + [{"p": 0.9, "outcome": 0.0}] * 5
        self.assertAlmostEqual(ab.calibration_error(rows), 40.0, places=6)

    def test_identical_models_produce_an_interval_containing_zero(self):
        rows = [{"p": 0.4, "outcome": float(i % 2), "event": ("E", i)} for i in range(60)]
        ci = ab.paired_bootstrap(rows, [dict(r) for r in rows])
        self.assertAlmostEqual(ci["delta"], 0.0, places=9)
        self.assertLessEqual(ci["lo"], 0.0)
        self.assertGreaterEqual(ci["hi"], 0.0)

    def test_the_sign_convention_is_positive_means_improvement(self):
        """delta = Brier(baseline) - Brier(model).

        A rule written in one sign and computed in the other is a contradiction
        that survives every review, so the direction is pinned here rather than
        left to prose.
        """
        good = [{"p": 0.95, "outcome": 1.0, "event": ("E", i)} for i in range(60)]
        bad = [{"p": 0.05, "outcome": 1.0, "event": ("E", i)} for i in range(60)]
        better = ab.paired_bootstrap(good, bad)      # model good, baseline bad
        self.assertGreater(better["delta"], 0.0, "an improvement must read POSITIVE")
        self.assertGreater(better["lo"], 0.0, "and promotion needs the whole interval above zero")
        worse = ab.paired_bootstrap(bad, good)
        self.assertLess(worse["delta"], 0.0, "a deterioration must read NEGATIVE")
        self.assertLess(worse["hi"], 0.0)

    def test_the_bootstrap_resamples_events_not_contracts(self):
        """Contracts in one event are strikes on ONE price path. Resampling
        them individually treats correlated evidence as independent and reports
        intervals several times too narrow."""
        # Ten events, every contract inside an event agreeing.
        rows, base = [], []
        for e in range(10):
            right = e % 2 == 0
            for _ in range(20):
                rows.append({"p": 0.9 if right else 0.1, "outcome": 1.0, "event": ("E", e)})
                base.append({"p": 0.5, "outcome": 1.0, "event": ("E", e)})
        clustered = ab.paired_bootstrap(rows, base)
        flat_rows = [dict(r, event=("X", i)) for i, r in enumerate(rows)]
        flat_base = [dict(r, event=("X", i)) for i, r in enumerate(base)]
        naive = ab.paired_bootstrap(flat_rows, flat_base)
        self.assertEqual(clustered["n_events"], 10)
        self.assertGreater(clustered["hi"] - clustered["lo"],
                           2.0 * (naive["hi"] - naive["lo"]),
                           "clustering correlated evidence must WIDEN the interval")

    def test_too_few_events_is_unbounded_not_a_tight_interval(self):
        """One event cannot bound anything. Returning a narrow interval from a
        single cluster would be the most dangerous possible output."""
        rows = [{"p": 0.9, "outcome": 1.0, "event": ("E", 0)} for _ in range(30)]
        base = [{"p": 0.5, "outcome": 1.0, "event": ("E", 0)} for _ in range(30)]
        ci = ab.paired_bootstrap(rows, base)
        self.assertEqual(ci["n_events"], 1)
        self.assertIsNone(ci["lo"])
        self.assertIn("too few", ci["note"])

    def test_the_bootstrap_is_deterministic(self):
        rows = [{"p": 0.4, "outcome": float(i % 3 == 0), "event": ("E", i // 5)} for i in range(50)]
        base = [{"p": 0.5, "outcome": float(i % 3 == 0), "event": ("E", i // 5)} for i in range(50)]
        self.assertEqual(ab.paired_bootstrap(rows, base), ab.paired_bootstrap(rows, base))


class ReportShapeTest(unittest.TestCase):
    def test_too_few_contracts_is_SKIPPED_not_absent(self):
        """'Too few to measure' and 'not measured' must not look identical.

        After the per-family splits every family restarts at zero, so most will
        be in this state for weeks. Silently omitting them would read as
        'nothing to see'.
        """
        report = ab.build_report(now_ms=1)
        for family, result in report["families"].items():
            self.assertTrue("skipped" in result or "slices" in result, family)

    def test_the_report_declares_itself_analysis_only(self):
        report = ab.build_report(now_ms=1)
        self.assertTrue(report["analysis_only"])
        self.assertTrue(report["advisory_only"])
        self.assertIn("market_raw", report["models"])


if __name__ == "__main__":
    unittest.main()
