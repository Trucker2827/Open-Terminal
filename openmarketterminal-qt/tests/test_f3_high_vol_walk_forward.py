#!/usr/bin/env python3
"""Issue #172 — the methodology guarantees of the F3 high-volatility follow-up.

The deliverable of F3 is a measurement, so what has to be regression-tested is
not a number (the evidence logs rotate; the numbers move) but the properties the
measurement claims about itself:

  * the split is by CONTRACT, never by row — no ticker may appear in two folds,
    and folds must be strictly ordered in close time;
  * the volatility threshold is FROZEN — row selection depends only on the row's
    own volatility and the passed threshold, never on the surrounding
    distribution or on which fold the row is being scored in;
  * missing volatility reads missing — such rows are dropped and counted, and an
    empty cell scores None rather than a flattering 0.0;
  * every cell reports a contract count alongside its row count;
  * the sensitivity sweep actually contains the frozen threshold, so the frozen
    cut cannot quietly sit outside the grid that is supposed to test it.

Stdlib-only synthetic contracts — no evidence directory is read, so this test is
hermetic and runs anywhere CI does.
"""
import math
import os
import sys
import unittest

RESEARCH = os.path.abspath(os.path.join(os.path.dirname(__file__),
                                        "..", "..", "scripts", "research"))
sys.path.insert(0, RESEARCH)
import f3_high_vol_walk_forward as f3   # noqa: E402


HOUR_MS = 3_600_000


def make_contract(ticker, close_ms, rows, outcome=True, source="derived"):
    """rows: list of (vol_bps_or_None, calibrated_p, market_mid)."""
    built = []
    for index, (vol, p, mid) in enumerate(rows):
        built.append({"ticker": ticker, "ts_ms": close_ms - (index + 1) * 60_000,
                      "calibrated_p": p, "market_mid": mid, "outcome": outcome,
                      "vol_per_min_bps": vol, "outcome_source": source})
    return {"ticker": ticker, "close_ms": close_ms, "outcome": outcome,
            "outcome_source": source, "family": "KXBTCD",
            "observations": len(built), "rows": built}


def synthetic(n=60, rows_per_contract=4, base_ms=1_753_000_000_000):
    """n contracts on distinct close hours, alternating outcomes and vols."""
    contracts = []
    for i in range(n):
        vol = 1.0 + (i % 10)          # 1.0 .. 10.0 bps
        outcome = (i % 2 == 0)
        p = 0.7 if outcome else 0.4
        mid = 0.6 if outcome else 0.5
        contracts.append(make_contract(
            f"KXBTCD-T{i:05d}", base_ms + i * HOUR_MS,
            [(vol, p, mid)] * rows_per_contract, outcome=outcome,
            source="recorded" if i % 3 == 0 else "derived"))
    return contracts


class FilterByVolTest(unittest.TestCase):
    def test_selection_depends_only_on_row_vol_and_threshold(self):
        """The freeze: identical rows select identically in different datasets.

        This is the property that distinguishes a frozen threshold from a
        tercile that refits itself — if selection consulted the surrounding
        distribution, embedding the same contracts among much faster ones would
        change which rows are 'high vol'.
        """
        target = make_contract("KXBTCD-TARGET", 1_753_000_000_000,
                               [(3.0, 0.5, 0.5), (5.0, 0.5, 0.5)])
        calm = [make_contract(f"C{i}", 1_753_000_000_000 + i * HOUR_MS,
                              [(0.1, 0.5, 0.5)]) for i in range(20)]
        wild = [make_contract(f"W{i}", 1_753_000_000_000 + i * HOUR_MS,
                              [(99.0, 0.5, 0.5)]) for i in range(20)]

        in_calm, _ = f3.filter_by_vol([target] + calm, 3.82, "high")
        in_wild, _ = f3.filter_by_vol([target] + wild, 3.82, "high")
        picked_calm = [r["vol_per_min_bps"]
                       for c in in_calm if c["ticker"] == "KXBTCD-TARGET"
                       for r in c["rows"]]
        picked_wild = [r["vol_per_min_bps"]
                       for c in in_wild if c["ticker"] == "KXBTCD-TARGET"
                       for r in c["rows"]]
        self.assertEqual(picked_calm, [5.0])
        self.assertEqual(picked_calm, picked_wild)

    def test_threshold_boundary_is_strict_above_and_inclusive_below(self):
        """high and low_mid partition the rows exactly once, no row twice."""
        contract = make_contract("KXBTCD-B", 1_753_000_000_000,
                                 [(3.81, 0.5, 0.5), (3.82, 0.5, 0.5),
                                  (3.83, 0.5, 0.5)])
        high, _ = f3.filter_by_vol([contract], 3.82, "high")
        low, _ = f3.filter_by_vol([contract], 3.82, "low_mid")
        high_vols = [r["vol_per_min_bps"] for c in high for r in c["rows"]]
        low_vols = [r["vol_per_min_bps"] for c in low for r in c["rows"]]
        self.assertEqual(high_vols, [3.83])
        self.assertEqual(sorted(low_vols), [3.81, 3.82])
        self.assertEqual(len(high_vols) + len(low_vols), 3)

    def test_rows_without_volatility_are_dropped_and_counted(self):
        contract = make_contract("KXBTCD-N", 1_753_000_000_000,
                                 [(None, 0.5, 0.5), (9.0, 0.5, 0.5),
                                  (None, 0.5, 0.5)])
        high, dropped = f3.filter_by_vol([contract], 3.82, "high")
        self.assertEqual(dropped, 2)
        self.assertEqual([r["vol_per_min_bps"] for c in high for r in c["rows"]],
                         [9.0])

    def test_contract_with_no_surviving_row_disappears_entirely(self):
        contract = make_contract("KXBTCD-Q", 1_753_000_000_000, [(0.5, 0.5, 0.5)])
        high, _ = f3.filter_by_vol([contract], 3.82, "high")
        self.assertEqual(high, [])

    def test_rejects_unknown_regime(self):
        with self.assertRaises(ValueError):
            f3.filter_by_vol(synthetic(5), 3.82, "medium")


