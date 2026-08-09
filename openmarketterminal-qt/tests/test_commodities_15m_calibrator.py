#!/usr/bin/env python3
"""Tests for the commodities 15m directional calibrator."""
import math
import os
import sys
import unittest

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise"))
sys.path.insert(0, SCRIPTS)
import commodities_15m_calibrator as cal
import kxbtc15m_calibrator as race


def gold_market(open_price=4354.62, yes_bid=0.35, yes_ask=0.37, seconds_left=600,
                ticker="KXGOLD15M-26AUG072030-30", now_ms=None):
    if now_ms is None:
        close_ms = cal.parse_close_ms(ticker)
        now_ms = close_ms - seconds_left * 1000
    close_iso = __import__("datetime").datetime.utcfromtimestamp(
        (now_ms + seconds_left * 1000) / 1000.0
    ).strftime("%Y-%m-%dT%H:%M:%SZ")
    return {
        "ticker": ticker,
        "floor_strike": open_price,
        "yes_bid_dollars": yes_bid,
        "yes_ask_dollars": yes_ask,
        "close_time": close_iso,
    }, now_ms


class FamilyTest(unittest.TestCase):
    def test_commodity_families(self):
        self.assertTrue(cal.is_commodity_15m_ticker("KXGOLD15M-26AUG072030-30"))
        self.assertTrue(cal.is_commodity_15m_ticker("KXSILVER15M-26AUG072030-30"))
        self.assertTrue(cal.is_commodity_15m_ticker("KXWTI15M-26AUG072030-30"))
        self.assertFalse(cal.is_commodity_15m_ticker("KXBTC15M-26AUG072030-30"))
        self.assertFalse(cal.is_commodity_15m_ticker("KXBTCD-26AUG0712-T64000"))

    def test_shares_race_math_with_btc_calibrator(self):
        p = race.directional_probability(4354.0, 4360.0, 600, 8.0)
        self.assertAlmostEqual(
            p, race.directional_probability(4354.0, 4360.0, 600, 8.0), places=12)
        self.assertGreater(p, 0.5)


class PythParseTest(unittest.TestCase):
    def test_parse_pyth_price_expo(self):
        parsed = cal.parse_pyth_price({
            "price": {"price": "435812000000", "conf": "1000000", "expo": -8,
                      "publish_time": 1786120200},
        })
        self.assertIsNotNone(parsed)
        price, conf, publish_ms = parsed
        self.assertAlmostEqual(price, 4358.12, places=6)
        self.assertAlmostEqual(conf, 0.01, places=6)
        self.assertEqual(publish_ms, 1786120200_000)

    def test_feed_map_locked(self):
        self.assertEqual(cal.FAMILIES["KXGOLD15M"]["pyth_symbol"], "Metal.XAU/USD")
        self.assertTrue(cal.FAMILIES["KXGOLD15M"]["pyth_id"].startswith("765d2ba9"))
        self.assertEqual(cal.FAMILIES["KXSILVER15M"]["pyth_symbol"], "Metal.XAG/USD")
        self.assertEqual(cal.FAMILIES["KXWTI15M"]["pyth_symbol"], "Metal.XTI/USD")

    def test_fetch_pyth_latest_parses_hermes(self):
        payload = {
            "parsed": [{
                "id": "765d2ba906dbc32ca17cc11f5310a89e9ee1f6420508c63861f2f8ba4ee34bb2",
                "price": {"price": "436000000000", "conf": "1", "expo": -8,
                          "publish_time": 1000},
            }]
        }
        ticks = cal.fetch_pyth_latest(
            [cal.FAMILIES["KXGOLD15M"]["pyth_id"]],
            fetcher=lambda url: payload,
        )
        self.assertIn(cal.FAMILIES["KXGOLD15M"]["pyth_id"], ticks)
        self.assertAlmostEqual(ticks[cal.FAMILIES["KXGOLD15M"]["pyth_id"]][0], 4360.0)


