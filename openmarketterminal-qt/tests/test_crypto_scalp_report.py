#!/usr/bin/env python3
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from crypto_scalp_report import compute_report


def opportunity(index, call="BUY", cost=2.0):
    ts = index * 20_000
    return {
        "engine_version": "crypto-auto-scalp-v1",
        "opportunity_id": f"op-{index}",
        "symbol": "BTC-USD",
        "reversal_signal": {
            "call": call, "signal_bar_ms": str(ts),
            "reference_price": 100.0, "reason": "CLOSED_BAR_BOS",
        },
        "selected_proposal_index": 0,
        "venue_proposals": [{
            "venue": "kraken_pro", "executable": True,
            "round_trip_cost_bps": cost,
        }],
    }


def outcome_tick(index, price):
    return {
        "symbol": "BTC-USD",
        "received_ts_ms": index * 20_000 + 16_000,
        "price": price,
    }


class CryptoScalpReportTest(unittest.TestCase):
    def test_qualification_requires_oos_net_edge_and_coverage(self):
        decisions = [opportunity(i) for i in range(200)]
        # +20bps gross, 2bps recorded cost => +18bps each.
        ticks = [outcome_tick(i, 100.2) for i in range(200)]
        report = compute_report(decisions, ticks)
        self.assertEqual(report["state"], "QUALIFIED")
        self.assertTrue(report["execution_eligible"])
        self.assertEqual(report["resolved_count"], 200)
        self.assertGreater(report["mean_net_ci95"][0], 0)

    def test_unresolved_and_losing_samples_stay_shadow(self):
        decisions = [opportunity(i) for i in range(200)]
        ticks = [outcome_tick(i, 99.9) for i in range(100)]
        report = compute_report(decisions, ticks)
        self.assertEqual(report["state"], "SHADOW")
        self.assertFalse(report["checks"]["coverage"])
        self.assertFalse(report["checks"]["positive_mean_net_bps"])

    def test_mutated_immutable_opportunity_invalidates_epoch(self):
        first = opportunity(1)
        changed = opportunity(1)
        changed["reversal_signal"]["reference_price"] = 101.0
        report = compute_report([first, changed], [outcome_tick(1, 102)])
        self.assertEqual(report["state"], "INVALID_EPOCH")
        self.assertFalse(report["execution_eligible"])


if __name__ == "__main__":
    unittest.main()
