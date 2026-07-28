"""The Q1 regeneration's own arithmetic (issue #179).

`f1_quote_lag_strata.py` re-measures the #169 quote-lag table on the retained
lag-series window. Everything it says about the DATA comes from `q1_quote_lag`,
imported unmodified; what it adds is three things that can be wrong quietly, and
each is tested here against a hand-computable case rather than against a run:

  1. the MOVE-CLUSTERED interval. Its whole purpose is to stop markets that
     quoted the same spot move from counting as independent evidence, so the
     test that matters is the one where they do move together: 100 samples in
     two clusters must produce a standard error ten times the naive one, and a
     slice with a single move must produce NO interval at all rather than one
     that shrinks with the number of markets. A regression here would silently
     restore exactly the overconfidence the published table warned about.
  2. the BUCKET EDGES. 3.0σ, 4.0σ, 5.0σ and the expiry boundaries decide which
     numbers land in which published row; an off-by-one at an edge moves a move
     between rows with nothing to show for it.
  3. the MAKER GATE. #179 files a strategy follow-up if and only if a slice
     reaches 30 moves with positive drift net of the fee, so the gate is tested
     at 29, at 30, and on both signs.

Plus one structural guard: this module must keep IMPORTING q1's estimator
rather than growing its own copy of it.

Hermetic — synthetic values only, no evidence directory, no clock, no
subprocess.
"""
import math
import os
import sys
import unittest

RESEARCH = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..",
                                        "scripts", "research"))
sys.path.insert(0, RESEARCH)
import f1_quote_lag_strata as f1  # noqa: E402
import q1_quote_lag as q1  # noqa: E402


def sample(event, ticker, drift, z=3.5, seconds_to_close=1800.0,
           half_spread=0.005, fee=0.015):
    """One (event, market) pair shaped exactly like `q1.observe` emits."""
    return {"threshold_sigma": 3.0, "event_ts_utc": event, "z": z,
            "move_bps": z * 10.0, "ticker": ticker,
            "seconds_to_close": seconds_to_close,
            "mid_at_event": 0.5, "half_spread": half_spread, "fee": fee,
            "cost_to_take": half_spread + fee, "contested": True,
            "aligned_mid_drift": {5: drift, 15: drift, 30: drift, 60: drift},
            "spot_after": 65000.0, "strike": 64000.0,
            "sigma_per_min_bps": 3.0}


class ClusteredIntervalTest(unittest.TestCase):
    def test_correlated_clusters_do_not_count_as_independent_samples(self):
        """100 samples, 2 moves: the interval must reflect 2, not 100."""
        values = [1.0] * 50 + [-1.0] * 50
        clusters = ["A"] * 50 + ["B"] * 50
        summary = f1.cluster_summary(values, clusters)
        self.assertEqual(summary["n"], 100)
        self.assertEqual(summary["moves"], 2)
        self.assertAlmostEqual(summary["mean"], 0.0)
        # meat = 50^2 + (-50)^2 = 5000; G/(G-1) = 2; var = 5000*2/100^2 = 1.0
        self.assertAlmostEqual(summary["cluster_stderr"], 1.0, places=12)
        self.assertAlmostEqual(summary["naive_stderr"], math.sqrt(100.0 / 99.0)
                               / 10.0, places=12)
        self.assertGreater(summary["cluster_stderr"],
                           9.0 * summary["naive_stderr"])

    def test_one_move_gets_no_interval_rather_than_a_narrow_one(self):
        summary = f1.cluster_summary([0.18, 0.19, 0.17, 0.20], ["one"] * 4)
        self.assertEqual(summary["moves"], 1)
        self.assertIsNone(summary["cluster_stderr"])
        self.assertIsNone(summary["ci95"])
        self.assertIsNone(summary["cluster_t_stat"])
        self.assertIn("one spot move", summary["interval_note"])
        # The naive stderr is still reported — it is the thing being warned
        # about, so hiding it would remove the evidence for the warning.
        self.assertIsNotNone(summary["naive_stderr"])

    def test_singleton_clusters_reproduce_the_naive_stderr_exactly(self):
        """With one sample per move, clustering must change nothing."""
        values = [0.4, -0.1, 0.9, 0.2, -0.6, 0.3]
        summary = f1.cluster_summary(values, list(range(len(values))))
        self.assertEqual(summary["moves"], len(values))
        self.assertAlmostEqual(summary["cluster_stderr"],
                               summary["naive_stderr"], places=12)

    def test_interval_uses_t_on_move_count_minus_one(self):
        values = [2.0, 2.0, -1.0, -1.0, -1.0, -1.0]
        clusters = ["A", "A", "B", "B", "C", "C"]
        summary = f1.cluster_summary(values, clusters)
        self.assertEqual(summary["df"], 2)
        half_width = 4.303 * summary["cluster_stderr"]      # t(.975, df=2)
        self.assertAlmostEqual(summary["ci95"][0],
                               summary["mean"] - half_width, places=12)
        self.assertAlmostEqual(summary["ci95"][1],
                               summary["mean"] + half_width, places=12)

    def test_empty_slice_is_none_not_zero(self):
        self.assertIsNone(f1.cluster_summary([], []))

    def test_t_critical_is_exact_where_it_matters_and_wide_beyond(self):
        self.assertEqual(f1.t_critical_95(2), 4.303)
        self.assertEqual(f1.t_critical_95(29), 2.045)
        self.assertEqual(f1.t_critical_95(45), 2.021)     # falls back to df=40
        self.assertGreaterEqual(f1.t_critical_95(100000), 1.96)
        self.assertIsNone(f1.t_critical_95(0))


