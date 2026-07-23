import json
import os
import sys
import tempfile
import unittest

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts"))
sys.path.insert(0, os.path.join(SCRIPTS, "arena"))
sys.path.insert(0, os.path.join(SCRIPTS, "kalshi_advise"))

import arena_llm_forecaster as adapter
import arena_loop
import arena_report


class ParseReplyTest(unittest.TestCase):
    def test_bare_json(self):
        parsed, err = adapter.parse_model_reply(
            '{"probability": 0.62, "confidence": 0.5, "rationale": "above strike"}')
        self.assertIsNone(err)
        self.assertAlmostEqual(parsed["probability"], 0.62)

    def test_prose_embedded_json(self):
        parsed, err = adapter.parse_model_reply(
            'Based on the packet I estimate {"probability": 0.31, "rationale": "far below"} overall.')
        self.assertIsNone(err)
        self.assertAlmostEqual(parsed["probability"], 0.31)

    def test_out_of_range_and_junk_abstain(self):
        for bad in ('{"probability": 1.7}', '{"p": "high"}', "no json here", ""):
            parsed, err = adapter.parse_model_reply(bad)
            self.assertIsNone(parsed, bad)
            self.assertEqual(err, "UNPARSEABLE_REPLY")

    def test_alias_keys_and_string_numbers(self):
        parsed, _ = adapter.parse_model_reply('{"p": "0.4"}')
        self.assertAlmostEqual(parsed["probability"], 0.4)

    def test_predict_abstains_on_dead_endpoint(self):
        entry = {"id": "x", "kind": "ollama", "model": "m",
                 "endpoint": "http://127.0.0.1:1", "epoch_id": "e", "timeout_s": 2}
        out = adapter.predict(entry, {"seconds_left": 900})
        self.assertEqual(out["decision"], "abstain")
        self.assertEqual(out["reason_code"], "MODEL_UNAVAILABLE")
        self.assertTrue(out["prompt_hash"])


class RegistryTest(unittest.TestCase):
    def _write(self, forecasters):
        tmp = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        json.dump({"defaults": {"timeout_s": 88}, "forecasters": forecasters}, tmp)
        tmp.close()
        self.addCleanup(os.unlink, tmp.name)
        return tmp.name

    def test_loads_enabled_with_defaults(self):
        path = self._write([
            {"id": "a", "kind": "ollama", "model": "m1", "endpoint": "http://x",
             "epoch_id": "e1", "enabled": True},
            {"id": "b", "kind": "ollama", "model": "m2", "endpoint": "http://x",
             "epoch_id": "e2", "enabled": True},
            {"id": "off", "kind": "ollama", "model": "m3", "endpoint": "http://x",
             "epoch_id": "e3", "enabled": False}])
        lanes = arena_loop.load_registry(path)
        self.assertEqual([l["id"] for l in lanes], ["a", "b"])
        self.assertEqual(lanes[0]["timeout_s"], 88)

    def test_rejects_single_lane_and_missing_keys(self):
        path = self._write([{"id": "a", "kind": "ollama", "model": "m",
                             "endpoint": "http://x", "epoch_id": "e", "enabled": True}])
        with self.assertRaisesRegex(ValueError, ">= 2 enabled"):
            arena_loop.load_registry(path)
        path = self._write([
            {"id": "a", "kind": "ollama", "model": "m", "enabled": True,
             "epoch_id": "e"},
            {"id": "b", "kind": "ollama", "model": "m", "endpoint": "http://x",
             "epoch_id": "e2", "enabled": True}])
        with self.assertRaisesRegex(ValueError, "endpoint"):
            arena_loop.load_registry(path)

    def test_shipped_registry_is_valid(self):
        lanes = arena_loop.load_registry()
        self.assertGreaterEqual(len(lanes), 2)
        self.assertEqual(len({l["epoch_id"] for l in lanes}), len(lanes))


def fake_rounds():
    def lane(idx, status):
        return {"id": f"m{idx}", "status": status, "epoch_id": f"e{idx}"}
    rounds = []
    for i in range(10):
        rounds.append({"status": "DONE", "lanes": [
            lane(1, "COMMITTED_BLIND"),
            lane(2, "COMMITTED_BLIND" if i < 5 else "EXPIRED"),
        ]})
    rounds.append({"status": "NO_CONTRACT"})   # must not count as offered
    return rounds


class ReportTest(unittest.TestCase):
    LANES = [{"id": "m1", "kind": "ollama", "model": "a", "epoch_id": "e1"},
             {"id": "m2", "kind": "ollama", "model": "b", "epoch_id": "e2"}]

    def test_coverage_counts(self):
        stats = arena_report.coverage_from_rounds(fake_rounds())
        self.assertEqual(stats["m1"], {"offered": 10, "committed": 10,
                                       "abstained": 0, "expired": 0})
        self.assertEqual(stats["m2"]["committed"], 5)
        self.assertEqual(stats["m2"]["expired"], 5)

    def test_low_coverage_is_never_ranked(self):
        def score(lane):
            return {"brier_pre": 0.10 if lane["id"] == "m2" else 0.20,
                    "ci_low": 0.05, "ci_high": 0.30,
                    "coverage": {"resolved": 100}}
        report = arena_report.build_report(self.LANES, fake_rounds(), score,
                                           min_coverage=0.8, min_resolved=50, now_ms=0)
        by_id = {e["id"]: e for e in report["leaderboard"]}
        # m2 has the better Brier but 50% coverage — the v4 lesson: not ranked.
        self.assertFalse(by_id["m2"]["comparable"])
        self.assertIn("coverage", by_id["m2"]["reason"])
        self.assertTrue(by_id["m1"]["comparable"])
        self.assertEqual(by_id["m1"]["rank"], 1)

    def test_verdict_requires_ci_separation(self):
        def overlapping(lane):
            return {"brier_pre": 0.20 if lane["id"] == "m1" else 0.22,
                    "ci_low": 0.15, "ci_high": 0.27, "coverage": {"resolved": 100}}
        rounds = [{"status": "DONE", "lanes": [
            {"id": "m1", "status": "COMMITTED_BLIND"},
            {"id": "m2", "status": "COMMITTED_BLIND"}]}] * 10
        report = arena_report.build_report(self.LANES, rounds, overlapping,
                                           min_coverage=0.8, min_resolved=50, now_ms=0)
        self.assertEqual(report["verdict"], "STATISTICAL_TIE_SO_FAR")

        def separated(lane):
            if lane["id"] == "m1":
                return {"brier_pre": 0.10, "ci_low": 0.08, "ci_high": 0.12,
                        "coverage": {"resolved": 100}}
            return {"brier_pre": 0.30, "ci_low": 0.25, "ci_high": 0.35,
                    "coverage": {"resolved": 100}}
        report = arena_report.build_report(self.LANES, rounds, separated,
                                           min_coverage=0.8, min_resolved=50, now_ms=0)
        self.assertEqual(report["verdict"], "LEADER: m1")

    def test_no_data_is_insufficient_not_fabricated(self):
        report = arena_report.build_report(self.LANES, [], lambda lane: None,
                                           min_coverage=0.8, min_resolved=50, now_ms=0)
        self.assertEqual(report["verdict"], "INSUFFICIENT_DATA")
        for e in report["leaderboard"]:
            self.assertFalse(e["comparable"])
            self.assertIsNone(e["brier"])


if __name__ == "__main__":
    unittest.main()