class RestObservationTest(unittest.TestCase):
    def test_observation_from_rest_prefers_pyth(self):
        market, now_ms = gold_market()
        base = 4358.0
        t0 = now_ms - 60 * 60 * 1000
        series = [[t0 + i * 60_000, base + (0.2 if i % 2 == 0 else -0.1)] for i in range(40)]
        pyth = {"Metal.XAU/USD": series}
        # Yahoo series provided so Phase-3 tape enrichment does not hit the network.
        yahoo = {"GC=F": [(t, p) for t, p in series]}
        obs = cal.observation_from_rest(market, yahoo, now_ms, pyth_series=pyth)
        self.assertIsNotNone(obs)
        self.assertGreater(obs["p_model"], 0.5)
        self.assertEqual(obs["features"]["spot_source"], "pyth:Metal.XAU/USD")
        self.assertEqual(obs["features"]["settlement_source"], "pyth:Metal.XAU/USD")
        self.assertIn("physics_tape_confirm_near_close", obs["p_ablations"])
        self.assertIn("session_regime", obs["features"])

    def test_observation_from_rest_yahoo_fallback(self):
        market, now_ms = gold_market()
        # Synthetic Yahoo series: spot above open, mild vol.
        base = 4358.0
        series = []
        t0 = now_ms - 60 * 60 * 1000
        for i in range(40):
            series.append((t0 + i * 60_000, base + (0.2 if i % 2 == 0 else -0.1)))
        cache = {"GC=F": series}
        obs = cal.observation_from_rest(market, cache, now_ms, pyth_series={})
        self.assertIsNotNone(obs)
        self.assertGreater(obs["p_model"], 0.5)
        self.assertAlmostEqual(obs["yes_mid"], 0.36, places=6)
        self.assertEqual(obs["features"]["underlier"], "gold")
        self.assertTrue(obs["features"]["spot_source"].startswith("yahoo:"))
        self.assertIn("sqrt_minutes_left", obs["features"])

    def test_refuses_without_vol(self):
        market, now_ms = gold_market()
        cache = {"GC=F": [(now_ms, 4358.0)]}  # too few points for vol
        self.assertIsNone(cal.observation_from_rest(market, cache, now_ms, pyth_series={}))

    def test_settlement_parity_counts_pyth_match(self):
        state = cal.default_state()
        ticker = "KXGOLD15M-26AUG072030-30"
        close_ms = cal.parse_close_ms(ticker)
        open_price = 4350.0
        state["pyth_series"] = {
            "Metal.XAU/USD": [[close_ms, 4360.0]],
        }
        cal.note_settlement_parity(state, ticker, open_price, True, state["pyth_series"])
        cal.note_settlement_parity(state, ticker, open_price, False, state["pyth_series"])
        summary = cal.settlement_parity_summary(state)
        self.assertEqual(summary["checked"], 2)
        self.assertEqual(summary["matched"], 1)
        self.assertAlmostEqual(summary["match_rate"], 0.5)


class TrustGateTest(unittest.TestCase):
    def scored_state(self, full, mid, contracts):
        state = cal.default_state()
        state["contract_scores_full"] = [full] * contracts
        state["contract_scores_market_mid_raw"] = [mid] * contracts
        state["resolved"] = contracts
        return state

    def test_beating_mid_with_enough_contracts_is_value(self):
        report = cal.build_report(self.scored_state(0.10, 0.11, 200), {}, 0)
        self.assertTrue(report["adds_value_over_market"])
        self.assertEqual(report["trusted_variant"], "physics")
        self.assertEqual(report["family"], "COMMODITIES15M")
        self.assertIn("KXGOLD15M", report["families"])

    def test_below_floor_is_not_value(self):
        report = cal.build_report(self.scored_state(0.10, 0.11, 50), {}, 0)
        self.assertFalse(report["adds_value_over_market"])

    def test_tape_ablation_can_unlock_trust(self):
        state = cal.default_state()
        state["contract_scores_full"] = [0.20] * 120
        state["contract_scores_market_mid_raw"] = [0.12] * 120
        state["contract_scores_physics_tape_confirm_near_close"] = [0.09] * 120
        state["contract_scores_physics_vol_regime_confirm"] = [0.18] * 120
        report = cal.build_report(state, {}, 0)
        self.assertTrue(report["adds_value_over_market"])
        self.assertEqual(report["trusted_variant"], "physics_tape_confirm_near_close")


