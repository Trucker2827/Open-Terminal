import math
import os
import sys
import unittest

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise"))
sys.path.insert(0, SCRIPTS)
import spot_calibrator as cal


def snapshot(spot=65000.0, floor=64000.0, cap=0.0, seconds_left=900, yes_mid=0.62,
             vol_per_min=6.3, realized_move=12.0, yes_bid_size=None, yes_ask_size=None):
    horizon = {"spot": spot, "floor_strike": floor, "cap_strike": cap,
               "required_move_bps": abs(floor - spot) / spot * 10000.0 if spot else 0.0,
               "realized_move_30s_bps": realized_move}
    if vol_per_min:
        horizon["realized_volatility"] = {"per_min_bps": vol_per_min}
    snap = {"contract": {"seconds_left": seconds_left, "yes_mid": yes_mid,
                         "horizon": horizon}}
    if yes_bid_size is not None or yes_ask_size is not None:
        snap["execution"] = {"yes": {"bid_size": yes_bid_size, "ask_size": yes_ask_size}}
    return snap


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

    def test_book_imbalance_from_snapshot_sizes(self):
        # execution.yes carries bid_size/ask_size; imbalance is (bid-ask)/(bid+ask).
        snap = snapshot(yes_bid_size=300, yes_ask_size=100)
        feats = cal.extract_features(snap)
        self.assertAlmostEqual(feats["book_imbalance"], 0.5)       # (300-100)/(300+100)
        self.assertEqual(feats["trade_flow"], 0.0)                 # neutral stub (Task 2)
        self.assertEqual(feats["spot_drift"], 0.0)
        self.assertEqual(feats["news_forecast"], 0.0)

    def test_full_features_include_the_four_new_signals(self):
        for f in ("book_imbalance", "trade_flow", "spot_drift", "news_forecast"):
            self.assertIn(f, cal.FULL_FEATURES)


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

    def test_l2_shrinks_a_weight_toward_zero(self):
        # With no error signal, repeated L2-regularized updates pull a nonzero
        # feature weight toward 0; the bias is NOT regularized.
        m = cal.OnlineLogit(("x",))
        m.w[0] = 5.0
        for _ in range(200):
            m.update({"x": 0.0}, outcome=False, l2=0.1)   # x standardized ~0 => no data gradient
        self.assertLess(abs(m.w[0]), 5.0)

    def test_state_migrates_old_model_to_new_features(self):
        # An old saved "full" model with only the physics features loads into the
        # new FULL_FEATURES with the physics weights preserved and the four new
        # weights zero-initialized.
        old = cal.OnlineLogit(cal.PHYSICS_FEATURES)  # the pre-ensemble tuple
        # Distinguishable weights per feature (not all 1.0): an `old_index`
        # off-by-one or a swapped mapping would land a weight on the wrong
        # feature, which a uniform 1.0 fixture could never catch.
        old_weights = [1.1, 2.2, 3.3, 4.4, 5.5, 6.6]
        old_bias = 9.9
        self.assertEqual(len(old_weights), len(cal.PHYSICS_FEATURES))
        old.w = old_weights + [old_bias]
        migrated = cal.reconcile_full_model(old.to_json())
        self.assertEqual(tuple(migrated.features), cal.FULL_FEATURES)
        # Each physics weight lands at its OWN feature's new index, not just
        # somewhere nonzero.
        for f, expected in zip(cal.PHYSICS_FEATURES, old_weights):
            self.assertEqual(migrated.w[migrated.features.index(f)], expected)
        self.assertEqual(migrated.w[-1], old_bias)
        for f in ("book_imbalance", "trade_flow", "spot_drift", "news_forecast"):
            self.assertEqual(migrated.w[migrated.features.index(f)], 0.0)

    def test_migration_reaches_disk_through_settle_cycle(self):
        # reconcile_full_model() is correct in isolation, but only settle_cycle
        # PERSISTS state["full"] back to state (observe_cycle's copy is
        # predict-only and never saved). This drives an old 6-feature model
        # through the real observe_cycle -> settle_cycle pair and checks the
        # migration actually stuck on the state that would be written to disk,
        # not just on a local OnlineLogit that gets thrown away.
        state = cal.default_state()
        old_model = cal.OnlineLogit(cal.PHYSICS_FEATURES)
        old_weights = [1.1, 2.2, 3.3, 4.4, 5.5, 6.6]
        old_model.w = old_weights + [9.9]
        state["full"] = old_model.to_json()
        self.assertEqual(state["full"]["features"], list(cal.PHYSICS_FEATURES))

        evidence = {"snapshots": {"KXBTC-T1": snapshot()}}
        now = 1_000_000
        predictions = cal.observe_cycle(state, evidence, now)  # (a) no crash
        self.assertIn("KXBTC-T1", predictions)
        self.assertIn("p_yes_full", predictions["KXBTC-T1"])          # (c)
        self.assertIn("p_yes_market_baseline", predictions["KXBTC-T1"])

        later = now + 900 * 1000 + 121_000
        cal.settle_cycle(state, later, resolver=lambda t: True)      # (a) no crash

        # (b) the persisted model — the one settle_cycle just wrote back into
        # state["full"] for saving — now carries all ten features. If the
        # reconcile_full_model() call were removed from settle_cycle, this
        # would still read PHYSICS_FEATURES (6 features), not FULL_FEATURES.
        self.assertEqual(state["full"]["features"], list(cal.FULL_FEATURES))
        self.assertEqual(len(state["full"]["w"]), len(cal.FULL_FEATURES) + 1)


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
        self.assertEqual(len(state["contract_scores_full"]), 1)
        self.assertEqual(len(state["contract_scores_market_trained_logit"]), 1)
        self.assertEqual(len(state["contract_scores_market_mid_raw"]), 1)

    def test_unresolved_market_is_retried_then_dropped(self):
        state = cal.default_state()
        cal.observe_cycle(state, {"snapshots": {"KXBTC-T2": snapshot()}}, 0)
        cal.settle_cycle(state, 900 * 1000 + 121_000, resolver=lambda t: None)
        self.assertIn("KXBTC-T2", state["pending"])            # retried later
        cal.settle_cycle(state, 25 * 3600 * 1000, resolver=lambda t: None)
        self.assertNotIn("KXBTC-T2", state["pending"])         # dropped, not guessed
        self.assertEqual(state["resolved"], 0)

    def test_report_withholds_value_verdict_until_sample(self):
        state = scored_state(full=0.05, mid_raw=0.09, logit=0.09, contracts=10)
        report = cal.build_report(state, {}, 0)
        self.assertFalse(report["adds_value_over_market"])      # <100 CONTRACTS
        full = scored_state(full=0.05, mid_raw=0.09, logit=0.09, contracts=100)
        self.assertTrue(cal.build_report(full, {}, 0)["adds_value_over_market"])
        # And never claims value when the raw mid is sharper.
        losing = scored_state(full=0.09, mid_raw=0.05, logit=0.09, contracts=100)
        self.assertFalse(cal.build_report(losing, {}, 0)["adds_value_over_market"])

    def test_brier(self):
        self.assertIsNone(cal.brier([]))
        self.assertAlmostEqual(cal.brier([(1.0, True), (0.0, False)]), 0.0)
        self.assertAlmostEqual(cal.brier([(0.5, True)]), 0.25)

    def test_mean_or_none_is_the_across_contract_step(self):
        self.assertIsNone(cal.mean_or_none([]))
        self.assertAlmostEqual(cal.mean_or_none([0.1, 0.3]), 0.2)


