#!/usr/bin/env python3
"""Tests for the KXBTC15M directional calibrator."""
import math
import os
import sys
import unittest

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise"))
sys.path.insert(0, SCRIPTS)
import kxbtc15m_calibrator as cal


OPEN_TICKER = "KXBTC15M-26AUG071200-00"


def open_now_ms(ticker=OPEN_TICKER, seconds_left=600):
    """Wall clock just before close so ticker-derived seconds_left matches the snap."""
    close = cal.parse_close_ms(ticker)
    assert close is not None
    return close - int(seconds_left) * 1000


def snapshot(open_price=64000.0, spot=64100.0, seconds_left=600, yes_mid=0.62,
             vol_per_min=5.0, yes_bid=None, yes_ask=None,
             venue_lead_bps=0.0, mid_lag_cents=0.0,
             lead_confirms=None, lead_conflicts=None, brti_avg_60s=None):
    horizon = {
        "spot": spot,
        "floor_strike": open_price,
        "reference_strike": open_price,
        "cap_strike": 0,
        "realized_volatility": {"per_min_bps": vol_per_min},
        "venue_lead_bps_30s": venue_lead_bps,
        "mid_lag_cents_30s": mid_lag_cents,
    }
    if lead_confirms is not None:
        horizon["lead_confirms_direction"] = lead_confirms
    if lead_conflicts is not None:
        horizon["lead_conflicts"] = lead_conflicts
    if brti_avg_60s is not None:
        horizon["brti_avg_60s"] = brti_avg_60s
    snap = {"contract": {"seconds_left": seconds_left, "yes_mid": yes_mid,
                         "yes_change_30s_cents": mid_lag_cents,
                         "horizon": horizon, "question": "BTC price up in next 15 mins?"}}
    if yes_bid is not None or yes_ask is not None:
        snap["execution"] = {"yes": {"bid": yes_bid, "ask": yes_ask}}
    return snap


class DirectionalProbabilityTest(unittest.TestCase):
    def test_deep_itm_near_expiry_is_near_one(self):
        p = cal.directional_probability(64000.0, 65000.0, seconds_left=30, per_min_vol_bps=5.0)
        self.assertGreater(p, 0.95)

    def test_deep_otm_near_expiry_is_near_zero(self):
        p = cal.directional_probability(64000.0, 63000.0, seconds_left=30, per_min_vol_bps=5.0)
        self.assertLess(p, 0.05)

    def test_at_the_money_with_time_is_near_half(self):
        p = cal.directional_probability(64000.0, 64000.0, seconds_left=600, per_min_vol_bps=8.0)
        self.assertAlmostEqual(p, 0.5, delta=0.02)

    def test_zero_seconds_is_deterministic(self):
        self.assertGreater(cal.directional_probability(64000.0, 64001.0, 0, 5.0), 0.9)
        self.assertLess(cal.directional_probability(64000.0, 63999.0, 0, 5.0), 0.1)

    def test_missing_inputs_refuse(self):
        self.assertIsNone(cal.directional_probability(0.0, 64000.0, 600, 5.0))
        self.assertIsNone(cal.directional_probability(64000.0, 0.0, 600, 5.0))

    def test_matches_manual_normal_cdf(self):
        open_p, spot, secs, vol = 64000.0, 64128.0, 900, 10.0
        sigma = spot * (vol / 10000.0) * math.sqrt(secs / 60.0)
        expected = cal.clamp_probability(cal.normal_cdf((spot - open_p) / sigma))
        self.assertAlmostEqual(
            cal.directional_probability(open_p, spot, secs, vol), expected, places=12)


class ExtractObservationTest(unittest.TestCase):
    def test_extracts_kxbtc15m(self):
        obs = cal.extract_observation(
            OPEN_TICKER, snapshot(), now_ms=open_now_ms(seconds_left=600))
        self.assertIsNotNone(obs)
        self.assertGreater(obs["p_model"], 0.5)  # spot above open
        self.assertAlmostEqual(obs["yes_mid"], 0.62)
        self.assertAlmostEqual(obs["features"]["sqrt_minutes_left"], math.sqrt(10.0), places=9)

    def test_skips_threshold_family(self):
        self.assertIsNone(cal.extract_observation(
            "KXBTCD-26AUG0712-T64000", snapshot(), now_ms=open_now_ms()))

    def test_skips_invalid_mid_or_expired(self):
        self.assertIsNone(cal.extract_observation(
            OPEN_TICKER, snapshot(yes_mid=0.0), now_ms=open_now_ms()))
        close = cal.parse_close_ms(OPEN_TICKER)
        self.assertIsNone(cal.extract_observation(
            OPEN_TICKER, snapshot(seconds_left=600), now_ms=close))

    def test_skips_frozen_seconds_left_after_close(self):
        """Daemon may leave seconds_left>0 on a closed snapshot — refuse it."""
        close = cal.parse_close_ms(OPEN_TICKER)
        self.assertIsNone(cal.extract_observation(
            OPEN_TICKER, snapshot(seconds_left=999), now_ms=close + 1))

    def test_book_passthrough(self):
        book = cal.extract_book(snapshot(yes_bid=0.61, yes_ask=0.63))
        self.assertAlmostEqual(book["market_yes_bid"], 0.61)
        self.assertAlmostEqual(book["market_yes_ask"], 0.63)