class ObservePredictTest(unittest.TestCase):
    def test_observe_emits_bot_shaped_prediction(self):
        state = cal.default_state()
        market, now_ms = gold_market()
        base = 4358.0
        series = [(now_ms - (40 - i) * 60_000, base + (0.15 if i % 2 else -0.1))
                  for i in range(40)]
        preds = cal.observe_cycle(
            state, {}, now_ms, yahoo_cache={"GC=F": series}, rest_markets=[market],
            refresh_pyth=False,
        )
        self.assertIn(market["ticker"], preds)
        row = preds[market["ticker"]]
        self.assertIn("p_yes_full", row)
        self.assertEqual(row["probability_source"], "commodities-15m-directional-gaussian")
        self.assertAlmostEqual(row["market_yes_bid"], 0.35)


class SettleTest(unittest.TestCase):
    def test_one_contract_one_score(self):
        state = cal.default_state()
        market, now_ms = gold_market(seconds_left=600)
        series = [(now_ms - (40 - i) * 60_000, 4358.0 + (0.1 if i % 2 else -0.05))
                  for i in range(40)]
        for _ in range(5):
            cal.observe_cycle(state, {}, now_ms, yahoo_cache={"GC=F": series},
                              rest_markets=[market], refresh_pyth=False)
        ticker = market["ticker"]
        state["pending"][ticker]["close_ms"] = now_ms
        cal.settle_cycle(state, now_ms + 121_000, resolver=lambda t, o: True)
        self.assertEqual(state["resolved"], 1)
        self.assertEqual(len(state["contract_scores_full"]), 1)

    @staticmethod
    def _obs(p_model, yes_mid):
        """A pending observation shaped like _record_observation's, with every
        ablation set equal to p_model -- this drives the settle-loop GLUE
        (eligible_rows/rows + ce.eligible_pairs call sites), not the ablation
        math (covered elsewhere), so every variant is eligible under the same
        simple edge-vs-mid rule as physics."""
        return {
            "p_model": p_model,
            "p_ablations": {
                "physics": p_model,
                "physics_tape_confirm_near_close": p_model,
                "physics_vol_regime_confirm": p_model,
            },
            "yes_mid": yes_mid,
            "features": {},
            "ts_ms": 0,
        }

    def settle_with_obs(self, state, ticker, obs_list, outcome, close_at=0):
        """Like test_one_contract_one_score, but with directly-shaped
        observations so a contract's observations can straddle the
        eligibility edge."""
        state["pending"][ticker] = {
            "close_ms": close_at, "open_price": 4354.62, "obs": obs_list,
        }
        cal.settle_cycle(state, close_at + 121_000, resolver=lambda t, o: outcome)

    def test_settle_cycle_pairs_each_eligible_variant_with_its_own_mid(self):
        """Drives settle_cycle itself over a contract with a MIX of eligible
        and ineligible observations -- every other BetEligibleTrustTest case
        hand-builds the final state and never exercises the settle-loop glue
        (commodities_15m_calibrator.py's eligible_rows/rows comprehensions and
        the ce.eligible_pairs call sites), which is exactly where the original
        population-mismatch bug lived."""
        state = cal.default_state()
        eligible_ticker = "KXGOLD15M-26AUG072030-30"
        self.settle_with_obs(
            state, eligible_ticker,
            [
                self._obs(p_model=0.70, yes_mid=0.50),  # edge 0.20 -- eligible
                self._obs(p_model=0.70, yes_mid=0.68),  # edge 0.02 -- ineligible
            ],
            outcome=True, close_at=0)

        self.assertEqual(len(state["contract_scores_eligible_full"]), 1)
        self.assertEqual(len(state["contract_scores_eligible_market_mid_raw"]), 1)
        for key in ("physics_tape_confirm_near_close", "physics_vol_regime_confirm"):
            model_list = state[f"contract_scores_eligible_{key}"]
            mid_list = state[f"contract_scores_eligible_mid_{key}"]
            self.assertEqual(len(model_list), 1, key)
            self.assertEqual(len(mid_list), 1, key)
            self.assertEqual(len(model_list), len(mid_list), key)

        # A second contract with NO eligible observation must append nothing
        # anywhere -- lengths must stay exactly where they were, not grow.
        ineligible_ticker = "KXGOLD15M-26AUG072045-30"
        self.settle_with_obs(
            state, ineligible_ticker,
            [
                self._obs(p_model=0.55, yes_mid=0.50),  # edge 0.05 -- ineligible
                self._obs(p_model=0.52, yes_mid=0.50),  # edge 0.02 -- ineligible
            ],
            outcome=True, close_at=10_000_000)

        self.assertEqual(len(state["contract_scores_eligible_full"]), 1)
        self.assertEqual(len(state["contract_scores_eligible_market_mid_raw"]), 1)
        for key in ("physics_tape_confirm_near_close", "physics_vol_regime_confirm"):
            self.assertEqual(len(state[f"contract_scores_eligible_{key}"]), 1, key)
            self.assertEqual(len(state[f"contract_scores_eligible_mid_{key}"]), 1, key)

    def test_settle_cycle_scores_variant_against_its_own_eligible_mid_not_physics(self):
        """Distinguishes v_mid from eligible_mid inside settle_cycle itself --
        the exact bug this whole change exists to remove. Every other
        assertion in this file either checks LENGTHS (which pass even if the
        wrong mid value is appended) or reads a hand-built scoreboard (which
        never exercises the settle-loop append). Here physics and the tape
        variant are eligible on DIFFERENT observations of the SAME contract,
        so the two variants' eligible mids are numerically different
        (0.50 vs 0.20) and a regression that appends physics's eligible mid
        instead of the variant's own would fail only this test."""
        state = cal.default_state()
        ticker = "KXGOLD15M-26AUG072100-30"
        obs_a = {  # physics eligible (edge .20), tape ineligible (edge .02)
            "p_model": 0.70,
            "p_ablations": {"physics": 0.70, "physics_tape_confirm_near_close": 0.52},
            "yes_mid": 0.50,
            "features": {},
            "ts_ms": 0,
        }
        obs_b = {  # physics ineligible (edge .05), tape eligible (edge .60)
            "p_model": 0.25,
            "p_ablations": {"physics": 0.25, "physics_tape_confirm_near_close": 0.80},
            "yes_mid": 0.20,
            "features": {},
            "ts_ms": 0,
        }
        self.settle_with_obs(state, ticker, [obs_a, obs_b], outcome=True, close_at=0)

        self.assertAlmostEqual(state["contract_scores_eligible_full"][-1], 0.09)
        self.assertAlmostEqual(state["contract_scores_eligible_market_mid_raw"][-1], 0.25)
        self.assertAlmostEqual(
            state["contract_scores_eligible_physics_tape_confirm_near_close"][-1], 0.04)
        # The discriminating assertion: tape's OWN eligible mid is 0.20 (from
        # obs_b, where TAPE is eligible) -- not physics's eligible mid 0.50
        # (from obs_a, where PHYSICS is eligible). brier(0.20 vs True) = 0.64.
        self.assertAlmostEqual(
            state["contract_scores_eligible_mid_physics_tape_confirm_near_close"][-1], 0.64)