def scored_state(full, mid_raw, logit, contracts):
    """A state whose per-contract score lists are already populated."""
    state = cal.default_state()
    state["contract_scores_full"] = [full] * contracts
    state["contract_scores_market_mid_raw"] = [mid_raw] * contracts
    state["contract_scores_market_trained_logit"] = [logit] * contracts
    state["resolved"] = contracts
    return state


class PerContractScoringTest(unittest.TestCase):
    """Issue #171: the Brier is scored per CONTRACT, not per observation.

    Schema 1 appended one pair per observation and a contract contributes up to
    MAX_OBS_PER_TICKER = 60 of them, so `training_samples: 500` was ~8-10
    contracts and the >=100 gate cleared after about two settled.
    """

    def settle_one_contract(self, state, ticker, observations, outcome, close_at=0):
        """Observe `observations` snapshots of one ticker, then settle it."""
        for _ in range(observations):
            cal.observe_cycle(state, {"snapshots": {ticker: snapshot()}}, close_at)
        cal.settle_cycle(state, close_at + 900 * 1000 + 121_000,
                         resolver=lambda t: outcome)

    def test_one_contract_scores_once_however_many_observations(self):
        state = cal.default_state()
        self.settle_one_contract(state, "KXBTC-T1", 60, True)
        self.assertEqual(state["resolved"], 1)
        self.assertEqual(len(state["contract_scores_full"]), 1)
        self.assertEqual(len(state["contract_scores_market_mid_raw"]), 1)
        # ...and the model still TRAINED on all 60 rows. Only the bookkeeping
        # is per contract.
        self.assertEqual(state["full"]["n_seen"], 60)
        report = cal.build_report(state, {}, 0)
        self.assertEqual(report["scored_contracts"], 1)
        self.assertEqual(report["training_observations"], 60)

    def test_dense_observations_do_not_clear_the_contract_gate(self):
        # The defect, encoded: two contracts x 60 observations cleared the old
        # >=100 gate outright. Under the contract count it is 2 of 100.
        state = cal.default_state()
        for i in range(2):
            self.settle_one_contract(state, "KXBTC-T%d" % i, 60, i % 2 == 0)
        report = cal.build_report(state, {}, 0)
        self.assertEqual(report["scored_contracts"], 2)
        self.assertEqual(report["training_observations"], 120)
        self.assertLess(report["scored_contracts"], cal.MIN_SCORED_CONTRACTS)
        self.assertFalse(report["adds_value_over_market"])

    def test_every_contract_weighs_the_same(self):
        # 60 observations and 2 observations are one score each, so a densely
        # sampled contract cannot outvote a sparse one.
        state = cal.default_state()
        self.settle_one_contract(state, "KXBTC-DENSE", 60, True)
        self.settle_one_contract(state, "KXBTC-SPARSE", 2, True, close_at=10_000_000)
        self.assertEqual(len(state["contract_scores_full"]), 2)
        self.assertAlmostEqual(cal.build_report(state, {}, 0)["brier_full"],
                               sum(state["contract_scores_full"]) / 2.0)

    def test_scores_are_taken_before_training_on_the_contract(self):
        # A cold model predicts 0.5, so its first contract must score exactly
        # 0.25 — anything lower means the outcome leaked into its own score.
        state = cal.default_state()
        self.settle_one_contract(state, "KXBTC-T1", 5, True)
        self.assertAlmostEqual(state["contract_scores_full"][0], 0.25)
        self.assertAlmostEqual(state["contract_scores_market_trained_logit"][0], 0.25)

    def test_a_settled_contract_with_no_observations_scores_nothing(self):
        state = cal.default_state()
        state["pending"]["KXBTC-EMPTY"] = {"close_ms": 0, "obs": []}
        cal.settle_cycle(state, 200_000, resolver=lambda t: True)
        self.assertNotIn("KXBTC-EMPTY", state["pending"])
        self.assertEqual(state["contract_scores_full"], [])


