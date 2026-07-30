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


class SizeBookTest(unittest.TestCase):
    def test_at_is_backward_looking_only(self):
        # Rows at 1000 and 3000; a lookup at 2000 must return the 1000 row,
        # never the 3000 one that hasn't "happened" yet (no look-ahead).
        book = mql.SizeBook({"T": [(1000, 5.0, 7.0), (3000, 50.0, 70.0)]})
        self.assertEqual(book.at("T", 2000), (1000, 5.0, 7.0))
        self.assertEqual(book.at("T", 1000), (1000, 5.0, 7.0))  # at == ts is in-bounds
        self.assertIsNone(book.at("T", 999))                    # before any row
        self.assertIsNone(book.at("UNKNOWN", 2000))


class SimulateEventAheadSizeTest(unittest.TestCase):
    """Guards the non-degenerate bracket: with a real ahead_size > 0, the
    pessimistic fill must be able to land LATER than the optimistic one, so
    that within some horizon the optimistic bound has filled while the
    pessimistic bound has not (they differ). Before this fix `ahead_size`
    was hardcoded to 0.0, which forces `maker_fill`'s pessimistic branch to
    trigger on the very first hit -- collapsing it onto the optimistic bound
    every time, at every horizon. This test fails against that regression:
    with `ahead_size` stuck at 0.0, `filled_pess` would equal `filled_opt`
    at the 15s horizon below (both True), instead of False.
    """

    TICKER = "KXBTCD-30JUL2600-T50000"

    def _book(self, t):
        # A quote row within QUOTE_STALENESS_MS (15s) of every exit horizon
        # this test inspects (5s, 15s, 30s), so `book.at` never returns None
        # at those exit timestamps purely from staleness.
        return mql.q1.QuoteBook({self.TICKER: [
            (t - 1000, 0.40, 0.45, 0.425),
            (t + 14000, 0.40, 0.45, 0.425),
            (t + 29000, 0.40, 0.45, 0.425),
        ]})

    def test_real_ahead_size_makes_pessimistic_fill_later_than_optimistic(self):
        # 30 minutes before close: inside [MIN_SECONDS_TO_CLOSE,
        # MAX_SECONDS_TO_CLOSE] = [120, 3600]s, so simulate_event's window
        # filter keeps this ticker.
        close_ms = mql.common.parse_ticker(self.TICKER)["close_ms"]
        t = close_ms - 1800 * 1000
        # Rest a YES bid at the touch (0.40) with 12 contracts resting ahead
        # of us on the YES bid.
        book = self._book(t)
        # Two taker sells of YES through our price after join: the first
        # (5 contracts, at t+1s) does not clear the 12 ahead of us; the
        # second (10 contracts, at t+20s) does, so the pessimistic fill
        # lands there -- strictly later than the optimistic (first-hit) ts.
        trades = {self.TICKER: [(t + 1000, 0.39, 5.0, True),
                                (t + 20000, 0.38, 10.0, True)]}
        outcomes = {}
        sizes = mql.SizeBook({self.TICKER: [(t - 1000, 12.0, 30.0)]})  # yes_bid_size=12
        event = {"ts_ms": t, "move_bps": 25.0}  # positive move -> bet YES

        records = mql.simulate_event(event, book, trades, outcomes, sizes=sizes)
        by_horizon = {r["horizon_s"]: r for r in records
                     if r["rest_offset_ticks"] == 0}

        for horizon in (5, 15, 30):
            self.assertIn(horizon, by_horizon, f"missing horizon {horizon}")
            self.assertEqual(by_horizon[horizon]["ahead_size"], 12.0)
            self.assertEqual(by_horizon[horizon]["ahead_size_source"],
                             mql.AHEAD_SIZE_SOURCE_TOP_OF_BOOK)

        # At the 15s horizon the optimistic hit (t+1s) has landed but the
        # pessimistic hit (t+20s) has not -- proof the bracket is REAL, not
        # collapsed: the two bounds disagree on whether we filled at all.
        r15 = by_horizon[15]
        self.assertTrue(r15["filled_opt"])
        self.assertIsNotNone(r15["pnl_optimistic"])
        self.assertFalse(r15["filled_pess"])
        self.assertIsNone(r15["pnl_pessimistic"])

        # By the 30s horizon the pessimistic hit has also landed (t+20s <=
        # t+30s): both bounds now fill, confirming the pessimistic bound is
        # a real (later), not a phantom, timestamp -- not that it never
        # fills at all.
        r30 = by_horizon[30]
        self.assertTrue(r30["filled_opt"])
        self.assertTrue(r30["filled_pess"])
        self.assertIsNotNone(r30["pnl_pessimistic"])

    def test_no_size_row_falls_back_to_zero_and_says_so(self):
        close_ms = mql.common.parse_ticker(self.TICKER)["close_ms"]
        t = close_ms - 1800 * 1000
        book = self._book(t)
        trades = {self.TICKER: [(t + 1000, 0.39, 5.0, True)]}
        outcomes = {}
        event = {"ts_ms": t, "move_bps": 25.0}

        records = mql.simulate_event(event, book, trades, outcomes, sizes=None)

        matches = [r for r in records if r["rest_offset_ticks"] == 0
                  and r["horizon_s"] == 5]
        self.assertEqual(len(matches), 1)
        self.assertEqual(matches[0]["ahead_size"], 0.0)
        self.assertEqual(matches[0]["ahead_size_source"], mql.AHEAD_SIZE_SOURCE_NO_SIZE_ROW)
