import json
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


class SpotFallbackTest(unittest.TestCase):
    """Zero is not a price.

    Pyth published 0.0 for Metal.XTI/USD. Because 0.0 is not None it satisfied
    resolve_spot's `if spot is None` test and BLOCKED the Yahoo fallback, which
    had a good CL=F quote all along. KXWTIH skipped 9,520 markets as
    unmodelled, trained on nothing, and could never earn promotion.
    """

    def test_a_zero_pyth_price_is_unavailable_not_a_price(self):
        self.assertIsNone(stf.fetch_pyth_spot("dead-feed", fetcher=lambda url: {
            "parsed": [{"price": {"price": "0", "expo": -8}}]}))
        # A real price still comes through.
        self.assertAlmostEqual(stf.fetch_pyth_spot("live-feed", fetcher=lambda url: {
            "parsed": [{"price": {"price": "8313000000", "expo": -8}}]}), 83.13)

    def test_a_dead_pyth_feed_falls_back_to_yahoo(self):
        spec = stf.SeriesSpec(yahoo="CL=F", label="wti", spot_mode="pyth",
                              pyth_symbol="Metal.XTI/USD", pyth_id="dead")
        real = stf.fetch_pyth_spot
        stf.fetch_pyth_spot = lambda *a, **k: None      # the hardened return
        try:
            spot, vol, source = stf.resolve_spot(spec, {"CL=F": [(1, 83.14), (2, 83.13)]})
        finally:
            stf.fetch_pyth_spot = real
        self.assertGreater(spot, 0.0, "a dead primary must not leave spot at zero")
        self.assertIn("yahoo", source)


    def test_a_brti_source_returning_zero_also_falls_back(self):
        """The belt-and-braces guard, on the OTHER source path.

        Without its own test this guard passed a neuter — the fallback test
        above stubs the primary to return None and never reaches the `<= 0.0`
        branch, so it would have proved nothing about it.
        """
        spec = stf.SeriesSpec(yahoo="BTC=F", label="btc", spot_mode="brti")
        real = stf.latest_brti_spot
        stf.latest_brti_spot = lambda: 0.0      # present, but not a price
        try:
            spot, vol, source = stf.resolve_spot(spec, {"BTC=F": [(1, 64000.0), (2, 64100.0)]})
        finally:
            stf.latest_brti_spot = real
        self.assertGreater(spot, 0.0, "a zero primary must not survive as the spot")
        self.assertIn("yahoo", source)


