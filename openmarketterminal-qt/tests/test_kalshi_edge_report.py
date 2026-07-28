"""The lessons artifact's reducer (issue #174).

`kalshi_edge_report.reduce()` is where a canned verdict would hide: it turns
#169's four measurements into four short conclusions, and if it ever returned
the 2026-07-27 report's answers rather than THIS run's, nothing downstream
could tell. So every test here feeds synthetic payloads and asserts that the
verdict follows the numbers — including, for each question, a payload shaped
like the published report (which must reproduce the report's verdict) AND a
payload with the numbers moved the other way (which must NOT).

No evidence directory, no subprocess, no clock: `reduce()` is pure, which is
what makes that possible.
"""
import os
import sys
import unittest

RESEARCH = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..",
                                        "scripts", "research"))
sys.path.insert(0, RESEARCH)
import kalshi_edge_report as ker  # noqa: E402

NOW_MS = 1_785_000_000_000
Q1, Q2, Q3, Q4 = ker.QUESTIONS


def horizon(seconds=15, drift=0.0207, t_stat=2.50, half_spread=0.0053, fee=0.0155, n=44):
    return {"seconds": seconds, "n": n, "mean_aligned_mid_drift": drift, "t_stat": t_stat,
            "mean_half_spread": half_spread, "mean_fee": fee,
            "mean_cost_to_take": half_spread + fee,
            "net_of_cost_taking": drift - half_spread - fee,
            "net_of_fee_only": drift - fee}


def q1_payload(strata=None, hours=8.2):
    """Shaped like the published Q1: 3σ / 8 events, 2σ / 22 events."""
    if strata is None:
        strata = [
            {"threshold_sigma": 2.0,
             "contested_markets": {"events": 22, "markets": 56,
                                   "horizons": [horizon(5, 0.0053, 2.42, n=116),
                                                horizon(15, 0.0122, 3.52, fee=0.0152, n=116)]}},
            {"threshold_sigma": 3.0,
             "contested_markets": {"events": 8, "markets": 28,
                                   "horizons": [horizon(15, 0.0207, 2.50, n=44)]}},
        ]
    return {"question": "Q1", "by_threshold": strata,
            "data": {"paired_window_utc": ["2026-07-27T13:42:00+00:00",
                                           "2026-07-27T21:52:00+00:00"],
                     "paired_window_hours": hours}}


def q2_payload(contracts=239, calibrated=0.1043, market=0.0989, rows=5011):
    delta = (None if calibrated is None or market is None else calibrated - market)
    return {"question": "Q2",
            "contract_span_utc": ["2026-07-24T15:00:00+00:00", "2026-07-27T21:00:00+00:00"],
            "overall": {"rows": rows, "contracts": contracts,
                        "brier_calibrated": calibrated, "brier_market_mid": market,
                        "delta_vs_market": delta,
                        "beats_market": bool(delta is not None and delta < 0)}}


def q3_payload(contracts=239, platt=-0.0074, isotonic=-0.0096, versus_market=0.0121):
    """`versus_market` is corrected-minus-mid: POSITIVE means still worse than
    the mid, which is what the published record shows."""
    return {"question": "Q3",
            "contract_span_utc": ["2026-07-24T15:00:00+00:00", "2026-07-27T21:00:00+00:00"],
            "walk_forward": {"aggregate": {"folds": 4, "pooled_test_contracts": contracts,
                                           "pooled_test_rows": 3000,
                                           "platt_gain_vs_raw": platt,
                                           "isotonic_gain_vs_raw": isotonic,
                                           "best_corrected_vs_market": versus_market}}}


def q4_payload(settled=15, net=-7.03, fees=0.71, wins=8, losses=7, required=300):
    return {"question": "Q4",
            "data": {"rows": 87801,
                     "span_utc": ["2026-07-24T15:48:00+00:00", "2026-07-27T21:40:00+00:00"]},
            "gate": {"available": True, "params": {"min_settled_bids": required}},
            "outcome_summary": {"settled": settled, "wins": wins, "losses": losses,
                                "net_realized_pnl_usd": net, "fees_usd": fees,
                                "gross_stake_usd": 25.32,
                                "win_rate": (wins / settled) if settled else None}}


def lessons_of(**payloads):
    """reduce() over a mapping of question id -> payload, keyed by id."""
    results = {qid: {"payload": p} for qid, p in payloads.items()}
    report = ker.reduce(results, NOW_MS)
    return {lesson["id"]: lesson for lesson in report["lessons"]}, report


