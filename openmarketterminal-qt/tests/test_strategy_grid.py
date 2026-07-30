import os, sys, json, unittest, tempfile, datetime
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts", "research"))
import strategy_grid as sg  # noqa: E402

class GridTest(unittest.TestCase):
    def test_grid_is_pre_registered_and_ids_stable(self):
        g1 = sg.build_grid()
        g2 = sg.build_grid()
        ids = [v["variant_id"] for v in g1]
        self.assertEqual(len(ids), len(set(ids)))            # ids unique
        self.assertEqual([v["variant_id"] for v in g2], ids) # stable across calls
        # 2 sides * 6 bands * 3 gates * (1 hold + 5 tp + 4 sl + 3 trail) exits = 468
        self.assertEqual(len(g1), 2 * 6 * 3 * (1 + 5 + 4 + 3))
        v = g1[0]
        self.assertIn(v["side"], ("YES", "NO"))
        self.assertIn(v["gate"], ("mechanical", "physics", "calibrator"))

    def test_variant_id_encodes_the_tuple(self):
        v = {"side": "NO", "band": [0.10, 0.25], "gate": "physics",
             "exit": {"kind": "sl", "amount": 0.15}}
        vid = sg.variant_id(v)
        self.assertEqual(vid, "NO|b10-25|physics|sl15")

class SimTest(unittest.TestCase):
    def _path(self):
        # bet-side (ts_ms, bid, ask); price rises 0.20 -> 0.45 then settles
        return [(1000, 0.19, 0.20), (2000, 0.30, 0.31), (3000, 0.44, 0.45)]

    def test_take_profit_sells_at_bid_net_of_fees(self):
        path = self._path()
        # entered at ask 0.20 at i0=0; TP +0.20 -> first bid >= 0.40 is 0.44 @ t=3000
        r = sg.contract_pnl(path, 0, 0.20, {"kind": "tp", "amount": 0.20}, won=False)
        self.assertTrue(r["exited"])
        expected = (0.44 - 0.20) - sg.common.fee_per_contract(0.20) - sg.common.fee_per_contract(0.44)
        self.assertAlmostEqual(r["pnl"], expected, places=6)

    def test_hold_uses_settlement_not_a_sale(self):
        path = self._path()
        r = sg.contract_pnl(path, 0, 0.20, {"kind": "hold", "amount": None}, won=True)
        self.assertFalse(r["exited"])
        self.assertAlmostEqual(r["pnl"], (1.0 - 0.20) - sg.common.fee_per_contract(0.20), places=6)

    def test_stop_loss_triggers_on_decline(self):
        path = [(1000, 0.49, 0.50), (2000, 0.33, 0.34), (3000, 0.20, 0.21)]
        # entry ask 0.50; SL -0.15 -> first bid <= 0.35 is 0.33 @ t=2000
        r = sg.contract_pnl(path, 0, 0.50, {"kind": "sl", "amount": 0.15}, won=False)
        self.assertTrue(r["exited"])
        expected = (0.33 - 0.50) - sg.common.fee_per_contract(0.50) - sg.common.fee_per_contract(0.33)
        self.assertAlmostEqual(r["pnl"], expected, places=6)

    def test_bet_side_quotes_flips_for_NO(self):
        yes = [(1000, 0.60, 0.62)]
        self.assertEqual(sg.bet_side_quotes(yes, "NO"), [(1000, 1 - 0.62, 1 - 0.60)])

    def test_trailing_stop_exits_on_pullback_from_peak(self):
        # entry ask 0.30; bids rise to a 0.50 peak then pull back to 0.39;
        # trail -0.10 -> 0.39 <= 0.50-0.10 -> exit at 0.39 (taker at bid).
        path = [(1000, 0.34, 0.35), (2000, 0.50, 0.51), (3000, 0.39, 0.40)]
        r = sg.contract_pnl(path, 0, 0.30, {"kind": "trail", "amount": 0.10}, won=False)
        self.assertTrue(r["exited"])
        expected = (0.39 - 0.30) - sg.common.fee_per_contract(0.30) - sg.common.fee_per_contract(0.39)
        self.assertAlmostEqual(r["pnl"], expected, places=6)
        # shallow pullback that never reaches the trail distance -> holds to settlement
        path2 = [(1000, 0.34, 0.35), (2000, 0.50, 0.51), (3000, 0.45, 0.46)]
        r2 = sg.contract_pnl(path2, 0, 0.30, {"kind": "trail", "amount": 0.10}, won=True)
        self.assertFalse(r2["exited"])
        self.assertAlmostEqual(r2["pnl"], (1.0 - 0.30) - sg.common.fee_per_contract(0.30), places=6)