class LagAblationTest(unittest.TestCase):
    def test_ablation_veto_clamps_to_mid_on_conflict(self):
        ablations = cal.ablation_probabilities(
            p_physics=0.80, yes_mid=0.55, lead_confirms=False, lead_conflicts=True)
        self.assertAlmostEqual(ablations["physics"], 0.80)
        self.assertAlmostEqual(ablations["physics_veto_on_conflict"], 0.55)
        self.assertAlmostEqual(ablations["physics_confirm_only"], 0.55)

    def test_ablation_confirm_only_leaves_mid_without_confirm(self):
        ablations = cal.ablation_probabilities(
            p_physics=0.80, yes_mid=0.55, lead_confirms=True, lead_conflicts=False)
        self.assertAlmostEqual(ablations["physics_confirm_only"], 0.80)
        self.assertAlmostEqual(ablations["physics_veto_on_conflict"], 0.80)

    def test_extract_observation_carries_lag_features(self):
        obs = cal.extract_observation(
            OPEN_TICKER,
            snapshot(venue_lead_bps=3.0, mid_lag_cents=-0.5,
                     lead_confirms=False, lead_conflicts=True),
            now_ms=open_now_ms(seconds_left=600),
        )
        self.assertIsNotNone(obs)
        self.assertTrue(obs["features"]["lead_conflicts"])
        self.assertFalse(obs["features"]["lead_confirms_direction"])
        self.assertAlmostEqual(obs["p_ablations"]["physics_veto_on_conflict"],
                               obs["yes_mid"])

    def test_veto_ablation_can_unlock_trust(self):
        state = cal.default_state()
        # Physics loses to mid; veto wins.
        state["contract_scores_full"] = [0.20] * 120
        state["contract_scores_market_mid_raw"] = [0.12] * 120
        state["contract_scores_physics_veto_on_conflict"] = [0.10] * 120
        state["contract_scores_physics_confirm_only"] = [0.18] * 120
        report = cal.build_report(state, {}, 0)
        self.assertTrue(report["adds_value_over_market"])
        self.assertEqual(report["trusted_variant"], "physics_veto_on_conflict")
        self.assertTrue(report["ablations"]["physics_veto_on_conflict"]["beats_mid"])
        self.assertFalse(report["ablations"]["physics"]["beats_mid"])


class BrtiAvg60Test(unittest.TestCase):
    def test_settlement_aligned_spot_prefers_avg60(self):
        spot, source = cal.settlement_aligned_spot(64000.0, 64150.0, 60)
        self.assertAlmostEqual(spot, 64150.0)
        self.assertEqual(source, "brti_avg_60s")

    def test_settlement_aligned_spot_falls_back_without_avg60(self):
        spot, source = cal.settlement_aligned_spot(64000.0, None, 30)
        self.assertAlmostEqual(spot, 64000.0)
        self.assertEqual(source, "last_print_near_close_no_avg60")

    def test_extract_uses_avg60_for_brti_ablation(self):
        # Last print deep OTM, avg60 deep ITM → brti ablation should be high.
        obs = cal.extract_observation(
            OPEN_TICKER,
            snapshot(open_price=64000.0, spot=63000.0, seconds_left=60,
                     vol_per_min=5.0, brti_avg_60s=65000.0),
            now_ms=open_now_ms(seconds_left=60),
        )
        self.assertIsNotNone(obs)
        self.assertLess(obs["p_model"], 0.2)
        self.assertGreater(obs["p_ablations"]["physics_brti_avg60"], 0.8)
        self.assertEqual(obs["features"]["settlement_aligned_source"], "brti_avg_60s")

    def test_brti_ablation_can_unlock_trust(self):
        state = cal.default_state()
        state["contract_scores_full"] = [0.20] * 120
        state["contract_scores_market_mid_raw"] = [0.12] * 120
        state["contract_scores_physics_veto_on_conflict"] = [0.18] * 120
        state["contract_scores_physics_confirm_only"] = [0.18] * 120
        state["contract_scores_physics_brti_avg60"] = [0.09] * 120
        report = cal.build_report(state, {}, 0)
        self.assertTrue(report["adds_value_over_market"])
        self.assertEqual(report["trusted_variant"], "physics_brti_avg60")

    def test_vol_regime_ablation_present(self):
        # Quiet vol → vol-regime ablation clamps to mid.
        obs = cal.extract_observation(
            OPEN_TICKER,
            snapshot(open_price=64000.0, spot=64100.0, seconds_left=600,
                     vol_per_min=1.0, yes_mid=0.55),
            now_ms=open_now_ms(seconds_left=600),
        )
        self.assertIsNotNone(obs)
        self.assertAlmostEqual(obs["p_ablations"]["physics_vol_regime_confirm"], 0.55)
        self.assertEqual(obs["features"]["vol_regime"], "quiet")