class Q1Test(unittest.TestCase):
    def test_published_shape_reproduces_the_reports_fee_eaten_verdict(self):
        lesson = ker.reduce_q1(Q1, q1_payload())
        self.assertEqual(lesson["verdict"], ker.FEE_EATEN)
        # The 2σ stratum decides: 22 independent moves clears the floor, 8 does not.
        self.assertEqual(lesson["sample"]["n"], 22)
        self.assertIn("2.0", lesson["sample"]["unit"])

    def test_the_highest_sigma_with_enough_events_is_the_one_that_decides(self):
        payload = q1_payload()
        payload["by_threshold"][1]["contested_markets"]["events"] = 40   # 3σ now qualifies
        lesson = ker.reduce_q1(Q1, payload)
        self.assertEqual(lesson["sample"]["n"], 40)
        self.assertIn("3.0", lesson["sample"]["unit"])

    def test_a_drift_that_clears_its_cost_is_an_edge(self):
        # Same shape, one number moved: drift large enough to clear spread+fee.
        payload = q1_payload(strata=[
            {"threshold_sigma": 3.0,
             "contested_markets": {"events": 30, "markets": 28,
                                   "horizons": [horizon(15, 0.0500, 4.10, n=90)]}}])
        lesson = ker.reduce_q1(Q1, payload)
        self.assertEqual(lesson["verdict"], ker.EDGE)

    def test_an_insignificant_drift_is_no_edge_whatever_its_size(self):
        payload = q1_payload(strata=[
            {"threshold_sigma": 3.0,
             "contested_markets": {"events": 30, "markets": 28,
                                   "horizons": [horizon(15, 0.0500, 0.40, n=90)]}}])
        lesson = ker.reduce_q1(Q1, payload)
        self.assertEqual(lesson["verdict"], ker.NO_EDGE)

    def test_a_significant_drift_the_wrong_way_is_no_edge(self):
        payload = q1_payload(strata=[
            {"threshold_sigma": 3.0,
             "contested_markets": {"events": 30, "markets": 28,
                                   "horizons": [horizon(15, -0.0300, -3.10, n=90)]}}])
        lesson = ker.reduce_q1(Q1, payload)
        self.assertEqual(lesson["verdict"], ker.NO_EDGE)

    def test_too_few_independent_moves_is_insufficient_data_not_a_verdict(self):
        payload = q1_payload(strata=[
            {"threshold_sigma": 3.0,
             "contested_markets": {"events": 4, "markets": 9,
                                   "horizons": [horizon(15, 0.0900, 9.90, n=12)]}}])
        lesson = ker.reduce_q1(Q1, payload)
        self.assertEqual(lesson["verdict"], ker.INSUFFICIENT_DATA)
        self.assertIsNone(lesson["sample"]["n"])
        # Even a spectacular number does not get published as an edge.
        self.assertNotIn("EDGE", lesson["claim"])

    def test_a_missing_15s_horizon_is_insufficient_data(self):
        payload = q1_payload(strata=[
            {"threshold_sigma": 3.0,
             "contested_markets": {"events": 30, "markets": 28,
                                   "horizons": [horizon(60, 0.0500, 4.10, n=90)]}}])
        self.assertEqual(ker.reduce_q1(Q1, payload)["verdict"], ker.INSUFFICIENT_DATA)

    def test_the_span_is_carried_from_the_paired_window(self):
        lesson = ker.reduce_q1(Q1, q1_payload())
        self.assertEqual(lesson["data_span"]["hours"], 8.2)
        self.assertIn("8.2h", lesson["data_span"]["text"])


class Q2Test(unittest.TestCase):
    def test_published_shape_reproduces_the_reports_no_edge_verdict(self):
        lesson = ker.reduce_q2(Q2, q2_payload())
        self.assertEqual(lesson["verdict"], ker.NO_EDGE)
        self.assertEqual(lesson["sample"]["n"], 239)

    def test_the_sample_is_contracts_and_never_the_row_count(self):
        lesson = ker.reduce_q2(Q2, q2_payload(contracts=239, rows=5011))
        self.assertEqual(lesson["sample"]["n"], 239)
        self.assertNotEqual(lesson["sample"]["n"], 5011)
        self.assertIn("contracts", lesson["sample"]["unit"])

    def test_beating_the_mid_is_an_edge(self):
        lesson = ker.reduce_q2(Q2, q2_payload(calibrated=0.0900, market=0.0989))
        self.assertEqual(lesson["verdict"], ker.EDGE)

    def test_below_the_preregistered_contract_floor_is_insufficient_data(self):
        lesson = ker.reduce_q2(Q2, q2_payload(contracts=ker.MIN_SCORED_CONTRACTS - 1,
                                              calibrated=0.0100, market=0.5000))
        self.assertEqual(lesson["verdict"], ker.INSUFFICIENT_DATA)

    def test_an_unscoreable_brier_is_insufficient_data_not_zero(self):
        lesson = ker.reduce_q2(Q2, q2_payload(calibrated=None))
        self.assertEqual(lesson["verdict"], ker.INSUFFICIENT_DATA)
        self.assertIn("unmeasured", [n["text"] for n in lesson["key_numbers"]])