class PerFamilySplitTest(unittest.TestCase):
    """Gold 15m, Silver 15m and Oil 15m are three prediction problems.

    You can count one apple and one orange as two objects; you cannot average
    their quality into one number and learn anything. These pin that each
    family carries its own model, its own evidence and its own verdict.
    """

    def _profile(self):
        tmp = tempfile.mkdtemp()
        return stf.Profile(
            event="test_split",
            family="TEST_SPLIT",
            series={
                "KXGOLDH": stf.SeriesSpec(yahoo="GC=F", label="gold", spot_mode="yahoo"),
                "KXSILVERH": stf.SeriesSpec(yahoo="SI=F", label="silver", spot_mode="yahoo"),
            },
            state_path=os.path.join(tmp, "state.json"),
            output_path=os.path.join(tmp, "out.json"),
            probability_source="test",
        )

    def test_each_family_gets_its_own_state_slice(self):
        profile = self._profile()
        state = stf.split_state({}, profile)
        self.assertEqual(sorted(state), ["KXGOLDH", "KXSILVERH"])
        self.assertIsNot(state["KXGOLDH"], state["KXSILVERH"])

    def test_updating_gold_cannot_change_silver(self):
        """The isolation property, asserted on the fitted model itself."""
        profile = self._profile()
        state = stf.split_state({}, profile)
        silver_before = json.dumps(state["KXSILVERH"], sort_keys=True)

        gold = state["KXGOLDH"]
        gold["contract_scores_full"] = [0.01] * 40
        gold["resolved"] = 40
        gold["full"]["n_seen"] = 999

        self.assertEqual(json.dumps(state["KXSILVERH"], sort_keys=True), silver_before)
        self.assertNotEqual(state["KXGOLDH"]["full"]["n_seen"],
                            state["KXSILVERH"]["full"].get("n_seen"))

    def test_a_legacy_pooled_state_never_seeds_a_family(self):
        """A gold+silver+oil model is not gold's model.

        Copying the pooled fit into each family would recreate exactly the
        contamination the split removes, while making the counters look
        healthy — the most dangerous possible outcome.
        """
        profile = self._profile()
        pooled = stf.default_state()
        pooled["resolved"] = 500
        pooled["contract_scores_full"] = [0.02] * 500
        pooled["full"]["n_seen"] = 33709

        state = stf.split_state(pooled, profile)
        for family in ("KXGOLDH", "KXSILVERH"):
            self.assertEqual(state[family]["resolved"], 0, family)
            self.assertEqual(state[family]["contract_scores_full"], [], family)
            self.assertNotEqual(state[family]["full"].get("n_seen"), 33709, family)

    def test_a_single_family_state_is_adopted_not_reset(self):
        """A profile that was never pooled must keep its history.

        Its flat state already IS that family's own evidence. Resetting it
        would destroy legitimate history to fix a problem it never had —
        measured the hard way: kxbtc-daily (KXBTC only) lost resolved=4 /
        n_seen=167 to exactly this before the guard existed.
        """
        tmp = tempfile.mkdtemp()
        solo = stf.Profile(
            event="test_solo",
            family="TEST_SOLO",
            series={"KXBTC": stf.SeriesSpec(yahoo="BTC=F", label="btc", spot_mode="yahoo")},
            state_path=os.path.join(tmp, "state.json"),
            output_path=os.path.join(tmp, "out.json"),
            probability_source="test",
        )
        flat = stf.default_state()
        flat["resolved"] = 4
        flat["contract_scores_full"] = [0.11] * 4
        flat["full"]["n_seen"] = 167

        state = stf.split_state(flat, solo)
        self.assertEqual(state["KXBTC"]["resolved"], 4)
        self.assertEqual(len(state["KXBTC"]["contract_scores_full"]), 4)
        self.assertEqual(state["KXBTC"]["full"]["n_seen"], 167)

    def test_per_family_totals_reconcile_with_the_pooled_diagnostic(self):
        profile = self._profile()
        state = stf.split_state({}, profile)
        state["KXGOLDH"]["contract_scores_full"] = [0.1] * 30
        state["KXGOLDH"]["resolved"] = 30
        state["KXSILVERH"]["contract_scores_full"] = [0.2] * 70
        state["KXSILVERH"]["resolved"] = 70

        pooled = stf.pooled_view(state)
        self.assertEqual(len(pooled["contract_scores_full"]), 100)
        self.assertEqual(pooled["resolved"], 100)

    def test_a_single_family_report_earns_its_own_trust(self):
        """Narrowed to one series, a family is no longer 'pooled' and its
        verdict is its own — which is the entire point of the split."""
        profile = self._profile()
        sub = stf.family_profile(profile, "KXGOLDH")
        self.assertEqual(list(sub.series), ["KXGOLDH"])

        state = stf.default_state()
        state["contract_scores_full"] = [0.05] * 500
        state["contract_scores_market_mid_raw"] = [0.30] * 500
        state["contract_scores_eligible_full"] = [0.05] * 500
        state["contract_scores_eligible_market_mid_raw"] = [0.30] * 500
        report = stf.build_report(state, {}, 1, sub)

        self.assertEqual(report["families"], ["KXGOLDH"])
        self.assertTrue(report["adds_value_on_bet_eligible"])
        self.assertNotIn("pooled_trust_withheld", report)