class BucketEdgeTest(unittest.TestCase):
    def test_sigma_bucket_edges(self):
        self.assertIsNone(f1.sigma_bucket(2.999))
        self.assertEqual(f1.sigma_bucket(3.0), "[3σ, 4σ)")
        self.assertEqual(f1.sigma_bucket(3.999), "[3σ, 4σ)")
        self.assertEqual(f1.sigma_bucket(4.0), "[4σ, 5σ)")
        self.assertEqual(f1.sigma_bucket(4.999), "[4σ, 5σ)")
        self.assertEqual(f1.sigma_bucket(5.0), ">= 5σ")
        self.assertEqual(f1.sigma_bucket(11.4), ">= 5σ")

    def test_a_down_move_buckets_on_its_magnitude(self):
        self.assertEqual(f1.sigma_bucket(-4.2), "[4σ, 5σ)")

    def test_expiry_bucket_edges_cover_q1s_whole_admitted_band(self):
        self.assertIsNone(f1.expiry_bucket(119.0))
        self.assertEqual(f1.expiry_bucket(float(q1.MIN_SECONDS_TO_CLOSE)),
                         "2–10 min to close")
        self.assertEqual(f1.expiry_bucket(599.9), "2–10 min to close")
        self.assertEqual(f1.expiry_bucket(600.0), "10–30 min to close")
        self.assertEqual(f1.expiry_bucket(1799.9), "10–30 min to close")
        self.assertEqual(f1.expiry_bucket(1800.0), "30–60 min to close")
        self.assertEqual(f1.expiry_bucket(float(q1.MAX_SECONDS_TO_CLOSE)),
                         "30–60 min to close")
        self.assertIsNone(f1.expiry_bucket(3601.0))


class StratumTest(unittest.TestCase):
    def test_markets_sharing_a_move_count_as_one_move(self):
        samples = [sample("T1", "A", 0.03), sample("T1", "B", 0.03),
                   sample("T1", "C", 0.03)]
        row = f1.stratum(samples, "one move")
        self.assertEqual(row["samples"], 3)
        self.assertEqual(row["moves"], 1)
        self.assertEqual(row["markets"], 3)
        horizon = next(h for h in row["horizons"] if h["seconds"] == 15)
        self.assertEqual(horizon["moves"], 1)
        self.assertIsNone(horizon["aligned_mid_drift"]["ci95"])

    def test_both_costs_are_netted_separately_and_neither_silently(self):
        samples = [sample("T1", "A", 0.04, half_spread=0.005, fee=0.015),
                   sample("T2", "B", 0.02, half_spread=0.005, fee=0.015)]
        horizon = next(h for h in f1.stratum(samples, "x")["horizons"]
                       if h["seconds"] == 15)
        self.assertAlmostEqual(horizon["aligned_mid_drift"]["mean"], 0.03)
        self.assertAlmostEqual(horizon["net_of_maker_cost"]["mean"], 0.015)
        self.assertAlmostEqual(horizon["net_of_taking_cost"]["mean"], 0.010)
        self.assertAlmostEqual(horizon["mean_fee"], 0.015)
        self.assertAlmostEqual(horizon["mean_half_spread"], 0.005)

    def test_a_horizon_no_sample_reached_is_absent_not_zero(self):
        thin = sample("T1", "A", 0.02)
        thin["aligned_mid_drift"] = {5: 0.02}
        seconds = [h["seconds"] for h in f1.stratum([thin], "x")["horizons"]]
        self.assertEqual(seconds, [5])

    def test_empty_slice_reports_zero_samples_and_no_rows(self):
        row = f1.stratum([], "empty")
        self.assertEqual((row["samples"], row["moves"], row["markets"]),
                         (0, 0, 0))
        self.assertEqual(row["horizons"], [])


