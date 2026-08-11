"""Scoring diagnostics must be right, because a wrong one is plausible.

Brier alone says WHETHER the model beats the mid, never WHY. These metrics say
why -- and a subtly wrong decomposition would produce numbers that look
reasonable and mislead every reader. That is the failure mode worth testing
hard rather than eyeballing.
"""
import math
import os
import random
import sys
import unittest

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise"))
sys.path.insert(0, SCRIPTS)
import calibrator_scores as cs  # noqa: E402


def brier(pairs):
    return sum((p - (1.0 if y else 0.0)) ** 2 for p, y in pairs) / len(pairs)


class MurphyDecompositionTest(unittest.TestCase):
    def test_identity_is_exact_against_the_binned_brier(self):
        # Murphy's identity assumes each forecast IS its bin representative, so
        # it is exact against the binned Brier. Asserting it against the raw
        # Brier instead would force a loose tolerance that hides real drift.
        rng = random.Random(11)
        for _ in range(25):
            pairs = [(rng.random(), rng.random() < 0.4) for _ in range(400)]
            rel, res, unc = cs.murphy_decomposition(pairs)
            self.assertAlmostEqual(rel - res + unc, cs.binned_brier(pairs), places=9)

    def test_identity_is_exact_against_raw_brier_for_binned_forecasts(self):
        # When forecasts already sit at bin centres, binning is lossless and the
        # identity holds against the RAW Brier with no residual at all.
        rng = random.Random(12)
        centres = [(k + 0.5) / 10.0 for k in range(10)]
        pairs = [(rng.choice(centres), rng.random() < 0.45) for _ in range(600)]
        rel, res, unc = cs.murphy_decomposition(pairs)
        self.assertAlmostEqual(rel - res + unc, brier(pairs), places=9)

    def test_the_binning_residual_stays_small(self):
        # The residual is real but must not be large enough to change a reading.
        rng = random.Random(13)
        pairs = [(rng.random(), rng.random() < 0.4) for _ in range(2000)]
        rel, res, unc = cs.murphy_decomposition(pairs)
        self.assertLess(abs((rel - res + unc) - brier(pairs)), 0.01)

    def test_identity_holds_at_the_extremes(self):
        # All-one-outcome and p pinned to 0/1 are where a binning bug shows up.
        for pairs in ([(0.0, False)] * 50, [(1.0, True)] * 50,
                      [(0.0, True)] * 50, [(1.0, False)] * 50,
                      [(0.5, True), (0.5, False)]):
            rel, res, unc = cs.murphy_decomposition(pairs)
            self.assertAlmostEqual(rel - res + unc, brier(pairs), places=9)

    def test_a_perfect_forecaster(self):
        pairs = [(1.0, True)] * 30 + [(0.0, False)] * 30
        rel, res, unc = cs.murphy_decomposition(pairs)
        self.assertAlmostEqual(rel, 0.0, places=9)      # perfectly calibrated
        self.assertAlmostEqual(res, unc, places=9)      # maximally sharp
        self.assertAlmostEqual(brier(pairs), 0.0, places=9)

    def test_calibrated_but_useless_forecaster(self):
        # Always predicts the base rate: perfectly calibrated, ZERO resolution.
        # This is the "well-calibrated and useless" case and it must be
        # distinguishable from a good forecaster by resolution alone.
        base = 0.4
        pairs = [(base, i < 40) for i in range(100)]
        rel, res, unc = cs.murphy_decomposition(pairs)
        self.assertAlmostEqual(rel, 0.0, places=6)
        self.assertAlmostEqual(res, 0.0, places=6)
        self.assertAlmostEqual(brier(pairs), unc, places=6)

    def test_sharp_but_miscalibrated_is_the_other_failure_mode(self):
        # Confident and directionally right, but overstated: resolution is
        # high AND reliability is bad. The two failure modes must not collapse
        # into one number -- that is the entire point of the decomposition.
        useless = [(0.4, i < 40) for i in range(100)]
        sharp = [(0.95, i < 40) for i in range(40)] + [(0.05, False)] * 60
        r_use, s_use, _ = cs.murphy_decomposition(useless)
        r_sharp, s_sharp, _ = cs.murphy_decomposition(sharp)
        self.assertGreater(s_sharp, s_use)     # sharper
        self.assertGreater(r_sharp, r_use)     # but less reliable

    def test_empty_input_is_none_not_a_crash(self):
        self.assertIsNone(cs.murphy_decomposition([]))


