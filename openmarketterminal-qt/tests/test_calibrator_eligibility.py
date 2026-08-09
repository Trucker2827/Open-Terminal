import os
import sys
import unittest

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise"))
sys.path.insert(0, SCRIPTS)
import calibrator_eligibility as ce


class ThresholdTest(unittest.TestCase):
    def test_threshold_mirrors_the_bot_bid_threshold(self):
        # KalshiBotDecision::Config::edge_threshold (KalshiBotDecision.h:236).
        # Nothing can enforce this from Python; the constant is pinned here so a
        # drift is at least visible in a diff.
        self.assertEqual(ce.BET_EDGE_THRESHOLD, 0.10)
        self.assertEqual(ce.MIN_ELIGIBLE_CONTRACTS, 100)

    def test_edge_exactly_at_threshold_is_eligible(self):
        self.assertTrue(ce.is_eligible(0.72, 0.62))
        self.assertFalse(ce.is_eligible(0.719, 0.62))

    def test_eligibility_is_symmetric(self):
        # A model BELOW the mid by the threshold is as bettable as one above:
        # the bot bids the other side.
        self.assertTrue(ce.is_eligible(0.52, 0.62))
        self.assertFalse(ce.is_eligible(0.521, 0.62))

    def test_missing_values_are_not_eligible(self):
        self.assertFalse(ce.is_eligible(None, 0.62))
        self.assertFalse(ce.is_eligible(0.9, None))


class EligiblePairsTest(unittest.TestCase):
    def test_ineligible_observations_are_dropped(self):
        rows = [(0.90, 0.62), (0.63, 0.62), (0.20, 0.62)]
        model, mid = ce.eligible_pairs(rows, True)
        self.assertEqual(model, [(0.90, True), (0.20, True)])
        self.assertEqual(mid, [(0.62, True), (0.62, True)])

    def test_contract_with_no_eligible_observation_yields_nothing(self):
        model, mid = ce.eligible_pairs([(0.63, 0.62), (0.60, 0.62)], False)
        self.assertEqual(model, [])
        self.assertEqual(mid, [])


class AddsValueTest(unittest.TestCase):
    def test_below_the_floor_is_false_even_when_winning(self):
        self.assertFalse(ce.adds_value([0.1] * 99, [0.4] * 99))

    def test_at_the_floor_and_winning_is_true(self):
        self.assertTrue(ce.adds_value([0.1] * 100, [0.4] * 100))

    def test_losing_at_the_floor_is_false(self):
        self.assertFalse(ce.adds_value([0.4] * 100, [0.1] * 100))

    def test_empty_is_false(self):
        self.assertFalse(ce.adds_value([], []))


if __name__ == "__main__":
    unittest.main()