class MakerGateTest(unittest.TestCase):
    """#179's filing trigger: >= 30 moves AND positive drift net of the fee."""

    def slice_with(self, moves, drift, fee=0.015):
        samples = [sample("T%d" % index, "M%d" % index, drift, fee=fee)
                   for index in range(moves)]
        return [("family", [f1.stratum(samples, "slice")])]

    def test_twenty_nine_moves_is_not_testable_however_good_it_looks(self):
        verdict = f1.maker_verdict(self.slice_with(29, 0.05))
        self.assertIn("NOT TESTABLE", verdict["verdict"])
        self.assertFalse(verdict["slices"][0]["testable"])
        self.assertTrue(verdict["slices"][0]["point_estimate_positive"])

    def test_thirty_moves_with_positive_maker_net_triggers_the_follow_up(self):
        verdict = f1.maker_verdict(self.slice_with(30, 0.05))
        self.assertIn("TESTABLE AND POSITIVE", verdict["verdict"])
        self.assertTrue(verdict["slices"][0]["testable"])

    def test_thirty_moves_with_negative_maker_net_does_not(self):
        verdict = f1.maker_verdict(self.slice_with(30, 0.001))
        self.assertIn("TESTABLE AND NEGATIVE", verdict["verdict"])
        self.assertTrue(verdict["slices"][0]["testable"])
        self.assertFalse(verdict["slices"][0]["point_estimate_positive"])

    def test_the_threshold_counts_moves_not_samples(self):
        """Ten moves quoted by ten markets each is ten moves, not a hundred."""
        samples = [sample("T%d" % move, "M%d" % market, 0.05)
                   for move in range(10) for market in range(10)]
        verdict = f1.maker_verdict([("family", [f1.stratum(samples, "s")])])
        self.assertEqual(verdict["slices"][0]["samples"], 100)
        self.assertEqual(verdict["slices"][0]["moves"], 10)
        self.assertIn("NOT TESTABLE", verdict["verdict"])

    def test_a_positive_point_with_an_interval_across_zero_says_both(self):
        samples = ([sample("T%d" % index, "M", 0.10) for index in range(15)]
                   + [sample("T%d" % (100 + index), "M", -0.06)
                      for index in range(15)])
        row = f1.maker_verdict([("f", [f1.stratum(samples, "s")])])["slices"][0]
        self.assertTrue(row["point_estimate_positive"])
        self.assertFalse(row["interval_excludes_zero"])


class CoverageTest(unittest.TestCase):
    """Span hours can hide a hole; covered hours may not."""

    def quotes(self, instants):
        return {"KXBTCD-X": [(ts, 0.4, 0.6, 0.5) for ts in instants]}

    def test_a_hole_longer_than_the_cooldown_is_listed_and_subtracted(self):
        hour = 3_600_000
        instants = [0, 60_000, 120_000, 120_000 + 2 * hour, 120_000 + 2 * hour + 60_000]
        window = f1.coverage(self.quotes(instants), 0, instants[-1])
        self.assertEqual(len(window["gaps"]), 1)
        self.assertAlmostEqual(window["gaps"][0]["hours"], 2.0)
        self.assertAlmostEqual(window["span_hours"] - window["covered_hours"],
                               2.0)

    def test_a_continuous_stream_loses_nothing(self):
        instants = list(range(0, 3_600_000 + 1, 60_000))
        window = f1.coverage(self.quotes(instants), 0, instants[-1])
        self.assertEqual(window["gaps"], [])
        self.assertAlmostEqual(window["covered_hours"], window["span_hours"])
        self.assertAlmostEqual(window["span_hours"], 1.0)

    def test_gaps_are_pooled_across_markets(self):
        """One market's silence is not a gap if another market was quoting."""
        hour = 3_600_000
        quotes = {"A": [(0, 0.4, 0.6, 0.5), (hour, 0.4, 0.6, 0.5)],
                  "B": [(t, 0.4, 0.6, 0.5)
                        for t in range(0, hour + 1, 60_000)]}
        window = f1.coverage(quotes, 0, hour)
        self.assertEqual(window["gaps"], [])


class ReuseTest(unittest.TestCase):
    """#179 says "unmodified methods" — so the estimator must stay q1's."""

    def test_the_statistic_is_imported_from_q1_never_redefined_here(self):
        for name in ("detect_events", "observe", "load_quote_series",
                     "QuoteBook", "summarize"):
            defined_here = f1.__dict__.get(name)
            self.assertIsNone(
                defined_here,
                "%s must be called through q1, not shadowed in f1" % name)
        self.assertIs(f1.q1, q1)

    def test_the_expiry_buckets_stay_inside_q1s_admitted_band(self):
        self.assertEqual(f1.EXPIRY_BUCKETS_S[0][0], q1.MIN_SECONDS_TO_CLOSE)
        self.assertEqual(f1.EXPIRY_BUCKETS_S[-1][1], q1.MAX_SECONDS_TO_CLOSE)

    def test_reported_horizons_exclude_the_momentum_ones(self):
        self.assertTrue(all(h < 120 for h in f1.REPORTED_HORIZONS_S))
        self.assertIn(f1.PRIMARY_HORIZON_S, f1.REPORTED_HORIZONS_S)

    def test_a_coverage_hole_is_measured_against_q1s_event_cooldown(self):
        self.assertEqual(f1.COVERAGE_GAP_MS, q1.EVENT_COOLDOWN_MS)


if __name__ == "__main__":
    unittest.main()