class PerContractScoringTest(unittest.TestCase):
    def settle_one(self, state, ticker, n_obs, outcome, close_at=0):
        evidence = {"snapshots": {ticker: snapshot()}}
        for _ in range(n_obs):
            # Empty brti_samples skips the 50MB CF log in unit tests.
            cal.observe_cycle(state, evidence, close_at, brti_samples=[])
        # Force close in the past so settle_cycle is willing.
        state["pending"][ticker]["close_ms"] = close_at
        cal.settle_cycle(state, close_at + 121_000, resolver=lambda t, o: outcome)

    def test_many_observations_score_one_contract(self):
        state = cal.default_state()
        self.settle_one(state, "KXBTC15M-26AUG071200-00", 60, True)
        self.assertEqual(state["resolved"], 1)
        self.assertEqual(len(state["contract_scores_full"]), 1)
        self.assertEqual(len(state["contract_scores_market_mid_raw"]), 1)
        report = cal.build_report(state, {}, 0)
        self.assertEqual(report["scored_contracts"], 1)
        self.assertEqual(report["training_observations"], 60)

    def test_dense_observations_do_not_clear_contract_gate(self):
        state = cal.default_state()
        for i in range(2):
            self.settle_one(state, "KXBTC15M-26AUG07120%d-00" % i, 60, i % 2 == 0,
                            close_at=i * 10_000_000)
        report = cal.build_report(state, {}, 0)
        self.assertEqual(report["scored_contracts"], 2)
        self.assertLess(report["scored_contracts"], cal.MIN_SCORED_CONTRACTS)
        self.assertFalse(report["adds_value_over_market"])


class TrustGateTest(unittest.TestCase):
    def scored_state(self, full, mid, contracts):
        state = cal.default_state()
        state["contract_scores_full"] = [full] * contracts
        state["contract_scores_market_mid_raw"] = [mid] * contracts
        state["resolved"] = contracts
        return state

    def test_tying_mid_is_not_value(self):
        report = cal.build_report(self.scored_state(0.11, 0.11, 200), {}, 0)
        self.assertFalse(report["adds_value_over_market"])

    def test_beating_mid_with_enough_contracts_is_value(self):
        report = cal.build_report(self.scored_state(0.10, 0.11, 200), {}, 0)
        self.assertTrue(report["adds_value_over_market"])

    def test_beating_mid_below_floor_is_not_value(self):
        report = cal.build_report(self.scored_state(0.10, 0.11, 50), {}, 0)
        self.assertFalse(report["adds_value_over_market"])
        self.assertEqual(report["scored_contracts"], 50)


