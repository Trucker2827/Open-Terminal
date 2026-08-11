import os
import sys
import tempfile
import unittest

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise"))
sys.path.insert(0, SCRIPTS)
import strike_threshold_family as stf


def gold_daily_market(now_ms, floor=4500.0, yes_bid=0.40, yes_ask=0.50):
    close = (now_ms + 6 * 3600 * 1000) / 1000.0
    import datetime

    close_iso = datetime.datetime.utcfromtimestamp(close).strftime("%Y-%m-%dT%H:%M:%SZ")
    return {
        "ticker": "KXGOLDD-26AUG1017-T4500",
        "floor_strike": floor,
        "cap_strike": None,
        "yes_bid_dollars": yes_bid,
        "yes_ask_dollars": yes_ask,
        "close_time": close_iso,
        "yes_bid_size": 10,
        "yes_ask_size": 12,
    }


class SeriesChipsTest(unittest.TestCase):
    def test_hourly_daily_kxbtc_series(self):
        self.assertTrue(stf.is_profile_ticker("KXGOLDH-26AUG0716-T4377", stf.COMMODITIES_HOURLY))
        self.assertTrue(stf.is_profile_ticker("KXWTI-26AUG1114-T84.99", stf.COMMODITIES_DAILY))
        self.assertTrue(stf.is_profile_ticker("KXBTC-26AUG0916-T73799.99", stf.KXBTC_DAILY))
        self.assertFalse(stf.is_profile_ticker("KXGOLD15M-26AUG080000-00", stf.COMMODITIES_DAILY))
        self.assertFalse(stf.is_profile_ticker("KXBTCD-26AUG0915-T73799.99", stf.KXBTC_DAILY))


class ObserveTest(unittest.TestCase):
    def test_floor_only_market_produces_bot_shaped_prediction(self):
        now_ms = 1_784_900_000_000
        profile = stf.Profile(
            event="test_commod_daily",
            family="TEST",
            series={
                "KXGOLDD": stf.SeriesSpec(yahoo="GC=F", label="gold", spot_mode="yahoo"),
            },
            state_path=os.path.join(tempfile.mkdtemp(), "state.json"),
            output_path=os.path.join(tempfile.mkdtemp(), "out.json"),
            probability_source="test",
        )
        state = stf.default_state()
        yahoo_cache = {"GC=F": [(now_ms - 60_000, 4490.0), (now_ms, 4510.0)]}
        preds = stf.observe_cycle(
            state,
            profile,
            now_ms,
            rest_markets=[gold_daily_market(now_ms)],
            yahoo_cache=yahoo_cache,
        )
        self.assertIn("KXGOLDD-26AUG1017-T4500", preds)
        row = preds["KXGOLDD-26AUG1017-T4500"]
        self.assertIn("p_yes_full", row)
        self.assertAlmostEqual(row["market_yes_mid"], 0.45)
        self.assertEqual(row["probability_source"], "test")

    def test_range_cap_is_skipped(self):
        now_ms = 1_784_900_000_000
        profile = stf.Profile(
            event="test",
            family="TEST",
            series={"KXGOLDD": stf.SeriesSpec(yahoo="GC=F", label="gold", spot_mode="yahoo")},
            state_path="/tmp/x-state.json",
            output_path="/tmp/x-out.json",
            probability_source="test",
        )
        market = gold_daily_market(now_ms)
        market["cap_strike"] = 4600.0
        state = stf.default_state()
        preds = stf.observe_cycle(
            state,
            profile,
            now_ms,
            rest_markets=[market],
            yahoo_cache={"GC=F": [(now_ms, 4510.0)]},
        )
        self.assertEqual(preds, {})
        self.assertGreaterEqual(state["skipped_unmodeled"], 1)


class TrustReportTest(unittest.TestCase):
    def test_build_report_fail_closed_below_floor(self):
        state = stf.default_state()
        state["contract_scores_full"] = [0.1] * 50
        state["contract_scores_market_mid_raw"] = [0.2] * 50
        report = stf.build_report(state, {}, 1, stf.COMMODITIES_DAILY)
        self.assertFalse(report["adds_value_over_market"])
        self.assertEqual(report["family"], "COMMODITIES_DAILY")
        self.assertIn("KXWTI", report["families"])

    def test_pooled_evidence_never_authorises_a_bid(self):
        """A multi-series profile must not publish an authorising trust flag.

        Measured 2026-08-11: commodities-hourly pooled KXGOLDH + KXSILVERH +
        KXWTIH over 500 contracts, published adds_value_on_bet_eligible=true,
        and authorised bids in all three underlyings from evidence no single
        one of them had earned. Numbers here are chosen so the POOLED verdict
        would be true -- the point is that it is withheld anyway.
        """
        state = stf.default_state()
        state["contract_scores_full"] = [0.05] * 500
        state["contract_scores_market_mid_raw"] = [0.30] * 500
        state["contract_scores_eligible_full"] = [0.05] * 500
        state["contract_scores_eligible_market_mid_raw"] = [0.30] * 500
        report = stf.build_report(state, {}, 1, stf.COMMODITIES_HOURLY)

        self.assertGreater(len(report["families"]), 1, "fixture must be a pooled profile")
        # The pooled computation genuinely says "adds value"...
        self.assertTrue(report["pooled_adds_value_over_market"])
        self.assertTrue(report["pooled_adds_value_on_bet_eligible"])
        # ...and it is withheld from every authorising field.
        self.assertFalse(report["adds_value_over_market"])
        self.assertFalse(report["adds_value_on_bet_eligible"])
        # Withheld is not the same claim as "measured to have no edge".
        self.assertTrue(report["pooled_trust_withheld"])
        self.assertEqual(report["pooled_families"], report["families"])

    def test_single_series_profile_still_earns_trust(self):
        """The rule must bite only on POOLED evidence.

        Withholding trust from a single-series profile would be a different
        bug -- it would stop a family that legitimately earned its own
        evidence, and would make the pooled fix look like a global kill.
        """
        state = stf.default_state()
        state["contract_scores_full"] = [0.05] * 500
        state["contract_scores_market_mid_raw"] = [0.30] * 500
        state["contract_scores_eligible_full"] = [0.05] * 500
        state["contract_scores_eligible_market_mid_raw"] = [0.30] * 500
        report = stf.build_report(state, {}, 1, stf.KXBTC_DAILY)

        self.assertEqual(len(report["families"]), 1, "fixture must be a single-series profile")
        self.assertTrue(report["adds_value_over_market"])
        self.assertTrue(report["adds_value_on_bet_eligible"])
        self.assertNotIn("pooled_trust_withheld", report)


if __name__ == "__main__":
    unittest.main()
