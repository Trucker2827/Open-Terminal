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


if __name__ == "__main__":
    unittest.main()
