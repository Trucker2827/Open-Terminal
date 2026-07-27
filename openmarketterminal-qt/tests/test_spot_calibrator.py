import math
import os
import sys
import unittest

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise"))
sys.path.insert(0, SCRIPTS)
import spot_calibrator as cal


def snapshot(spot=65000.0, floor=64000.0, cap=0.0, seconds_left=900, yes_mid=0.62,
             vol_per_min=6.3, realized_move=12.0):
    horizon = {"spot": spot, "floor_strike": floor, "cap_strike": cap,
               "required_move_bps": abs(floor - spot) / spot * 10000.0 if spot else 0.0,
               "realized_move_30s_bps": realized_move}
    if vol_per_min:
        horizon["realized_volatility"] = {"per_min_bps": vol_per_min}
    return {"contract": {"seconds_left": seconds_left, "yes_mid": yes_mid,
                         "horizon": horizon}}


class FeatureExtractionTest(unittest.TestCase):
    def test_extracts_modeled_above_market(self):
        f = cal.extract_features(snapshot())
        self.assertAlmostEqual(f["signed_distance_bps"], 1000.0 / 65000.0 * 10000.0, places=6)
        self.assertAlmostEqual(f["sqrt_minutes_left"], math.sqrt(15.0), places=9)
        self.assertAlmostEqual(f["yes_mid"], 0.62)
        self.assertGreater(f["required_move_sigma"], 0.0)

    def test_vol_missing_degrades_not_fails(self):
        f = cal.extract_features(snapshot(vol_per_min=0.0))
        self.assertEqual(f["per_min_vol_bps"], 0.0)
        self.assertEqual(f["required_move_sigma"], 0.0)

    def test_unmodeled_snapshots_are_skipped(self):
        self.assertIsNone(cal.extract_features(snapshot(cap=66000.0)))   # range market
        self.assertIsNone(cal.extract_features(snapshot(floor=0.0)))     # no strike
        self.assertIsNone(cal.extract_features(snapshot(seconds_left=0)))
        self.assertIsNone(cal.extract_features(snapshot(yes_mid=0.0)))   # no market mid
        self.assertIsNone(cal.extract_features({"contract": {"seconds_left": "abc"}}))


class OnlineLogitTest(unittest.TestCase):
    def test_learns_separable_signal(self):
        model = cal.OnlineLogit(("signed_distance_bps",), lr=0.3)
        for i in range(400):
            distance = 80.0 if i % 2 == 0 else -80.0
            model.update({"signed_distance_bps": distance}, distance > 0)
        self.assertGreater(model.predict({"signed_distance_bps": 80.0}), 0.75)
        self.assertLess(model.predict({"signed_distance_bps": -80.0}), 0.25)

    def test_state_round_trips_exactly(self):
        model = cal.OnlineLogit(cal.FULL_FEATURES)
        base = snapshot()
        for i in range(20):
            model.update(cal.extract_features(base), i % 2 == 0)
        clone = cal.OnlineLogit.from_json(model.to_json())
        f = cal.extract_features(base)
        self.assertEqual(model.predict(f), clone.predict(f))

    def test_cold_model_predicts_half(self):
        model = cal.OnlineLogit(cal.FULL_FEATURES)
        self.assertAlmostEqual(model.predict(cal.extract_features(snapshot())), 0.5)