class FoldSplitTest(unittest.TestCase):
    def test_no_ticker_appears_in_two_folds(self):
        contracts = synthetic(60)
        seen = set()
        for start, end in f3.fold_slices(contracts, 5):
            tickers = {c["ticker"] for c in contracts[start:end]}
            self.assertEqual(seen & tickers, set(),
                             "a contract leaked across a fold boundary")
            seen |= tickers
        self.assertEqual(len(seen), 60, "folds must cover every contract once")

    def test_folds_are_strictly_ordered_in_close_time(self):
        contracts = synthetic(60)
        slices = f3.fold_slices(contracts, 5)
        for (s0, e0), (s1, e1) in zip(slices, slices[1:]):
            self.assertLess(contracts[e0 - 1]["close_ms"],
                            contracts[s1]["close_ms"],
                            "a fold is not strictly in the future of the last")

    def test_shared_close_time_never_straddles_a_boundary(self):
        """Contracts closing at the same instant belong to the same fold.

        The group size (7) must NOT divide the fold size (40/4 = 10), or the
        unsnapped boundaries land on group edges and the test asserts nothing —
        so the fixture's non-vacuity is asserted first, in the same terms the
        splitter uses. Review found the original 10-per-instant version passing
        with the snap block deleted; the assertion below is what makes that
        deletion visible.
        """
        base = 1_753_000_000_000
        total, folds, per_instant = 40, 4, 7
        contracts = []
        for i in range(total):
            contracts.append(
                make_contract(f"T{i}", base + (i // per_instant) * HOUR_MS,
                              [(9.0, 0.5, 0.5)]))
        contracts.sort(key=lambda c: (c["close_ms"], c["ticker"]))

        # what an unsnapped splitter would do: at least one boundary must fall
        # strictly INSIDE a close-time group, or there is nothing to snap past
        naive = [int(round(index * total / folds)) for index in range(1, folds)]
        straddling = [edge for edge in naive
                      if contracts[edge]["close_ms"] == contracts[edge - 1]["close_ms"]]
        self.assertTrue(straddling,
                        "fixture is vacuous: no unsnapped boundary splits a "
                        "close-time group, so snapping is never exercised")

        fold_of = {}
        for index, (start, end) in enumerate(f3.fold_slices(contracts, folds)):
            for contract in contracts[start:end]:
                fold_of.setdefault(contract["close_ms"], index)
                self.assertEqual(fold_of[contract["close_ms"]], index,
                                 "one close instant was split across folds")

    def test_partition_covers_everything_for_many_fold_counts(self):
        contracts = synthetic(53)
        for folds in range(1, 8):
            slices = f3.fold_slices(contracts, folds)
            covered = sum(end - start for start, end in slices)
            self.assertEqual(covered, 53)
            self.assertEqual(slices[0][0], 0)
            self.assertEqual(slices[-1][1], 53)

    def test_empty_input_yields_no_folds(self):
        self.assertEqual(f3.fold_slices([], 5), [])


class ScoreCellTest(unittest.TestCase):
    def test_empty_cell_is_none_not_zero(self):
        self.assertIsNone(f3.score_cell([]))

    def test_reports_contract_count_beside_row_count(self):
        contracts = synthetic(10, rows_per_contract=6)
        cell = f3.score_cell(contracts)
        self.assertEqual(cell["contracts"], 10)
        self.assertEqual(cell["rows"], 60)
        self.assertEqual(cell["observations_per_contract"]["median"], 6)

    def test_pooled_and_contract_mean_can_disagree(self):
        """One row-heavy contract can outvote many light ones on pooled rows.

        This is why both weightings are reported: the contract-mean says the
        calibrator loses 2 of 3 contracts while the pooled rows say it wins.
        """
        heavy = make_contract("HEAVY", 1_753_000_000_000,
                              [(9.0, 0.9, 0.5)] * 50, outcome=True)
        light_a = make_contract("LIGHT_A", 1_753_000_000_000 + HOUR_MS,
                                [(9.0, 0.1, 0.4)], outcome=True)
        light_b = make_contract("LIGHT_B", 1_753_000_000_000 + 2 * HOUR_MS,
                                [(9.0, 0.1, 0.4)], outcome=True)
        cell = f3.score_cell([heavy, light_a, light_b])
        self.assertTrue(cell["calibrator_beats_mid_pooled"])
        self.assertFalse(cell["calibrator_beats_mid_contract_mean"])
        self.assertEqual(cell["paired_by_contract"]["contracts_calibrator_better"], 1)
        self.assertEqual(cell["paired_by_contract"]["contracts_market_better"], 2)

    def test_brier_matches_hand_computation(self):
        contract = make_contract("X", 1_753_000_000_000,
                                 [(9.0, 0.8, 0.6)], outcome=True)
        cell = f3.score_cell([contract])
        self.assertAlmostEqual(cell["pooled_row_brier_calibrated"], 0.04)
        self.assertAlmostEqual(cell["pooled_row_brier_market_mid"], 0.16)
        self.assertAlmostEqual(cell["pooled_row_delta"], -0.12)

    def test_sign_test_p_is_exact_binomial(self):
        # 0 of 5 contracts favouring the calibrator: 2 * (1/32) = 0.0625
        self.assertAlmostEqual(f3._binomial_two_sided_p(0, 5), 0.0625)
        self.assertAlmostEqual(f3._binomial_two_sided_p(5, 5), 0.0625)
        self.assertAlmostEqual(f3._binomial_two_sided_p(3, 6), 1.0)
        self.assertIsNone(f3._binomial_two_sided_p(0, 0))

    def test_ties_are_excluded_from_the_sign_test_denominator(self):
        tied = make_contract("TIED", 1_753_000_000_000, [(9.0, 0.5, 0.5)])
        won = make_contract("WON", 1_753_000_000_000 + HOUR_MS,
                            [(9.0, 0.9, 0.5)], outcome=True)
        paired = f3.score_cell([tied, won])["paired_by_contract"]
        self.assertEqual(paired["contracts_tied"], 1)
        self.assertEqual(paired["contracts_calibrator_better"], 1)
        self.assertEqual(paired["contracts_market_better"], 0)


class WalkForwardTest(unittest.TestCase):
    def test_leakage_check_is_clean_on_a_contract_wise_split(self):
        contracts = synthetic(60)
        result = f3.walk_forward(contracts, folds=5)
        self.assertTrue(result["leakage_check"]["clean"])
        self.assertEqual(result["leakage_check"]["tickers_in_more_than_one_fold"], 0)
        self.assertTrue(result["leakage_check"]["folds_strictly_time_ordered"])

    def test_folds_partition_the_contracts_exactly(self):
        contracts = synthetic(60)
        result = f3.walk_forward(contracts, folds=5)
        self.assertEqual(sum(f["contracts"] for f in result["folds"]), 60)
        self.assertEqual(result["pooled"]["contracts"], 60)

    def test_pooled_equals_scoring_the_whole_subset(self):
        """Nothing is fitted, so pooled must be the full-subset score itself."""
        contracts = synthetic(60)
        result = f3.walk_forward(contracts, folds=5)
        whole = f3.score_cell(contracts)
        self.assertAlmostEqual(result["pooled"]["pooled_row_brier_calibrated"],
                               whole["pooled_row_brier_calibrated"])
        self.assertAlmostEqual(result["pooled"]["pooled_row_delta"],
                               whole["pooled_row_delta"])

    def test_fold_scores_do_not_depend_on_the_surrounding_folds(self):
        """A frozen threshold means a fold scores the same in isolation.

        If any per-fold quantity were re-derived from the fold's own
        distribution, scoring that fold alone would change its numbers.
        """
        contracts = synthetic(60)
        result = f3.walk_forward(contracts, folds=5)
        slices = f3.fold_slices(contracts, 5)
        for fold, (start, end) in zip(result["folds"], slices):
            alone = f3.score_cell(contracts[start:end])
            self.assertAlmostEqual(fold["pooled_row_delta"],
                                   alone["pooled_row_delta"])
            self.assertEqual(fold["contracts"], alone["contracts"])

    def test_too_small_a_subset_reads_insufficient_not_a_number(self):
        result = f3.walk_forward(synthetic(4), folds=5)
        self.assertEqual(result["folds"], [])
        self.assertIsNone(result["pooled"])
        self.assertIn("INSUFFICIENT DATA", result["note"])


class FrozenThresholdTest(unittest.TestCase):
    def test_frozen_threshold_is_the_published_tercile_boundary(self):
        self.assertEqual(f3.FROZEN_VOL_THRESHOLD_BPS, 3.82)
        self.assertIn("#169", f3.FROZEN_THRESHOLD_SOURCE)

    def test_sensitivity_grid_contains_the_frozen_threshold(self):
        self.assertIn(f3.FROZEN_VOL_THRESHOLD_BPS, f3.SENSITIVITY_THRESHOLDS_BPS)
        flagged = [t for t in f3.SENSITIVITY_THRESHOLDS_BPS
                   if t == f3.FROZEN_VOL_THRESHOLD_BPS]
        self.assertEqual(len(flagged), 1)

    def test_sensitivity_spans_both_sides_of_the_frozen_cut(self):
        grid = f3.SENSITIVITY_THRESHOLDS_BPS
        self.assertTrue(any(t < f3.FROZEN_VOL_THRESHOLD_BPS for t in grid))
        self.assertTrue(any(t > f3.FROZEN_VOL_THRESHOLD_BPS for t in grid))
        self.assertEqual(list(grid), sorted(grid))

    def test_sensitivity_reports_contract_counts_for_every_cut(self):
        contracts = synthetic(60)
        rows = f3.sensitivity(contracts, thresholds=(2.0, 3.82, 8.0))
        self.assertEqual(len(rows), 3)
        for row in rows:
            self.assertIn("contracts", row)
            self.assertIn("rows", row)
        self.assertTrue(rows[1]["is_frozen_threshold"])
        # a higher cut can never admit more contracts than a lower one
        self.assertGreaterEqual(rows[0]["contracts"], rows[1]["contracts"])
        self.assertGreaterEqual(rows[1]["contracts"], rows[2]["contracts"])

    def test_prefix_diagnostic_flags_a_degenerate_boundary(self):
        """A calm prefix followed by a fast remainder must read `degenerate`."""
        base = 1_753_000_000_000
        contracts = []
        for i in range(40):
            contracts.append(make_contract(f"CALM{i:03d}", base + i * HOUR_MS,
                                           [(0.5, 0.5, 0.5)] * 4))
        for i in range(80):
            contracts.append(make_contract(f"FAST{i:03d}",
                                           base + (40 + i) * HOUR_MS,
                                           [(50.0, 0.5, 0.5)] * 4))
        diagnostic = f3.prefix_derived_threshold_diagnostic(contracts)
        self.assertTrue(diagnostic["available"])
        self.assertTrue(diagnostic["degenerate"])
        self.assertGreater(diagnostic["share_of_later_rows_admitted"], 0.9)

    def test_prefix_diagnostic_reads_unavailable_rather_than_guessing(self):
        diagnostic = f3.prefix_derived_threshold_diagnostic(synthetic(9))
        self.assertFalse(diagnostic["available"])
        self.assertIn("reason", diagnostic)


class EpisodeStructureTest(unittest.TestCase):
    """The effective-n check: a burst of contracts is one observation, not many."""

    def _rows_at(self, ticker, close_ms, stamps):
        rows = [{"ticker": ticker, "ts_ms": ts, "calibrated_p": 0.6,
                 "market_mid": 0.5, "outcome": True, "vol_per_min_bps": 9.0,
                 "outcome_source": "derived"} for ts in stamps]
        return {"ticker": ticker, "close_ms": close_ms, "outcome": True,
                "outcome_source": "derived", "family": "KXBTCD",
                "observations": len(rows), "rows": rows}

    def test_rows_an_hour_apart_split_into_two_episodes(self):
        base = 1_753_000_000_000
        contracts = [
            self._rows_at("A", base + HOUR_MS, [base, base + 60_000]),
            self._rows_at("B", base + 10 * HOUR_MS,
                          [base + 5 * HOUR_MS, base + 5 * HOUR_MS + 60_000]),
        ]
        result = f3.episode_structure(contracts)
        self.assertEqual(result["episode_count"], 2)
        self.assertEqual([e["rows"] for e in result["episodes"]], [2, 2])
        self.assertEqual([e["contracts"] for e in result["episodes"]], [1, 1])

    def test_a_dense_burst_is_one_episode_however_many_contracts(self):
        """Fifty contracts inside one minute must not read as fifty episodes."""
        base = 1_753_000_000_000
        contracts = [self._rows_at(f"T{i}", base + HOUR_MS, [base + i * 1000])
                     for i in range(50)]
        result = f3.episode_structure(contracts)
        self.assertEqual(result["episode_count"], 1)
        self.assertEqual(result["episodes"][0]["contracts"], 50)
        self.assertEqual(result["largest_episode_share_of_rows"], 1.0)

    def test_gap_boundary_is_strictly_greater_than_the_gap(self):
        base = 1_753_000_000_000
        exactly = [self._rows_at("A", base + 5 * HOUR_MS,
                                 [base, base + f3.EPISODE_GAP_MS])]
        self.assertEqual(f3.episode_structure(exactly)["episode_count"], 1)
        beyond = [self._rows_at("A", base + 5 * HOUR_MS,
                                [base, base + f3.EPISODE_GAP_MS + 1])]
        self.assertEqual(f3.episode_structure(beyond)["episode_count"], 2)

    def test_empty_subset_reads_empty_not_one_episode(self):
        result = f3.episode_structure([])
        self.assertEqual(result["episode_count"], 0)
        self.assertEqual(result["episodes"], [])

    def test_volatility_by_day_shows_a_calm_day_could_not_contribute(self):
        """A day under the threshold must report 0 rows above it, not silence."""
        base = 1_753_000_000_000          # 2026-07-20T10:13:20Z
        calm = self._rows_at("CALM", base + 5 * HOUR_MS, [base])
        calm["rows"][0]["vol_per_min_bps"] = 1.5
        fast = self._rows_at("FAST", base + 30 * HOUR_MS, [base + 25 * HOUR_MS])
        days = f3.volatility_by_utc_day([calm, fast])
        self.assertEqual(len(days), 2)
        self.assertEqual(days[0]["rows_above_frozen_threshold"], 0)
        self.assertEqual(days[0]["max_bps"], 1.5)
        self.assertEqual(days[1]["rows_above_frozen_threshold"], 1)
        self.assertEqual(days[1]["share_above_frozen_threshold"], 1.0)

    def test_rows_by_day_counts_every_row_once(self):
        base = 1_753_000_000_000
        contracts = [self._rows_at(f"T{i}", base + 100 * HOUR_MS,
                                   [base + i * 26 * HOUR_MS]) for i in range(4)]
        result = f3.episode_structure(contracts)
        self.assertEqual(sum(result["rows_by_utc_day"].values()), 4)


class VerdictTest(unittest.TestCase):
    def test_verdict_says_does_not_survive_when_the_mid_wins(self):
        contracts = [make_contract(f"L{i}", 1_753_000_000_000 + i * HOUR_MS,
                                   [(9.0, 0.2, 0.6)] * 3, outcome=True)
                     for i in range(40)]
        result = f3.walk_forward(contracts, folds=5)
        text = f3.verdict(result, None, f3.sensitivity(contracts, (3.82,)))
        self.assertIn("DOES NOT SURVIVE", text)
        self.assertIn("n=40", text)

    def test_verdict_says_reproduces_when_the_calibrator_wins_both(self):
        contracts = [make_contract(f"W{i}", 1_753_000_000_000 + i * HOUR_MS,
                                   [(9.0, 0.9, 0.6)] * 3, outcome=True)
                     for i in range(40)]
        result = f3.walk_forward(contracts, folds=5)
        text = f3.verdict(result, None, f3.sensitivity(contracts, (3.82,)))
        self.assertIn("REPRODUCES", text)

    def test_verdict_reports_no_verdict_on_an_unscoreable_subset(self):
        result = f3.walk_forward(synthetic(3), folds=5)
        self.assertIn("NO VERDICT", f3.verdict(result, None, []))


if __name__ == "__main__":
    unittest.main()