class QuoteCaptureTest(unittest.TestCase):
    """The spread must be recoverable from a settled contract.

    A calibration gap measured against the mid is not an edge until you know
    whether it survives the spread: you harvest an overpriced longshot by
    SELLING it, and you sell at the bid, not the mid. The commodity books are
    thin, so the spread can plausibly exceed the whole gap. These tests pin the
    quotes into the settled record so that question is answerable.
    """

    def _profile(self):
        return stf.Profile(
            event="test_quote_capture",
            family="TEST",
            series={"KXGOLDD": stf.SeriesSpec(yahoo="GC=F", label="gold", spot_mode="yahoo")},
            state_path=os.path.join(tempfile.mkdtemp(), "state.json"),
            output_path=os.path.join(tempfile.mkdtemp(), "out.json"),
            probability_source="test",
        )

    def _observe(self, state, now_ms, bid, ask):
        stf.observe_cycle(
            state,
            self._profile(),
            now_ms,
            rest_markets=[gold_daily_market(now_ms, yes_bid=bid, yes_ask=ask)],
            yahoo_cache={"GC=F": [(now_ms - 60_000, 4490.0), (now_ms, 4510.0)]},
        )

    TICKER = "KXGOLDD-26AUG1017-T4500"

    def test_each_observation_carries_the_quotes_beside_it(self):
        now_ms = 1_784_900_000_000
        state = stf.default_state()
        self._observe(state, now_ms, 0.40, 0.50)
        self._observe(state, now_ms + 1000, 0.11, 0.19)
        entry = state["pending"][self.TICKER]
        self.assertEqual(len(entry["books"]), len(entry["obs"]),
                         "books must stay index-aligned with observations")
        self.assertEqual(entry["books"][0]["market_yes_bid"], 0.40)
        self.assertEqual(entry["books"][0]["market_yes_ask"], 0.50)
        self.assertEqual(entry["books"][1]["market_yes_bid"], 0.11)
        self.assertEqual(entry["books"][1]["market_yes_ask"], 0.19)

    def test_the_spread_is_recoverable_from_a_settled_contract(self):
        now_ms = 1_784_900_000_000
        state = stf.default_state()
        self._observe(state, now_ms, 0.11, 0.19)
        close_ms = state["pending"][self.TICKER]["close_ms"]
        stf.settle_cycle(state, close_ms + 200_000, resolver=lambda t: 0.0)
        self.assertEqual(len(state["resolved_record"]), 1)
        record = state["resolved_record"][0]
        self.assertIn("books", record)
        book = record["books"][0]
        # mid 0.15, but you would have SOLD at 0.11 -- an 8-point round trip
        # that a mid-only record makes invisible.
        self.assertAlmostEqual(record["observations"][0]["yes_mid"], 0.15)
        self.assertAlmostEqual(book["market_yes_ask"] - book["market_yes_bid"], 0.08)

    def test_a_settled_contract_keeps_its_identity(self):
        now_ms = 1_784_900_000_000
        state = stf.default_state()
        self._observe(state, now_ms, 0.40, 0.50)
        close_ms = state["pending"][self.TICKER]["close_ms"]
        stf.settle_cycle(state, close_ms + 200_000, resolver=lambda t: 1.0)
        record = state["resolved_record"][0]
        self.assertEqual(record["ticker"], self.TICKER)
        self.assertEqual(record["event_ticker"], "KXGOLDD-26AUG1017")

    def test_a_pending_entry_written_before_capture_is_padded_not_misaligned(self):
        now_ms = 1_784_900_000_000
        state = stf.default_state()
        self._observe(state, now_ms, 0.40, 0.50)
        # Simulate state on disk from before quote capture existed.
        del state["pending"][self.TICKER]["books"]
        self._observe(state, now_ms + 1000, 0.11, 0.19)
        entry = state["pending"][self.TICKER]
        self.assertEqual(len(entry["books"]), len(entry["obs"]))
        self.assertEqual(entry["books"][0], {}, "the un-captured observation stays empty")
        self.assertEqual(entry["books"][1]["market_yes_bid"], 0.11,
                         "the new quotes must land on the observation they describe")

    def test_quotes_never_enter_the_feature_vector(self):
        now_ms = 1_784_900_000_000
        state = stf.default_state()
        self._observe(state, now_ms, 0.40, 0.50)
        features = state["pending"][self.TICKER]["obs"][0]
        for key in ("market_yes_bid", "market_yes_ask"):
            self.assertNotIn(key, features,
                             "a model that trained on the spread would be reading "
                             "the answer out of the question")


if __name__ == "__main__":
    unittest.main()