class RawMidBaselineTest(unittest.TestCase):
    """Issue #171: 'beats the market' must mean beating the raw mid.

    MARKET_FEATURES = ("yes_mid",) is a TRAINED one-feature logit — a
    handicapped baseline. Both are reported; only the raw mid confers trust.
    """

    def test_raw_mid_is_the_untrained_mid(self):
        state = cal.default_state()
        # yes_mid 0.62 on a contract that settles YES: (0.62 - 1)^2.
        cal.observe_cycle(state, {"snapshots": {"KXBTC-T1": snapshot(yes_mid=0.62)}}, 0)
        cal.settle_cycle(state, 900 * 1000 + 121_000, resolver=lambda t: True)
        self.assertAlmostEqual(state["contract_scores_market_mid_raw"][0], (0.62 - 1.0) ** 2)

    def test_tying_the_raw_mid_is_not_adding_value(self):
        tied = scored_state(full=0.0989, mid_raw=0.0989, logit=0.2, contracts=239)
        report = cal.build_report(tied, {}, 0)
        self.assertFalse(report["adds_value_over_market"])       # tie is not a win
        self.assertTrue(report["beats_trained_logit_baseline"])  # ...and says so
        beating = scored_state(full=0.0988, mid_raw=0.0989, logit=0.2, contracts=239)
        self.assertTrue(cal.build_report(beating, {}, 0)["adds_value_over_market"])

    def test_beating_only_the_handicapped_baseline_is_not_value(self):
        # The autopsy's measured shape: the calibrator loses to the raw mid
        # (0.1043 vs 0.0989) while clearing a trained logit.
        state = scored_state(full=0.1043, mid_raw=0.0989, logit=0.1100, contracts=239)
        report = cal.build_report(state, {}, 0)
        self.assertFalse(report["adds_value_over_market"])
        self.assertTrue(report["beats_trained_logit_baseline"])
        # Both stay visible, distinctly named.
        self.assertAlmostEqual(report["brier_market_mid_raw"], 0.0989)
        self.assertAlmostEqual(report["brier_market_trained_logit"], 0.1100)


