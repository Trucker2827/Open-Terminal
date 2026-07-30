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