class Q3Test(unittest.TestCase):
    def test_published_shape_reproduces_the_reports_no_edge_verdict(self):
        lesson = ker.reduce_q3(Q3, q3_payload())
        self.assertEqual(lesson["verdict"], ker.NO_EDGE)
        self.assertIn("out-of-sample", lesson["sample"]["unit"])

    def test_a_gain_that_still_loses_to_the_mid_is_not_an_edge(self):
        # The live record on 2026-07-28 looks exactly like this: Platt gains
        # +0.0017 out of sample while the corrected score is still +0.0015
        # WORSE than the raw market mid. Improving a losing signal is not edge.
        lesson = ker.reduce_q3(Q3, q3_payload(platt=0.0017, isotonic=-0.0004,
                                              versus_market=0.0015))
        self.assertEqual(lesson["verdict"], ker.NO_EDGE)
        self.assertIn("does not carry it past the market mid", lesson["claim"])

    def test_a_gain_that_beats_the_mid_is_an_edge(self):
        self.assertEqual(
            ker.reduce_q3(Q3, q3_payload(platt=0.0031, versus_market=-0.0040))["verdict"],
            ker.EDGE)

    def test_the_better_corrector_decides(self):
        # Isotonic gains where Platt loses, and the corrected score clears the mid.
        self.assertEqual(
            ker.reduce_q3(Q3, q3_payload(platt=-0.02, isotonic=0.004,
                                         versus_market=-0.001))["verdict"],
            ker.EDGE)

    def test_a_gain_with_no_market_comparison_is_not_an_edge(self):
        lesson = ker.reduce_q3(Q3, q3_payload(platt=0.0031, versus_market=None))
        self.assertEqual(lesson["verdict"], ker.NO_EDGE)
        self.assertIn("unmeasured against", lesson["verdict_reason"])

    def test_a_short_walk_forward_is_insufficient_data(self):
        lesson = ker.reduce_q3(Q3, q3_payload(contracts=12, platt=0.5))
        self.assertEqual(lesson["verdict"], ker.INSUFFICIENT_DATA)

    def test_a_walk_forward_that_never_ran_is_insufficient_data(self):
        lesson = ker.reduce_q3(Q3, {"walk_forward": {"folds": [], "note": "insufficient"}})
        self.assertEqual(lesson["verdict"], ker.INSUFFICIENT_DATA)


class Q4Test(unittest.TestCase):
    def test_published_shape_is_insufficient_data_at_fifteen_positions(self):
        lesson = ker.reduce_q4(Q4, q4_payload())
        self.assertEqual(lesson["verdict"], ker.INSUFFICIENT_DATA)
        self.assertEqual(lesson["sample"]["n"], 15)
        self.assertIn("settled positions", lesson["sample"]["unit"])
        # The measured P&L is still stated — unanswerable is not silent.
        self.assertIn("-7.03", lesson["verdict_reason"])

    def test_the_floor_is_the_sealed_gates_own_minimum(self):
        # Same 15 positions, a gate that only asks for 10: now answerable.
        lesson = ker.reduce_q4(Q4, q4_payload(required=10))
        self.assertEqual(lesson["verdict"], ker.NO_EDGE)

    def test_a_profitable_record_over_the_floor_is_an_edge(self):
        lesson = ker.reduce_q4(Q4, q4_payload(settled=400, net=42.0, fees=9.0))
        self.assertEqual(lesson["verdict"], ker.EDGE)

    def test_winning_before_fees_and_losing_after_is_fee_eaten(self):
        lesson = ker.reduce_q4(Q4, q4_payload(settled=400, net=-3.0, fees=9.0))
        self.assertEqual(lesson["verdict"], ker.FEE_EATEN)

    def test_losing_before_fees_too_is_no_edge(self):
        lesson = ker.reduce_q4(Q4, q4_payload(settled=400, net=-30.0, fees=9.0))
        self.assertEqual(lesson["verdict"], ker.NO_EDGE)

    def test_a_gate_with_no_sealed_minimum_falls_back_to_the_charters(self):
        payload = q4_payload(settled=299)
        payload["gate"] = {"available": False, "error": "no such file"}
        lesson = ker.reduce_q4(Q4, payload)
        self.assertEqual(lesson["verdict"], ker.INSUFFICIENT_DATA)
        self.assertIn(str(ker.Q4_FALLBACK_MIN_SETTLED), lesson["verdict_reason"])


