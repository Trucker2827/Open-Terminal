"""Regression tests for the netting-aware settlement P&L transform.

Background: /portfolio/settlements rows where BOTH yes and no contracts
traded were previously marked accounting_status=mixed_requires_fill_
reconciliation with realized_pnl=None — permanently blocking the advisor
safety gate on pnl_reconciliation_pending (38 live rows). A yes+no pair
redeems $1 at trade time on Kalshi's single order book, so
    pnl = min(yes,no)*$1 + revenue - yes_cost - no_cost - fees
is exact for every row; one-sided rows reduce to the previous formula.

Run: python3 -m unittest tests.test_prediction_kalshi_settlements -v
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))
import prediction_kalshi as pk  # noqa: E402


def row(**kw):
    base = {
        "yes_count_fp": "0.00", "no_count_fp": "0.00",
        "yes_total_cost_dollars": "0.000000", "no_total_cost_dollars": "0.000000",
        "fee_cost": "0.000000", "revenue": 0,
        "ticker": "KXTEST-1", "event_ticker": "KXTEST",
        "market_result": "yes", "settled_time": "2026-07-20T19:45:09.000000Z",
    }
    base.update(kw)
    return pk.settlement_row_from_api(base)


class TestSettlementPnl(unittest.TestCase):
    def test_one_sided_win_matches_legacy_formula(self):
        r = row(yes_count_fp="215.32", yes_total_cost_dollars="190.5248",
                fee_cost="1.5356", revenue=21532, market_result="yes")
        self.assertEqual(r["side"], "YES")
        self.assertEqual(r["accounting_status"], "exact_one_sided")
        self.assertAlmostEqual(r["realized_pnl"], 215.32 - 190.5248 - 1.5356, places=4)

    def test_one_sided_loss(self):
        r = row(no_count_fp="10.00", no_total_cost_dollars="4.00",
                fee_cost="0.10", revenue=0, market_result="yes")
        self.assertEqual(r["side"], "NO")
        self.assertAlmostEqual(r["realized_pnl"], -4.10, places=4)

    def test_fully_netted_mixed_real_live_row(self):
        # Real live-account row that was pending forever pre-fix.
        r = row(yes_count_fp="141.78", no_count_fp="141.78",
                yes_total_cost_dollars="93.5748", no_total_cost_dollars="113.649",
                fee_cost="3.7801", revenue=0, market_result="no")
        self.assertEqual(r["side"], "MIXED")
        self.assertEqual(r["accounting_status"], "exact_netted_mixed")
        self.assertIsInstance(r["realized_pnl"], float)  # NEVER None any more
        self.assertAlmostEqual(r["realized_pnl"], 141.78 - 93.5748 - 113.649 - 3.7801, places=4)

    def test_partial_mixed_with_winning_remainder(self):
        # 5 yes + 2 no: 2 pairs redeem $2; remaining 3 yes win -> revenue 300c.
        r = row(yes_count_fp="5.00", no_count_fp="2.00",
                yes_total_cost_dollars="2.50", no_total_cost_dollars="1.00",
                fee_cost="0.10", revenue=300, market_result="yes")
        self.assertAlmostEqual(r["realized_pnl"], 2.0 + 3.0 - 2.5 - 1.0 - 0.10, places=4)
        self.assertAlmostEqual(r["netted_redemption"], 2.0, places=4)

    def test_revenue_dollars_preferred_over_cents(self):
        r = row(yes_count_fp="1.00", yes_total_cost_dollars="0.40",
                revenue=999999, revenue_dollars="1.00", market_result="yes")
        self.assertAlmostEqual(r["realized_pnl"], 1.0 - 0.40, places=4)

    def test_no_row_is_ever_pending(self):
        for kw in (dict(yes_count_fp="1", no_count_fp="1"),
                   dict(yes_count_fp="1"), dict(no_count_fp="1"), dict()):
            self.assertIsInstance(row(**kw)["realized_pnl"], float)


if __name__ == "__main__":
    unittest.main()
