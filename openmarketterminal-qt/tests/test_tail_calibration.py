import os
import sys
import unittest

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise"))
sys.path.insert(0, SCRIPTS)
import tail_calibration as tc


_SEQ = [0]


def record(prices, outcome, books=None, ticker=None, spot=None):
    # Distinct spot => distinct proxy event key, so a fixture of N contracts is
    # N independent settlements unless a test deliberately shares one.
    if spot is None:
        _SEQ[0] += 1
        spot = 100.0 + _SEQ[0]
    rec = {
        "observations": [{"yes_mid": p, "spot": spot, "sqrt_minutes_left": 3.0} for p in prices],
        "outcome": outcome,
    }
    if books is not None:
        rec["books"] = books
    if ticker:
        rec["ticker"] = ticker
        rec["event_ticker"] = ticker.rsplit("-", 1)[0]
    return rec


class SignConventionTest(unittest.TestCase):
    """The sign is the whole finding. A flipped one turns 'overpriced' into
    'underpriced' and reads perfectly plausibly either way."""

    def test_a_longshot_that_never_wins_reports_a_NEGATIVE_gap(self):
        rows = tc.rows_from_records([record([0.15], 0) for _ in range(20)])
        out = tc.measure(rows)["0.100-0.200"]
        self.assertLess(out["gap_points"], 0.0)
        self.assertAlmostEqual(out["gap_points"], -15.0, places=6)

    def test_a_favourite_that_always_wins_reports_a_POSITIVE_gap(self):
        rows = tc.rows_from_records([record([0.90], 1) for _ in range(20)])
        out = tc.measure(rows)["0.900-0.950"]
        self.assertGreater(out["gap_points"], 0.0)
        self.assertAlmostEqual(out["gap_points"], 10.0, places=6)


class DegenerateIntervalTest(unittest.TestCase):
    """An all-win bucket yields a bootstrap CI that looks decisive and means
    nothing: the resample cannot draw a loss."""

    def test_an_all_identical_bucket_refuses_to_publish_an_interval(self):
        rows = tc.rows_from_records([record([0.15], 0) for _ in range(20)])
        ci = tc.measure(rows)["0.100-0.200"]["clustered_ci"]
        self.assertIsNone(ci["lo"])
        self.assertIsNone(ci["hi"])
        self.assertIn("degenerate", ci["note"])

    def test_the_null_test_still_answers_where_the_bootstrap_cannot(self):
        # 60 contracts priced 0.15 that all lost: expected 9 wins, saw 0.
        rows = tc.rows_from_records([record([0.15], 0) for _ in range(60)])
        null = tc.measure(rows)["0.100-0.200"]["under_correct_market_null"]
        self.assertEqual(null["observed_wins"], 0)
        self.assertAlmostEqual(null["expected_wins"], 9.0, places=6)
        self.assertLess(null["p_two_sided"], 0.01)

    def test_a_mixed_bucket_does_get_an_interval(self):
        recs = [record([0.15], 1) for _ in range(5)] + [record([0.15], 0) for _ in range(15)]
        ci = tc.measure(tc.rows_from_records(recs))["0.100-0.200"]["clustered_ci"]
        self.assertIsNotNone(ci["lo"])
        self.assertIsNotNone(ci["hi"])


class BookAlignmentTest(unittest.TestCase):
    def test_the_quote_taken_is_the_one_belonging_to_the_scored_observation(self):
        # Three observations; the LAST is the one scored, so the LAST book is
        # the one that must be read.
        rec = record([0.40, 0.30, 0.15], 0,
                     books=[{"market_yes_bid": 0.38, "market_yes_ask": 0.42},
                            {"market_yes_bid": 0.28, "market_yes_ask": 0.32},
                            {"market_yes_bid": 0.11, "market_yes_ask": 0.19}])
        row = tc.rows_from_records([rec])[0]
        self.assertAlmostEqual(row["p"], 0.15)
        self.assertAlmostEqual(row["bid"], 0.11)
        self.assertAlmostEqual(row["ask"], 0.19)

    def test_a_record_with_no_books_reports_no_spread_rather_than_guessing(self):
        row = tc.rows_from_records([record([0.15], 0)])[0]
        self.assertIsNone(row["spread"])
        self.assertIsNone(row["bid"])

    def test_a_short_books_list_is_refused_rather_than_misaligned(self):
        rec = record([0.40, 0.15], 0, books=[{"market_yes_bid": 0.38, "market_yes_ask": 0.42}])
        row = tc.rows_from_records([rec])[0]
        self.assertIsNone(row["bid"], "a book for observation 0 must not be read as observation 1's")


