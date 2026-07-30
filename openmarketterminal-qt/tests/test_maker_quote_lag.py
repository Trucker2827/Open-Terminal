import os, sys, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts", "research"))
import maker_quote_lag as mql  # noqa: E402

class FillModelTest(unittest.TestCase):
    def test_optimistic_is_the_first_through_trade(self):
        hits = [(1000, 5.0), (2000, 10.0)]
        self.assertEqual(mql.maker_fill(ahead_size=100.0, hits=hits)["optimistic"], 1000)

    def test_pessimistic_waits_for_ahead_size_to_clear(self):
        # ahead of us: 12 contracts. Hits 5,10,3 -> cumulative clears 12 at the 2nd hit.
        hits = [(1000, 5.0), (2000, 10.0), (3000, 3.0)]
        r = mql.maker_fill(ahead_size=12.0, hits=hits)
        self.assertEqual(r["optimistic"], 1000)
        self.assertEqual(r["pessimistic"], 2000)   # 5+10=15 > 12

    def test_never_fills_pessimistic_when_ahead_size_never_clears(self):
        hits = [(1000, 2.0), (2000, 1.0)]           # only 3 total, ahead is 50
        r = mql.maker_fill(ahead_size=50.0, hits=hits)
        self.assertEqual(r["optimistic"], 1000)
        self.assertIsNone(r["pessimistic"])

    def test_no_hits_fills_neither(self):
        r = mql.maker_fill(ahead_size=0.0, hits=[])
        self.assertIsNone(r["optimistic"])
        self.assertIsNone(r["pessimistic"])

    def test_exact_clear_is_a_non_fill_pessimistic(self):
        # ahead=15; hits sum to exactly 15 -> queue clears but our order is NOT
        # reached (strict >). A '>=' regression would fake a fill here.
        hits = [(1000, 5.0), (2000, 10.0)]
        r = mql.maker_fill(ahead_size=15.0, hits=hits)
        self.assertEqual(r["optimistic"], 1000)
        self.assertIsNone(r["pessimistic"])


class HitsTest(unittest.TestCase):
    def test_hits_are_taker_sells_through_our_price_after_join(self):
        # resting a YES buy at 0.40; a taker sells YES at 0.39 (through) after join.
        trades = [(500, 0.42, 3.0, True),    # before join -> excluded
                  (1500, 0.39, 5.0, True),   # taker sold YES at 0.39 <= 0.40 -> hit
                  (1600, 0.41, 9.0, True),   # 0.41 > 0.40 -> not a hit
                  (1700, 0.38, 4.0, False)]  # taker BOUGHT yes -> not a hit
        hits = mql.hits_against("YES", 0.40, join_ts=1000, trades_for_ticker=trades)
        self.assertEqual(hits, [(1500, 5.0)])

    def test_no_side_hit_uses_the_complementary_price_and_side(self):
        # resting a NO buy at 0.40 (bid_side terms): a taker "sells NO" is the
        # complement of taker_sold_yes, and the NO price is 1 - yes_price.
        trades = [(1500, 0.61, 5.0, False),  # taker sold NO @ 1-0.61=0.39 <= 0.40 -> hit
                  (1600, 0.59, 9.0, False),  # taker sold NO @ 1-0.59=0.41 > 0.40 -> not a hit
                  (1700, 0.62, 4.0, True)]   # taker sold YES (not NO) -> not a hit
        hits = mql.hits_against("NO", 0.40, join_ts=1000, trades_for_ticker=trades)
        self.assertEqual(hits, [(1500, 5.0)])