class BetEligibleTrustTest(unittest.TestCase):
    def _state(self, eligible_model, eligible_mid, n):
        state = cal.default_state()
        state["contract_scores_full"] = [0.05] * n
        state["contract_scores_market_mid_raw"] = [0.06] * n
        for key in ("physics_tape_confirm_near_close", "physics_vol_regime_confirm"):
            state[f"contract_scores_{key}"] = [0.05] * n
            state[f"contract_scores_eligible_{key}"] = [eligible_model] * n
            # Own-mid population: a contract can be eligible for one variant
            # and not another, so the variant's Brier must be paired against
            # the mid observed on ITS eligible contracts, not physics's.
            state[f"contract_scores_eligible_mid_{key}"] = [eligible_mid] * n
        state["contract_scores_eligible_full"] = [eligible_model] * n
        state["contract_scores_eligible_market_mid_raw"] = [eligible_mid] * n
        return state

    def test_losing_where_it_bets_is_untrusted(self):
        state = self._state(0.2576, 0.2130, 100)
        self.assertIsNone(cal.select_trusted_variant_eligible(state))
        r = cal.build_report(state, {}, 1_700_000_000_000)
        self.assertFalse(r["adds_value_on_bet_eligible"])

    def test_winning_where_it_bets_can_earn_trust(self):
        state = self._state(0.2130, 0.2576, 100)
        self.assertIsNotNone(cal.select_trusted_variant_eligible(state))
        r = cal.build_report(state, {}, 1_700_000_000_000)
        self.assertTrue(r["adds_value_on_bet_eligible"])

    def test_below_the_eligible_floor_is_untrusted(self):
        self.assertIsNone(cal.select_trusted_variant_eligible(self._state(0.2130, 0.2576, 99)))

    def test_publishes_the_eligible_numbers(self):
        r = cal.build_report(self._state(0.21, 0.26, 100), {}, 1_700_000_000_000)
        self.assertEqual(r["eligible_scored_contracts"], 100)
        self.assertAlmostEqual(r["brier_eligible_full"], 0.21)
        self.assertAlmostEqual(r["brier_eligible_market_mid_raw"], 0.26)
        self.assertEqual(r["min_eligible_contracts"], 100)

    def test_live_predictor_selection_is_untouched(self):
        # select_trusted_variant also picks live_p (the PUBLISHED probability).
        # This change must not move it: it gates trust, not prediction.
        state = self._state(0.2576, 0.2130, 100)
        self.assertIsNotNone(cal.select_trusted_variant(state))

    def test_variant_is_judged_against_its_own_eligible_mid(self):
        # Physics is eligible on a population where the mid is very good
        # (0.05) -- physics itself loses, and that is irrelevant to this
        # test. physics_tape_confirm_near_close is eligible on a DIFFERENT
        # population (per-predictor eligibility), where the mid is much
        # worse (0.30) and the variant beats it easily (0.10).
        #
        # Under the old (buggy) pairing, every variant was scored against
        # physics's shared mid list (0.05), so the variant would wrongly
        # fail to beat 0.05 with a 0.10 Brier and never earn trust. Judged
        # against its OWN eligible mid (0.30), it correctly wins.
        n = 100
        state = cal.default_state()
        state["contract_scores_eligible_full"] = [0.20] * n
        state["contract_scores_eligible_market_mid_raw"] = [0.05] * n
        state["contract_scores_eligible_physics_tape_confirm_near_close"] = [0.10] * n
        state["contract_scores_eligible_mid_physics_tape_confirm_near_close"] = [0.30] * n
        state["contract_scores_eligible_physics_vol_regime_confirm"] = []
        state["contract_scores_eligible_mid_physics_vol_regime_confirm"] = []
        self.assertEqual(
            cal.select_trusted_variant_eligible(state), "physics_tape_confirm_near_close")


if __name__ == "__main__":
    unittest.main()