class BookPassthroughTest(unittest.TestCase):
    """Issue #158: the report carries the daemon's real top-of-book so the bot
    can price a crossing bid against the spread it would actually pay."""

    def execution(self, **sides):
        return {"execution": {side: quote for side, quote in sides.items()}}

    def test_both_books_pass_through_untouched(self):
        book = cal.extract_book(self.execution(
            yes={"bid": 0.82, "ask": 0.84}, no={"bid": 0.16, "ask": 0.18}))
        self.assertEqual(book, {"market_yes_bid": 0.82, "market_yes_ask": 0.84,
                                "market_no_bid": 0.16, "market_no_ask": 0.18})

    def test_unquoted_levels_are_omitted_never_zeroed(self):
        # The daemon writes 0.0 for a side of the book with no levels. A zero
        # ask would read as a free contract, so it is dropped, not carried.
        book = cal.extract_book(self.execution(
            yes={"bid": 0.999, "ask": 0.0}, no={"bid": 0.0, "ask": None}))
        self.assertEqual(book, {"market_yes_bid": 0.999})
        self.assertEqual(cal.extract_book({}), {})
        self.assertEqual(cal.extract_book({"execution": {"yes": {"ask": "n/a"}}}), {})

    def test_observe_cycle_carries_the_book_beside_the_prediction(self):
        snap = snapshot()
        snap["execution"] = {"yes": {"bid": 0.61, "ask": 0.63},
                             "no": {"bid": 0.37, "ask": 0.39}}
        predictions = cal.observe_cycle(cal.default_state(),
                                        {"snapshots": {"KXBTC-T1": snap}}, 1_000_000)
        entry = predictions["KXBTC-T1"]
        self.assertEqual(entry["market_yes_ask"], 0.63)
        self.assertEqual(entry["market_no_ask"], 0.39)
        # A report with no execution block at all still predicts; the bot's
        # rule is to fail closed to passive quoting when the book is absent.
        bare = cal.observe_cycle(cal.default_state(),
                                 {"snapshots": {"KXBTC-T2": snapshot()}}, 1_000_000)
        self.assertNotIn("market_yes_ask", bare["KXBTC-T2"])

    def test_the_book_is_not_a_model_feature(self):
        # The trained models' input tuples round-trip through the saved state.
        # Adding a book field to either would retrain nothing and invalidate
        # everything, so the passthrough must stay outside both.
        for name in ("market_yes_ask", "market_no_ask", "yes_ask", "no_ask"):
            self.assertNotIn(name, cal.FULL_FEATURES)
            self.assertNotIn(name, cal.MARKET_FEATURES)
        snap = snapshot()
        snap["execution"] = {"yes": {"bid": 0.61, "ask": 0.63}}
        self.assertEqual(cal.extract_features(snap), cal.extract_features(snapshot()))


class SettleCycleTest(unittest.TestCase):
    def test_observe_then_settle_trains_and_scores(self):
        state = cal.default_state()
        evidence = {"snapshots": {"KXBTC-T1": snapshot()}}
        now = 1_000_000
        predictions = cal.observe_cycle(state, evidence, now)
        self.assertIn("KXBTC-T1", predictions)
        self.assertIn("KXBTC-T1", state["pending"])
        # Not yet past close+grace: nothing settles even if resolvable.
        cal.settle_cycle(state, now, resolver=lambda t: True)
        self.assertIn("KXBTC-T1", state["pending"])
        # Past close+grace with a settled market: trains both models.
        later = now + 900 * 1000 + 121_000
        cal.settle_cycle(state, later, resolver=lambda t: True)
        self.assertNotIn("KXBTC-T1", state["pending"])
        self.assertEqual(state["resolved"], 1)
        self.assertEqual(len(state["brier_full"]), 1)
        self.assertEqual(len(state["brier_market"]), 1)

    def test_unresolved_market_is_retried_then_dropped(self):
        state = cal.default_state()
        cal.observe_cycle(state, {"snapshots": {"KXBTC-T2": snapshot()}}, 0)
        cal.settle_cycle(state, 900 * 1000 + 121_000, resolver=lambda t: None)
        self.assertIn("KXBTC-T2", state["pending"])            # retried later
        cal.settle_cycle(state, 25 * 3600 * 1000, resolver=lambda t: None)
        self.assertNotIn("KXBTC-T2", state["pending"])         # dropped, not guessed
        self.assertEqual(state["resolved"], 0)

    def test_report_withholds_value_verdict_until_sample(self):
        state = cal.default_state()
        state["brier_full"] = [[0.8, True]] * 10
        state["brier_market"] = [[0.6, True]] * 10
        report = cal.build_report(state, {}, 0)
        self.assertFalse(report["adds_value_over_market"])      # <100 samples
        state["brier_full"] = [[0.8, True]] * 100
        state["brier_market"] = [[0.6, True]] * 100
        self.assertTrue(cal.build_report(state, {}, 0)["adds_value_over_market"])
        # And never claims value when the market baseline is sharper.
        state["brier_full"], state["brier_market"] = state["brier_market"], state["brier_full"]
        self.assertFalse(cal.build_report(state, {}, 0)["adds_value_over_market"])

    def test_brier(self):
        self.assertIsNone(cal.brier([]))
        self.assertAlmostEqual(cal.brier([(1.0, True), (0.0, False)]), 0.0)
        self.assertAlmostEqual(cal.brier([(0.5, True)]), 0.25)


if __name__ == "__main__":
    unittest.main()