class ObservePredictTest(unittest.TestCase):
    def test_observe_emits_bot_shaped_prediction(self):
        state = cal.default_state()
        ticker = "KXBTC15M-26AUG071230-30"
        preds = cal.observe_cycle(
            state, {"snapshots": {ticker: snapshot(yes_bid=0.60, yes_ask=0.64)}},
            now_ms=open_now_ms(ticker, seconds_left=600), brti_samples=[],
            rest_markets=[])
        self.assertIn(ticker, preds)
        self.assertIn("p_yes_full", preds[ticker])
        self.assertIn("market_yes_mid", preds[ticker])
        self.assertIn("sqrt_minutes_left", preds[ticker]["features"])
        self.assertAlmostEqual(preds[ticker]["market_yes_bid"], 0.60)
        self.assertEqual(preds[ticker]["probability_source"], "kxbtc15m-directional-gaussian")

    def test_rest_fallback_fills_when_ws_snapshots_empty(self):
        """At 15m rollover the daemon may briefly have no open snapshot."""
        ticker = OPEN_TICKER
        now_ms = open_now_ms(ticker, seconds_left=600)
        # Synthetic BRTI trail for spot + realized vol.
        brti = []
        spot = 64050.0
        for i in range(40):
            spot += (1.0 if i % 2 == 0 else -0.5)
            brti.append((now_ms - (39 - i) * 60_000, spot, spot))
        # Ensure a print lands inside the REST nearest-spot window.
        brti.append((now_ms, spot, spot))
        market = {
            "ticker": ticker,
            "floor_strike": 64000.0,
            "yes_bid_dollars": "0.60",
            "yes_ask_dollars": "0.64",
            "status": "active",
        }
        state = cal.default_state()
        preds = cal.observe_cycle(
            state, {"snapshots": {}}, now_ms=now_ms, brti_samples=brti,
            rest_markets=[market])
        self.assertIn(ticker, preds)
        self.assertAlmostEqual(preds[ticker]["market_yes_mid"], 0.62)
        self.assertEqual(preds[ticker]["features"].get("source"), "rest+brti")

    def test_yes_mid_from_no_book_when_yes_quotes_missing(self):
        mid = cal.yes_mid_from_market({
            "no_bid_dollars": "0.3500",
            "no_ask_dollars": "0.3600",
            "last_price_dollars": "0.6500",
        })
        # YES bid = 1-0.36, YES ask = 1-0.35 → mid 0.645
        self.assertAlmostEqual(mid, 0.645)


class TickerParseTest(unittest.TestCase):
    def test_parse_close_ms_eastern(self):
        ms = cal.parse_close_ms("KXBTC15M-26AUG071230-30")
        self.assertIsNotNone(ms)
        # 2026-08-07 12:30 America/New_York (EDT = UTC-4) → 16:30 UTC
        self.assertEqual(ms, 1786120200000)

    def test_is_kxbtc15m(self):
        self.assertTrue(cal.is_kxbtc15m_ticker("KXBTC15M-26AUG071230-30"))
        self.assertFalse(cal.is_kxbtc15m_ticker("KXBTCD-26AUG0712-T64000"))


class BetEligibleTrustTest(unittest.TestCase):
    VARIANTS = ("physics", "physics_veto_on_conflict", "physics_confirm_only",
                "physics_brti_avg60", "physics_vol_regime_confirm")

    def _state(self, eligible_model, eligible_mid, n):
        state = cal.default_state()
        state["contract_scores_full"] = [0.05] * n
        state["contract_scores_market_mid_raw"] = [0.06] * n
        for key in ("physics_veto_on_conflict", "physics_confirm_only",
                    "physics_brti_avg60", "physics_vol_regime_confirm"):
            state[f"contract_scores_{key}"] = [0.05] * n
            state[f"contract_scores_eligible_{key}"] = [eligible_model] * n
        state["contract_scores_eligible_full"] = [eligible_model] * n
        state["contract_scores_eligible_market_mid_raw"] = [eligible_mid] * n
        return state

    def test_losing_where_it_bets_is_untrusted_on_the_new_flag(self):
        # Wins the easy full population, loses the population it bets.
        state = self._state(0.2576, 0.2130, 100)
        self.assertIsNone(cal.select_trusted_variant_eligible(state))
        r = cal.build_report(state, {}, 1_700_000_000_000)
        self.assertFalse(r["adds_value_on_bet_eligible"])

    def test_winning_where_it_bets_can_earn_the_new_flag(self):
        state = self._state(0.2130, 0.2576, 100)
        self.assertIsNotNone(cal.select_trusted_variant_eligible(state))
        r = cal.build_report(state, {}, 1_700_000_000_000)
        self.assertTrue(r["adds_value_on_bet_eligible"])

    def test_below_the_eligible_floor_is_untrusted(self):
        state = self._state(0.2130, 0.2576, 99)
        self.assertIsNone(cal.select_trusted_variant_eligible(state))

    def test_live_predictor_selection_is_untouched(self):
        # select_trusted_variant also picks live_p (the PUBLISHED probability).
        # This change must not move it: it gates trust, not prediction.
        state = self._state(0.2576, 0.2130, 100)
        self.assertIsNotNone(cal.select_trusted_variant(state))

    def test_publishes_the_eligible_numbers(self):
        r = cal.build_report(self._state(0.21, 0.26, 100), {}, 1_700_000_000_000)
        self.assertEqual(r["eligible_scored_contracts"], 100)
        self.assertAlmostEqual(r["brier_eligible_full"], 0.21)
        self.assertAlmostEqual(r["brier_eligible_market_mid_raw"], 0.26)
        self.assertEqual(r["min_eligible_contracts"], 100)


if __name__ == "__main__":
    unittest.main()