class ReduceTest(unittest.TestCase):
    def test_every_question_appears_exactly_once_in_order(self):
        _, report = lessons_of(Q1=q1_payload(), Q2=q2_payload(), Q3=q3_payload(),
                               Q4=q4_payload())
        self.assertEqual([l["id"] for l in report["lessons"]], ["Q1", "Q2", "Q3", "Q4"])

    def test_a_question_that_did_not_run_is_insufficient_data_carrying_its_reason(self):
        report = ker.reduce({"Q1": {"error": "q1_quote_lag.py did not finish within 900s"},
                             "Q2": {"payload": q2_payload()}}, NOW_MS)
        lessons = {l["id"]: l for l in report["lessons"]}
        self.assertEqual(lessons["Q1"]["verdict"], ker.INSUFFICIENT_DATA)
        self.assertIn("did not finish", lessons["Q1"]["verdict_reason"])
        # The other three are still present, and Q2 still has its conclusion.
        self.assertEqual(len(report["lessons"]), len(ker.QUESTIONS))
        self.assertEqual(lessons["Q2"]["verdict"], ker.NO_EDGE)
        self.assertEqual(lessons["Q3"]["verdict"], ker.INSUFFICIENT_DATA)

    def test_an_unreadable_payload_is_insufficient_data_not_a_crash(self):
        report = ker.reduce({"Q1": {"payload": {"by_threshold": "not a list"}}}, NOW_MS)
        lessons = {l["id"]: l for l in report["lessons"]}
        self.assertEqual(lessons["Q1"]["verdict"], ker.INSUFFICIENT_DATA)

    def test_the_artifact_is_dated_and_declares_its_vocabulary(self):
        _, report = lessons_of(Q1=q1_payload())
        self.assertEqual(report["generated_at_ms"], NOW_MS)
        self.assertTrue(report["generated_at"].startswith("20"))
        self.assertEqual(set(report["verdicts"]), set(ker.VERDICTS))
        self.assertEqual(report["schema"], ker.SCHEMA)

    def test_every_lesson_carries_a_sample_size_and_a_legal_verdict(self):
        lessons, _ = lessons_of(Q1=q1_payload(), Q2=q2_payload(), Q3=q3_payload(),
                                Q4=q4_payload())
        for lesson in lessons.values():
            self.assertIn(lesson["verdict"], ker.VERDICTS)
            self.assertIn("unit", lesson["sample"])
            self.assertTrue(lesson["claim"])
            self.assertTrue(lesson["verdict_reason"])
            # Never a row count: no unit in this artifact is measured in rows.
            self.assertNotIn("row", lesson["sample"]["unit"])

    def test_the_verdicts_are_derived_not_canned(self):
        """The whole point. Same four questions, numbers moved the other way:
        every verdict must move with them."""
        published, _ = lessons_of(Q1=q1_payload(), Q2=q2_payload(), Q3=q3_payload(),
                                  Q4=q4_payload())
        self.assertEqual([published[q]["verdict"] for q in ("Q1", "Q2", "Q3", "Q4")],
                         [ker.FEE_EATEN, ker.NO_EDGE, ker.NO_EDGE, ker.INSUFFICIENT_DATA])
        moved, _ = lessons_of(
            Q1=q1_payload(strata=[{"threshold_sigma": 3.0,
                                   "contested_markets": {
                                       "events": 30, "markets": 28,
                                       "horizons": [horizon(15, 0.0500, 4.10, n=90)]}}]),
            Q2=q2_payload(calibrated=0.0900),
            Q3=q3_payload(platt=0.0031, versus_market=-0.0040),
            Q4=q4_payload(settled=400, net=42.0))
        self.assertEqual([moved[q]["verdict"] for q in ("Q1", "Q2", "Q3", "Q4")],
                         [ker.EDGE, ker.EDGE, ker.EDGE, ker.EDGE])


if __name__ == "__main__":
    unittest.main()