class MigrationTest(unittest.TestCase):
    """Issue #171: a schema-1 state reads as insufficient, never reinterpreted."""

    def old_state(self, pairs=500):
        state = cal.default_state()
        del state["contract_scores_full"]
        del state["contract_scores_market_trained_logit"]
        del state["contract_scores_market_mid_raw"]
        state["schema"] = 1
        state["brier_full"] = [[0.8, True]] * pairs
        state["brier_market"] = [[0.9, True]] * pairs
        state["resolved"] = 976
        state["full"]["n_seen"] = 24_700
        return state

    def test_observation_pairs_are_discarded_not_recounted_as_contracts(self):
        migrated = cal.migrate_state(self.old_state())
        self.assertEqual(migrated["schema"], cal.STATE_SCHEMA)
        self.assertEqual(migrated["contract_scores_full"], [])
        self.assertEqual(migrated["migrated_from_schema"], 1)
        self.assertEqual(migrated["discarded_observation_pairs"], 500)
        report = cal.build_report(migrated, {}, 0)
        self.assertEqual(report["scored_contracts"], 0)          # not 500
        self.assertIsNone(report["brier_full"])
        self.assertFalse(report["adds_value_over_market"])
        # The lifetime count and the training rows survive: they were never
        # the thing that was miscounted.
        self.assertEqual(report["resolved_contracts"], 976)
        self.assertEqual(report["training_observations"], 24_700)

    def test_migration_keeps_the_trained_weights_and_pending_work(self):
        old = self.old_state()
        old["pending"] = {"KXBTC-T1": {"close_ms": 5, "obs": [cal.extract_features(snapshot())]}}
        old["full"]["w"] = [0.5] * (len(cal.FULL_FEATURES) + 1)
        migrated = cal.migrate_state(old)
        self.assertEqual(migrated["full"]["w"], [0.5] * (len(cal.FULL_FEATURES) + 1))
        self.assertIn("KXBTC-T1", migrated["pending"])

    def test_a_current_state_is_left_alone(self):
        state = scored_state(full=0.1, mid_raw=0.2, logit=0.3, contracts=4)
        self.assertIs(cal.migrate_state(state), state)


if __name__ == "__main__":
    unittest.main()