class GateTest(unittest.TestCase):
    def test_find_entry_first_touch_with_room(self):
        # bet-side asks: 0.30 (too little time), 0.08 (in 2-10 band, room), 0.06
        close = 10_000_000
        path = [(close - 60_000, 0.29, 0.30),     # 60s left < 120s floor -> skip
                (close - 300_000, 0.07, 0.08),    # 300s left, ask 0.08 in [0.02,0.10)
                (close - 200_000, 0.05, 0.06)]
        path.sort()
        got = sg.find_entry(path, 0.02, 0.10, close)
        self.assertIsNotNone(got)
        i0, ask = got
        self.assertAlmostEqual(ask, 0.06 if path[0][2] == 0.06 else 0.08)  # first in-band by time

    def test_gate_mechanical_always_true_calibrator_ungateable_is_none(self):
        class NoSig:
            def physics_ok(self, *a): return True
            def calibrator_ok(self, *a): return None   # no fresh prediction
        s = NoSig()
        self.assertTrue(sg.gate_ok("mechanical", "T", 1, "YES", s))
        self.assertTrue(sg.gate_ok("physics", "T", 1, "YES", s))
        self.assertIsNone(sg.gate_ok("calibrator", "T", 1, "YES", s))

    def test_find_entry_floor_rejects_in_band_ticks_without_room(self):
        close = 10_000_000
        # in-band (0.02-0.10) ticks exist ONLY inside the last 120s -> floor skips all -> no entry
        late_only = [(close - 90_000, 0.07, 0.08), (close - 30_000, 0.05, 0.06)]
        self.assertIsNone(sg.find_entry(late_only, 0.02, 0.10, close))
        # add one in-band tick WITH room (>120s) -> that tick is the entry
        with_room = sorted(late_only + [(close - 300_000, 0.07, 0.08)])
        got = sg.find_entry(with_room, 0.02, 0.10, close)
        self.assertIsNotNone(got)
        self.assertEqual(with_room[got[0]][0], close - 300_000)

class StatsTest(unittest.TestCase):
    def test_effective_n_collapses_correlated_clusters(self):
        # 10 values all in ONE cluster -> effective_n == 1
        r = sg.clustered_mean([0.1]*10, ["A"]*10)
        self.assertEqual(r["n"], 10)
        self.assertAlmostEqual(r["effective_n"], 1.0, places=6)
        # 10 values in 10 clusters -> effective_n == 10
        r2 = sg.clustered_mean([0.1]*10, [str(i) for i in range(10)])
        self.assertAlmostEqual(r2["effective_n"], 10.0, places=6)

    def test_benjamini_hochberg_rejects_expected_set(self):
        # Standard step-up BH at alpha=0.05, m=5. Thresholds (i/m)*alpha are
        # 0.01, 0.02, 0.03, 0.04, 0.05. p(1)=0.001<=0.01 and p(2)=0.008<=0.02,
        # but p(3)=0.039>0.03 and p(4)=0.041>0.04, so the largest passing rank is
        # 2 -> reject ranks 1,2 only. (The too-lenient (rank+1)/m bug would wrongly
        # give [T,T,T,T,F]; this pins the correct k/m formula.)
        pvals = [0.001, 0.008, 0.039, 0.041, 0.9]
        rej = sg.benjamini_hochberg(pvals, alpha=0.05)
        self.assertEqual(rej, [True, True, False, False, False])

    def test_score_variant_uses_two_nulls(self):
        recs = [{"pnl": 0.05, "hold_pnl": 0.00, "market_pnl": 0.00,
                 "won": True, "cluster": str(i), "close_ms": i} for i in range(40)]
        s = sg.score_variant(recs)
        self.assertAlmostEqual(s["delta_vs_hold"], 0.05, places=6)
        self.assertIn("delta_vs_market", s)
        self.assertGreaterEqual(s["clustered"]["effective_n"], 30)


class EndToEndTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self._prev = os.environ.get("OPENTERMINAL_EVIDENCE_DIR")
        os.environ["OPENTERMINAL_EVIDENCE_DIR"] = self.dir
    def tearDown(self):
        if self._prev is None: os.environ.pop("OPENTERMINAL_EVIDENCE_DIR", None)
        else: os.environ["OPENTERMINAL_EVIDENCE_DIR"] = self._prev

    def _write(self, name, rows):
        with open(os.path.join(self.dir, name), "w") as f:
            for r in rows: f.write(json.dumps(r) + "\n")

    def test_run_emits_versioned_artifacts_with_humility_fields(self):
        # one KXBTCD ticker, a short two-sided quote path, a recorded settlement, BRTI
        close = datetime.datetime(2026, 7, 27, 19, 0, tzinfo=datetime.timezone.utc)
        cms = int(close.timestamp() * 1000)
        tk = "KXBTCD-26JUL2719-T63999.99"
        self._write("kalshi-tickers.jsonl", [
            {"event": "kalshi_ticker", "market_ticker": tk, "ts_ms": cms - 600_000,
             "yes_bid_dollars": "0.20", "yes_ask_dollars": "0.22"},
            {"event": "kalshi_ticker", "market_ticker": tk, "ts_ms": cms - 300_000,
             "yes_bid_dollars": "0.40", "yes_ask_dollars": "0.42"}])
        self._write("kalshi-settlements.jsonl", [
            {"kalshi_market_id": tk, "result": "yes", "expiration_value": 64100.0}])
        self._write("kalshi-cf-benchmarks.jsonl", [
            {"id": "BRTI", "time": cms - 600_000, "value": 64000.0},
            {"id": "BRTI", "time": cms, "value": 64100.0}])
        full = sg.run(sg.common.evidence_dir())
        self.assertEqual(full["schema_version"], sg.SCHEMA_VERSION)
        self.assertIn("variants", full)
        # every variant record carries the humility fields
        v = full["variants"][0]
        for key in ("variant_id", "n_contracts", "effective_n", "delta_vs_hold",
                    "delta_vs_market", "survives_correction", "trust"):
            self.assertIn(key, v)
        latest = sg.latest_summary(full)
        self.assertEqual(latest["schema_version"], sg.SCHEMA_VERSION)
        self.assertIn("headline", latest)
        self.assertIsInstance(latest["survivors"], list)
        # tiny sample -> nothing survives (previously asserted a tautology:
        # survivors are filtered on trust=="measured" by construction)
        self.assertEqual(latest["survivors"], [])   # a tiny sample -> nothing survives

    def test_latest_summary_extracts_a_measured_survivor(self):
        # hand-built `full` with one measured variant, exercising the
        # non-empty path of latest_summary (never hit by real or seeded data).
        full = {"schema_version": sg.SCHEMA_VERSION, "as_of_utc": "2026-07-27T00:00:00+00:00",
                "variants": [
                    {"variant_id": "YES|b2-10|mechanical|hold", "side": "YES",
                     "band": [0.02, 0.10], "gate": "mechanical", "exit": {"kind": "hold", "amount": None},
                     "delta_vs_hold": 0.05, "delta_vs_market": 0.03, "effective_n": 40.0,
                     "ci95": [0.01, 0.09], "walkforward_delta": 0.02, "trust": "measured"},
                    {"variant_id": "NO|b50-75|physics|tp5", "side": "NO",
                     "band": [0.50, 0.75], "gate": "physics", "exit": {"kind": "tp", "amount": 0.05},
                     "delta_vs_hold": 0.0, "delta_vs_market": 0.0, "effective_n": 2.0,
                     "ci95": [None, None], "walkforward_delta": None, "trust": "insufficient_sample"},
                ]}
        latest = sg.latest_summary(full)
        self.assertEqual(latest["headline"], "1 variant beats hold+market after correction")
        self.assertEqual(len(latest["survivors"]), 1)
        s = latest["survivors"][0]
        self.assertEqual(s["variant_id"], "YES|b2-10|mechanical|hold")
        for key in ("side", "band", "gate", "exit", "delta_vs_hold", "delta_vs_market",
                    "effective_n", "ci95", "walkforward_delta", "trust"):
            self.assertIn(key, s)


class CandidateTest(unittest.TestCase):
    def _variant(self, vid, dh, dm, eff, survives, trust, p=0.5, wf=1.0):
        return {"variant_id": vid, "side": "NO", "band": [0.10, 0.25], "gate": "mechanical",
                "exit": {"kind": "sl", "amount": 0.15}, "delta_vs_hold": dh,
                "delta_vs_market": dm, "effective_n": eff, "ci95": [0.0, 0.0],
                "walkforward_delta": wf, "p_value": p, "survives_correction": survives,
                "trust": trust}

    def test_positive_near_miss_becomes_a_candidate_with_reason(self):
        full = {"schema_version": sg.SCHEMA_VERSION, "as_of_utc": "2026-07-30T00:00:00+00:00",
                "variants": [
                    # positive both nulls, enough sample, but NOT BH-significant -> candidate
                    self._variant("A", 0.02, 0.01, 40, False, "rejected", p=0.11),
                    # positive both nulls but effective_n < 30 -> candidate, effective_n reason
                    self._variant("B", 0.03, 0.02, 18, False, "insufficient_sample", p=0.5),
                    # NEGATIVE market null -> never a candidate
                    self._variant("C", 0.02, -0.01, 40, False, "rejected", p=0.11),
                    # too small a sample (<10) -> never a candidate
                    self._variant("D", 0.02, 0.01, 5, False, "insufficient_sample", p=0.5)]}
        latest = sg.latest_summary(full)
        ids = [c["variant_id"] for c in latest["candidates"]]
        self.assertEqual(ids, ["B", "A"])   # ranked by delta_vs_market desc (0.02, 0.01)
        by = {c["variant_id"]: c["blocked_by"] for c in latest["candidates"]}
        self.assertIn("effective_n", by["B"])            # 18 < 30
        self.assertIn("not significant", by["A"])        # BH
        self.assertNotIn("C", ids)
        self.assertNotIn("D", ids)