class TradeabilityTest(unittest.TestCase):
    """Nobody trades at the mid. An eleven-point gap against the mid can be
    zero against the price you could actually get."""

    def _rows(self, price, bid, ask, outcome, n):
        return tc.rows_from_records([
            record([price], outcome, books=[{"market_yes_bid": bid, "market_yes_ask": ask}])
            for _ in range(n)])

    def test_an_overpriced_longshot_is_harvested_by_SELLING_at_the_bid(self):
        # mid 0.15, bid 0.13, never wins -> selling at 13c yields +13 points.
        rows = self._rows(0.15, 0.13, 0.17, 0, 20)
        t = tc.measure(rows)["0.100-0.200"]["tradeability"]
        self.assertTrue(t["measurable"])
        self.assertTrue(t["survives_the_spread"])
        self.assertEqual(t["direction"], "sell")
        self.assertAlmostEqual(t["sell_edge_points"], 13.0, places=6)

    def test_a_realized_rate_INSIDE_the_spread_is_reachable_from_neither_side(self):
        # bid 0.10, ask 0.20, and the contract wins 15% of the time. Selling at
        # 10c loses 5 points; buying at 20c loses 5 points. The mid is "fair" and
        # both tradeable prices are worse than fair -- which is what a spread IS.
        wins = tc.rows_from_records([
            record([0.15], 1, books=[{"market_yes_bid": 0.10, "market_yes_ask": 0.20}])
            for _ in range(15)])
        losses = tc.rows_from_records([
            record([0.15], 0, books=[{"market_yes_bid": 0.10, "market_yes_ask": 0.20}])
            for _ in range(85)])
        t = tc.measure(wins + losses)["0.100-0.200"]["tradeability"]
        self.assertFalse(t["survives_the_spread"])
        self.assertEqual(t["direction"], "neither")
        self.assertAlmostEqual(t["sell_edge_points"], -5.0, places=6)
        self.assertAlmostEqual(t["buy_edge_points"], -5.0, places=6)

    def test_a_gap_smaller_than_the_spread_does_not_survive(self):
        # The finding's own failure mode: -4 points against the mid, but a 12
        # point spread means selling at the bid is a LOSS.
        wins = tc.rows_from_records([
            record([0.15], 1, books=[{"market_yes_bid": 0.09, "market_yes_ask": 0.21}])
            for _ in range(11)])
        losses = tc.rows_from_records([
            record([0.15], 0, books=[{"market_yes_bid": 0.09, "market_yes_ask": 0.21}])
            for _ in range(89)])
        b = tc.measure(wins + losses)["0.100-0.200"]
        self.assertAlmostEqual(b["gap_points"], -4.0, places=6)   # looks like an edge
        self.assertFalse(b["tradeability"]["survives_the_spread"])  # is not one

    def test_records_without_quotes_say_so_instead_of_substituting_the_mid(self):
        rows = tc.rows_from_records([record([0.15], 0) for _ in range(20)])
        t = tc.measure(rows)["0.100-0.200"]["tradeability"]
        self.assertFalse(t["measurable"])
        self.assertEqual(t["contracts_with_quotes"], 0)
        self.assertIn("MID", t["reason"])


class ClusteringTest(unittest.TestCase):
    def test_a_recorded_event_ticker_is_preferred_over_the_proxy(self):
        rec = record([0.15], 0, ticker="KXGOLDH-26AUG1017-T4500")
        row = tc.rows_from_records([rec])[0]
        self.assertEqual(row["event"], "KXGOLDH-26AUG1017")
        self.assertTrue(row["event_from_record"])

    def test_contracts_sharing_an_event_are_one_cluster_not_many(self):
        recs = [record([0.15], 0, ticker="KXGOLDH-26AUG1017-T%d" % k) for k in range(10)]
        out = tc.measure(tc.rows_from_records(recs))["0.100-0.200"]
        self.assertEqual(out["contracts"], 10)
        self.assertEqual(out["events"], 1, "ten strikes on one ladder are one price path")


if __name__ == "__main__":
    unittest.main()
