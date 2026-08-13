#!/usr/bin/env python3
"""Tests for the measurement-only signal availability contract."""
import json
import os
import sys
import tempfile
import time
import unittest

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts", "kalshi_advise"))
sys.path.insert(0, SCRIPTS)
import signal_availability as sa


class ContractShapeTest(unittest.TestCase):
    def test_every_signal_carries_the_six_fields(self):
        for name in sa.SIGNALS:
            row = sa.describe(name, value=0.0, observed_at=123)
            for field in sa.CONTRACT_FIELDS:
                self.assertIn(field, row, "%s missing %s" % (name, field))

    def test_an_unknown_signal_is_unavailable_with_a_reason(self):
        row = sa.describe("not_a_signal")
        self.assertFalse(row["available"])
        self.assertIsNotNone(row["reason_unavailable"])


class AvailabilityIsAboutTheSourceTest(unittest.TestCase):
    """The whole point: availability is decided by the SOURCE, never by whether
    the value happens to be zero."""

    def test_a_measured_zero_is_available_not_missing(self):
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fh:
            fh.write("{}")
            path = fh.name
        try:
            state = sa.probe_source(path)
            self.assertTrue(state["available"])
            self.assertIsNone(state.get("reason"))
        finally:
            os.unlink(path)

    def test_a_missing_source_reports_missing_and_carries_no_value(self):
        real = sa.SIGNALS["news_forecast"]["path"]
        sa.SIGNALS["news_forecast"]["path"] = "/definitely/not/here.json"
        try:
            row = sa.describe("news_forecast", value=0.42, observed_at=1)
            self.assertFalse(row["available"])
            self.assertEqual(row["reason_unavailable"], "missing")
            # A number beside "unavailable" invites the exact confusion this
            # module exists to remove.
            self.assertIsNone(row["value"])
        finally:
            sa.SIGNALS["news_forecast"]["path"] = real

    def test_stale_is_its_own_state_not_missing_and_not_neutral(self):
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fh:
            fh.write('{"x": 1}')
            path = fh.name
        try:
            old = time.time() - 3600
            os.utime(path, (old, old))
            state = sa.probe_source(path, stale_ms=60_000)
            self.assertFalse(state["available"])
            self.assertEqual(state["reason"], "stale")
            self.assertGreater(state["age_ms"], 60_000)
        finally:
            os.unlink(path)

    def test_an_empty_source_is_empty_not_available(self):
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fh:
            path = fh.name
        try:
            state = sa.probe_source(path)
            self.assertFalse(state["available"])
            self.assertEqual(state["reason"], "empty")
        finally:
            os.unlink(path)

    def test_the_four_unavailable_reasons_are_distinguishable(self):
        """missing / stale / empty / (available & neutral) must not collapse."""
        reasons = set()
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fh:
            fh.write('{"x": 1}')
            good = fh.name
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fh:
            empty = fh.name
        try:
            reasons.add(sa.probe_source("/nope.json").get("reason"))
            reasons.add(sa.probe_source(empty).get("reason"))
            old = time.time() - 3600
            os.utime(good, (old, old))
            reasons.add(sa.probe_source(good, stale_ms=60_000).get("reason"))
            os.utime(good, None)
            healthy = sa.probe_source(good)
            self.assertTrue(healthy["available"])
            reasons.add(healthy.get("reason"))          # None
        finally:
            os.unlink(good)
            os.unlink(empty)
        self.assertEqual(reasons, {"missing", "empty", "stale", None})


class ZeroRateIsNotAvailabilityTest(unittest.TestCase):
    """A non-zero rate is not an availability rate. The module must never
    present one as the other."""

    def test_zero_rate_labels_itself_as_a_value_observation(self):
        records = [{"observations": [{"trade_flow": 0.0}, {"trade_flow": 0.5}]}]
        got = sa.zero_rate(records, "trade_flow")
        self.assertEqual(got["observations"], 2)
        self.assertEqual(got["exactly_zero"], 1)
        self.assertIn("NOT inferable", got["means"])

    def test_an_all_zero_column_does_not_assert_unavailability(self):
        records = [{"observations": [{"trade_flow": 0.0} for _ in range(50)]}]
        got = sa.zero_rate(records, "trade_flow")
        self.assertEqual(got["zero_fraction"], 1.0)
        self.assertNotIn("available", got,
                         "a zero rate must not carry an availability verdict")


class CodePathTest(unittest.TestCase):
    def test_the_code_path_is_what_establishes_commodity_unavailability(self):
        """For a producer that never reads the aux sources, non-availability is
        a fact about the CODE, not about any table of values."""
        report = sa.build_report(now_ms=1)
        for family, entry in report["families"].items():
            if entry["producer"] in sa.PRODUCERS_WITHOUT_AUX:
                self.assertFalse(entry["producer_reads_aux_sources"], family)
                self.assertTrue(entry["signals_unavailable_by_code_path"], family)
            else:
                self.assertTrue(entry["producer_reads_aux_sources"], family)
                self.assertEqual(entry["signals_unavailable_by_code_path"], [], family)

    def test_the_report_declares_itself_measurement_only(self):
        report = sa.build_report(now_ms=1)
        self.assertTrue(report["analysis_only"])
        self.assertIn("never as NaN", report["never_nan"])
        self.assertIn("NOT an availability rate", report["terminology"])
        self.assertEqual(list(report["contract_fields"]), list(sa.CONTRACT_FIELDS))


if __name__ == "__main__":
    unittest.main()