class LogScoreTest(unittest.TestCase):
    def test_perfect_forecast_scores_essentially_zero(self):
        # Not exactly 0: LOG_SCORE_CLIP keeps p away from the asymptote, which
        # costs ~1e-6. That floor is deliberate -- see the clip's comment.
        v = cs.log_score([(1.0, True), (0.0, False)])
        self.assertLess(v, 2.0 * cs.LOG_SCORE_CLIP)
        self.assertGreaterEqual(v, 0.0)

    def test_confident_and_wrong_is_finite_not_inf(self):
        # p=0 on a YES must not produce inf, or one bad row destroys the report.
        v = cs.log_score([(0.0, True)])
        self.assertTrue(math.isfinite(v))
        self.assertGreater(v, 5.0)

    def test_punishes_overconfidence_harder_than_brier(self):
        # The property that makes log score worth adding: moving from a modest
        # error to a confident one costs proportionally MORE in log score than
        # in Brier. That ratio is what exposed this model's overconfidence.
        modest = [(0.6, False)]
        confident = [(0.95, False)]
        brier_ratio = brier(confident) / brier(modest)
        log_ratio = cs.log_score(confident) / cs.log_score(modest)
        self.assertGreater(log_ratio, brier_ratio)

    def test_empty_input_is_none(self):
        self.assertIsNone(cs.log_score([]))


class InformationGainTest(unittest.TestCase):
    def test_identical_forecasts_gain_nothing(self):
        pairs = [(0.7, True), (0.3, False), (0.55, True)]
        self.assertAlmostEqual(cs.information_gain_bits(pairs, pairs), 0.0, places=9)

    def test_a_better_model_gains_positive_bits(self):
        model = [(0.9, True), (0.1, False)]
        mid = [(0.6, True), (0.4, False)]
        self.assertGreater(cs.information_gain_bits(model, mid), 0.0)

    def test_a_worse_model_destroys_information(self):
        # Negative bits is the kill-switch reading: the model is subtracting
        # information from the market price it was given.
        model = [(0.6, True), (0.4, False)]
        mid = [(0.9, True), (0.1, False)]
        self.assertLess(cs.information_gain_bits(model, mid), 0.0)

    def test_mismatched_or_empty_input_is_none(self):
        self.assertIsNone(cs.information_gain_bits([], []))
        self.assertIsNone(cs.information_gain_bits([(0.5, True)], []))


class BootstrapCiTest(unittest.TestCase):
    def test_is_deterministic_for_a_given_seed(self):
        # A report that moves when nothing changed is worse than no report.
        a = [0.2, 0.3, 0.25, 0.4, 0.1] * 20
        b = [0.1, 0.2, 0.15, 0.3, 0.05] * 20
        self.assertEqual(cs.paired_bootstrap_ci(a, b, seed=7),
                         cs.paired_bootstrap_ci(a, b, seed=7))

    def test_a_clear_difference_excludes_zero(self):
        a = [0.5] * 200
        b = [0.1] * 200
        point, lo, hi = cs.paired_bootstrap_ci(a, b, seed=3)
        self.assertAlmostEqual(point, 0.4, places=6)
        self.assertGreater(lo, 0.0)   # CI entirely above zero

    def test_no_difference_contains_zero(self):
        rng = random.Random(5)
        a = [rng.gauss(0.3, 0.05) for _ in range(300)]
        b = list(a)
        point, lo, hi = cs.paired_bootstrap_ci(a, b, seed=3)
        self.assertAlmostEqual(point, 0.0, places=9)
        self.assertLessEqual(lo, 0.0)
        self.assertGreaterEqual(hi, 0.0)

    def test_pairs_are_kept_together(self):
        # Paired, not independent: resampling must draw the SAME index from
        # both lists, or the CI understates a consistent per-contract delta.
        a = [1.0, 2.0, 3.0, 4.0]
        b = [0.9, 1.9, 2.9, 3.9]     # delta is exactly 0.1 everywhere
        point, lo, hi = cs.paired_bootstrap_ci(a, b, seed=1)
        self.assertAlmostEqual(point, 0.1, places=9)
        self.assertAlmostEqual(lo, 0.1, places=9)   # zero variance if paired
        self.assertAlmostEqual(hi, 0.1, places=9)

    def test_degenerate_input_is_none(self):
        self.assertIsNone(cs.paired_bootstrap_ci([], [], seed=1))
        self.assertIsNone(cs.paired_bootstrap_ci([0.1], [0.2, 0.3], seed=1))


if __name__ == "__main__":
    unittest.main()
