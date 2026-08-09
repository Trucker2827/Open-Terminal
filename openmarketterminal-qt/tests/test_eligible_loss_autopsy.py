import os
import sys
import unittest

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise"))
sys.path.insert(0, SCRIPTS)
import eligible_loss_autopsy as ela


def _obs(model_bias_mid: float, yes_mid: float, minutes: float) -> dict:
    """Feature dict; model is not used — walk_eligible trains OnlineLogit.

    For unit tests we drive mid + minutes only; walk_forward cold-starts so
    early contracts have model≈0.5. Use many warm-up contracts then one with
    extreme mid to force eligibility / loss patterns via mid vs outcome.
    """
    return {
        "signed_distance_bps": 0.0,
        "per_min_vol_bps": 3.0,
        "sqrt_minutes_left": minutes ** 0.5,
        "required_move_sigma": 1.0,
        "realized_move_bps": 0.0,
        "yes_mid": yes_mid,
        "book_imbalance": 0.0,
        "trade_flow": 0.0,
        "spot_drift": 0.0,
        "news_forecast": 0.0,
        "event_pressure": 0.0,
        "_unused_bias": model_bias_mid,
    }


class BucketsTest(unittest.TestCase):
    def test_edge_buckets(self):
        self.assertEqual(ela._edge_bucket(0.11), "0.10–0.12")
        self.assertEqual(ela._edge_bucket(0.14), "0.12–0.15")
        self.assertEqual(ela._edge_bucket(0.18), "0.15–0.20")
        self.assertEqual(ela._edge_bucket(0.25), "≥0.20")

    def test_minutes_buckets(self):
        self.assertEqual(ela._minutes_bucket(10), "<15m")
        self.assertEqual(ela._minutes_bucket(20), "15–30m")
        self.assertEqual(ela._minutes_bucket(45), "30–60m")
        self.assertEqual(ela._minutes_bucket(90), "≥60m")


class WalkEligibleTest(unittest.TestCase):
    def test_ineligible_only_contracts_are_skipped(self):
        # Cold model ~0.5; mid 0.48 → |edge|<0.10 → not eligible.
        record = [
            {"observations": [_obs(0.0, 0.48, 40.0)], "outcome": False},
            {"observations": [_obs(0.0, 0.49, 40.0)], "outcome": True},
        ]
        rows = ela.walk_eligible(record)
        self.assertEqual(rows, [])

    def test_eligible_when_mid_far_from_cold_model(self):
        # Cold predict ≈ 0.5; mid 0.30 → edge 0.20 eligible.
        record = [{"observations": [_obs(0.0, 0.30, 25.0)], "outcome": False}]
        rows = ela.walk_eligible(record)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["eligible_obs"], 1)
        self.assertEqual(rows[0]["thesis"], "YES")  # 0.5 > 0.30
        self.assertFalse(rows[0]["thesis_correct"])  # outcome False
        self.assertIn(rows[0]["minutes_bucket"], ("15–30m",))

    def test_summarize_counts_losses(self):
        rows = [
            {
                "model_loses": True,
                "model_brier": 0.4,
                "mid_brier": 0.1,
                "brier_delta": 0.3,
                "mean_edge": 0.11,
                "mean_minutes_left": 20.0,
                "thesis": "YES",
                "thesis_correct": False,
                "edge_bucket": "0.10–0.12",
                "minutes_bucket": "15–30m",
            },
            {
                "model_loses": False,
                "model_brier": 0.05,
                "mid_brier": 0.2,
                "brier_delta": -0.15,
                "mean_edge": 0.18,
                "mean_minutes_left": 50.0,
                "thesis": "NO",
                "thesis_correct": True,
                "edge_bucket": "0.15–0.20",
                "minutes_bucket": "30–60m",
            },
        ]
        summary = ela.summarize(rows)
        self.assertEqual(summary["eligible_contracts"], 2)
        self.assertEqual(summary["model_loses_n"], 1)
        self.assertEqual(summary["model_beats_n"], 1)
        self.assertEqual(summary["by_thesis"]["YES"]["model_loses"], 1)
        self.assertEqual(summary["by_edge_bucket"]["0.10–0.12"]["model_loses"], 1)


if __name__ == "__main__":
    unittest.main()
