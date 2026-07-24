"""Hermetic tests for the weekly retrain loop's compare-and-swap (issue #84).

No qlib, no network, no real model training: the decision rule, the window
split, the atomic pointer write, and the publisher's "active" resolution are
all pure/file-local by design so the swap logic that guards the live signal
can be tested exactly.
"""
import json
import os
import sys
import tempfile
import unittest

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts", "ai_quant_lab"))
sys.path.insert(0, SCRIPTS)
import model_retrain as mr
import signal_publisher as sp

DATES_14 = [f"2026-07-{d:02d}" for d in range(1, 15)]


class DecideSwapTest(unittest.TestCase):
    def test_unmeasurable_new_model_is_never_adopted(self):
        for incumbent in (None, "old_model"):
            swap, reason = mr.decide_swap(None, incumbent, 0.01)
            self.assertFalse(swap, reason)
            self.assertIn("never adopt", reason)

    def test_bootstrap_adopts_first_measurable_model(self):
        swap, reason = mr.decide_swap(-0.5, None, None)
        self.assertTrue(swap)
        self.assertIn("bootstrap", reason)

    def test_unmeasurable_incumbent_keeps_pointer(self):
        swap, reason = mr.decide_swap(0.9, "old_model", None)
        self.assertFalse(swap)
        self.assertIn("refusing to swap", reason)

    def test_swap_iff_new_rank_ic_at_least_incumbents(self):
        self.assertTrue(mr.decide_swap(0.02, "old", 0.01)[0])     # better
        self.assertTrue(mr.decide_swap(0.01, "old", 0.01)[0])     # equal (>=)
        self.assertFalse(mr.decide_swap(0.0099, "old", 0.01)[0])  # worse


class SplitWindowsTest(unittest.TestCase):
    def test_holdout_is_strictly_after_training_data(self):
        w = mr.split_windows(DATES_14, eval_days=2, valid_days=2)
        self.assertTrue(w["success"])
        self.assertEqual(w["train"], {"start": "2026-07-01", "end": "2026-07-10"})
        self.assertEqual(w["valid"], {"start": "2026-07-11", "end": "2026-07-12"})
        self.assertEqual(w["eval"], {"start": "2026-07-13", "end": "2026-07-14"})
        self.assertLess(w["valid"]["end"], w["eval"]["start"])

    def test_duplicate_unsorted_dates_are_normalized(self):
        w = mr.split_windows(DATES_14[::-1] + DATES_14, eval_days=1, valid_days=1)
        self.assertTrue(w["success"])
        self.assertEqual(w["eval"], {"start": "2026-07-14", "end": "2026-07-14"})

    def test_too_few_dates_is_an_error_not_a_degenerate_split(self):
        w = mr.split_windows(DATES_14[:6], eval_days=2, valid_days=2, min_train_days=5)
        self.assertFalse(w["success"])
        self.assertIn("distinct dates", w["error"])


class PointerTest(unittest.TestCase):
    def test_missing_or_corrupt_pointer_reads_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "active_model.json")
            self.assertIsNone(mr.read_active(path))
            with open(path, "w") as fh:
                fh.write("{not json")
            self.assertIsNone(mr.read_active(path))
            with open(path, "w") as fh:
                json.dump({"schema": 1}, fh)   # no model_id -> unusable
            self.assertIsNone(mr.read_active(path))

    def test_atomic_write_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "sub", "active_model.json")
            mr.write_active_atomic({"schema": 1, "model_id": "m2"}, path)
            self.assertEqual(mr.read_active(path)["model_id"], "m2")
            self.assertFalse(os.path.exists(path + ".tmp"))


class ApplyDecisionTest(unittest.TestCase):
    """The full compare-and-swap against a real pointer file."""

    def test_worse_new_model_leaves_pointer_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "active_model.json")
            mr.write_active_atomic({"schema": 1, "model_id": "old"}, path)
            before = os.stat(path).st_mtime_ns
            out = mr.apply_decision("new", 0.001, "old", 0.02,
                                    {"start": "a", "end": "b"}, 7, path)
            self.assertFalse(out["swapped"])
            self.assertEqual(mr.read_active(path)["model_id"], "old")
            self.assertEqual(os.stat(path).st_mtime_ns, before)

    def test_better_new_model_repoints_with_full_provenance(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "active_model.json")
            mr.write_active_atomic({"schema": 1, "model_id": "old"}, path)
            out = mr.apply_decision("new", 0.03, "old", 0.02,
                                    {"start": "2026-07-13", "end": "2026-07-14"},
                                    7, path)
            self.assertTrue(out["swapped"])
            pointer = mr.read_active(path)
            self.assertEqual(pointer["model_id"], "new")
            self.assertEqual(pointer["previous_model_id"], "old")
            self.assertEqual(pointer["new_rank_ic"], 0.03)
            self.assertEqual(pointer["incumbent_rank_ic"], 0.02)
            self.assertEqual(pointer["updated_at_ms"], 7)
            self.assertEqual(pointer["eval_window"]["end"], "2026-07-14")


class PublisherResolutionTest(unittest.TestCase):
    """signal_publisher must follow the pointer only for model_id 'active'."""

    def test_explicit_model_id_passes_through(self):
        model_id, err = sp.resolve_model_id({"model_id": "lightgbm_x"})
        self.assertEqual((model_id, err), ("lightgbm_x", None))

    def test_active_resolves_from_pointer(self):
        with tempfile.TemporaryDirectory() as tmp:
            os.environ["OPENTERMINAL_MODELS_DIR"] = tmp
            try:
                mr.write_active_atomic({"schema": 1, "model_id": "m9"})
                model_id, err = sp.resolve_model_id({"model_id": "active"})
                self.assertEqual((model_id, err), ("m9", None))
            finally:
                os.environ.pop("OPENTERMINAL_MODELS_DIR", None)

    def test_active_without_pointer_is_a_loud_error_not_a_fallback(self):
        with tempfile.TemporaryDirectory() as tmp:
            os.environ["OPENTERMINAL_MODELS_DIR"] = tmp
            try:
                model_id, err = sp.resolve_model_id({"model_id": "active"})
                self.assertIsNone(model_id)
                self.assertIn("active-model", err)
                result = sp.publish({"model_id": "active"})
                self.assertFalse(result["success"])
                self.assertIn("active-model", result["error"])
            finally:
                os.environ.pop("OPENTERMINAL_MODELS_DIR", None)


if __name__ == "__main__":
    unittest.main()
