import json
import math
import os
import random
import shutil
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer

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


class SeasonTest(unittest.TestCase):
    OPEN_PARAMS = '{"days": 14, "min_resolved": 300}'

    def _season_path(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        return os.path.join(tmp, "arena-season.json")

    def test_open_writes_sealed_readonly_file(self):
        path = self._season_path()
        rec = arena_report.open_season(self.OPEN_PARAMS, now_ms=1000, path=path)
        self.assertEqual(rec["closes_at_ms"], 1000 + 14 * 86400000)
        self.assertEqual(rec["params"],
                         {"days": 14, "min_resolved": 300, "min_coverage": 0.80})
        self.assertEqual(arena_report.load_season(path), rec)
        self.assertFalse(os.stat(path).st_mode & 0o222)  # written read-only

    def test_open_while_open_is_refused_and_file_unchanged(self):
        path = self._season_path()
        arena_report.open_season(self.OPEN_PARAMS, now_ms=0, path=path)
        with open(path, "rb") as fh:
            before = fh.read()
        with self.assertRaisesRegex(ValueError, "immutable"):
            arena_report.open_season('{"days": 1, "min_resolved": 5}',
                                     now_ms=5000, path=path)
        with open(path, "rb") as fh:
            self.assertEqual(fh.read(), before)

    def test_params_are_preregistered_and_validated(self):
        path = self._season_path()
        for bad in ("not json", "[]", '{"days": 14}', '{"min_resolved": 300}',
                    '{"days": 0, "min_resolved": 300}',
                    '{"days": true, "min_resolved": 300}',
                    '{"days": 14, "min_resolved": 300, "bonus": 1}',
                    '{"days": 14, "min_resolved": 300, "min_coverage": 1.5}'):
            with self.assertRaises(ValueError):
                arena_report.open_season(bad, now_ms=0, path=path)
        self.assertFalse(os.path.exists(path))

    def test_tampering_breaks_the_seal(self):
        path = self._season_path()
        arena_report.open_season(self.OPEN_PARAMS, now_ms=0, path=path)
        os.chmod(path, 0o644)
        with open(path, encoding="utf-8") as fh:
            record = json.load(fh)
        record["params"]["min_resolved"] = 1  # lower the bar after opening
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(record, fh)
        with self.assertRaisesRegex(ValueError, "seal"):
            arena_report.load_season(path)

    def test_open_after_close_archives_predecessor(self):
        path = self._season_path()
        first = arena_report.open_season('{"days": 1, "min_resolved": 10}',
                                         now_ms=0, path=path)
        second = arena_report.open_season('{"days": 2, "min_resolved": 20}',
                                          now_ms=first["closes_at_ms"], path=path)
        self.assertNotEqual(first["season_id"], second["season_id"])
        archive = os.path.join(os.path.dirname(path),
                               f"arena-season-{first['season_id']}.json")
        with open(archive, encoding="utf-8") as fh:
            self.assertEqual(json.load(fh), first)
        self.assertEqual(arena_report.load_season(path), second)


def windowed_rounds():
    """m2 commits every in-window round but expired every earlier one: if the
    season window leaks, m2's coverage collapses below any floor."""
    def rnd(ts, m2_status):
        return {"status": "DONE", "opened_at_ms": ts, "lanes": [
            {"id": "m1", "status": "COMMITTED_BLIND"},
            {"id": "m2", "status": m2_status}]}
    rounds = [rnd(100 + i, "COMMITTED_BLIND") for i in range(10)]      # in window
    rounds += [rnd(10 + i, "EXPIRED") for i in range(50)]              # before it
    rounds += [rnd(200 + i, "EXPIRED") for i in range(50)]             # after it
    rounds.append({"status": "DONE", "lanes": [                        # no timestamp
        {"id": "m1", "status": "COMMITTED_BLIND"},
        {"id": "m2", "status": "EXPIRED"}]})
    return rounds


class SeasonWindowTest(unittest.TestCase):
    LANES = ReportTest.LANES
    SEASON = {"season_id": "s-test", "opened_at_ms": 100, "closes_at_ms": 200,
              "params": {"days": 14, "min_resolved": 5, "min_coverage": 0.9}}

    @staticmethod
    def separated_score(lane):
        if lane["id"] == "m1":
            return {"brier_pre": 0.10, "ci_low": 0.08, "ci_high": 0.12,
                    "coverage": {"resolved": 100}}
        return {"brier_pre": 0.30, "ci_low": 0.25, "ci_high": 0.35,
                "coverage": {"resolved": 100}}

    def test_report_windows_rounds_and_uses_season_thresholds(self):
        report = arena_report.build_report(
            self.LANES, windowed_rounds(), self.separated_score,
            min_coverage=0.8, min_resolved=50, now_ms=150, season=self.SEASON)
        # Only the 10 in-window rounds count; out-of-window EXPIRED rounds
        # (and the timestamp-less round) must not drag m2 below the floor.
        self.assertEqual(report["rounds_total"], 10)
        by_id = {e["id"]: e for e in report["leaderboard"]}
        self.assertEqual(by_id["m2"]["offered"], 10)
        self.assertEqual(by_id["m2"]["expired"], 0)
        self.assertTrue(by_id["m2"]["comparable"])
        # Thresholds are the season's preregistered ones, not the CLI flags.
        self.assertEqual(report["thresholds"],
                         {"min_coverage": 0.9, "min_resolved": 5})
        self.assertEqual(report["season"]["season_id"], "s-test")
        self.assertEqual(report["season"]["state"], "OPEN")
        self.assertEqual(report["verdict"], "LEADER: m1")

    def test_without_season_out_of_window_rounds_still_count(self):
        report = arena_report.build_report(
            self.LANES, windowed_rounds(), self.separated_score,
            min_coverage=0.8, min_resolved=50, now_ms=150)
        self.assertEqual(report["rounds_total"], 111)
        self.assertIsNone(report["season"])
        by_id = {e["id"]: e for e in report["leaderboard"]}
        self.assertFalse(by_id["m2"]["comparable"])

    def test_cli_score_passes_the_season_window(self):
        calls = []

        class FakeResult:
            stdout = '{"brier_pre": 0.2}'

        def fake_run(cmd, **kwargs):
            calls.append(cmd)
            return FakeResult()

        real_run = arena_report.subprocess.run
        arena_report.subprocess.run = fake_run
        try:
            lane = {"kind": "ollama", "model": "m"}
            out = arena_report.cli_score(lane, cli="cli",
                                         since_ms=100, until_ms=200)
            arena_report.cli_score(lane, cli="cli")
        finally:
            arena_report.subprocess.run = real_run
        self.assertEqual(out, {"brier_pre": 0.2})
        windowed, unwindowed = calls
        self.assertEqual(
            windowed[windowed.index("--since-ms"):][:4],
            ["--since-ms", "100", "--until-ms", "200"])
        self.assertNotIn("--since-ms", unwindowed)
        self.assertNotIn("--until-ms", unwindowed)

    def test_season_status_reports_progress(self):
        season = {"season_id": "s-days", "opened_at_ms": 0,
                  "closes_at_ms": 14 * 86400000,
                  "params": {"days": 14, "min_resolved": 300,
                             "min_coverage": 0.8}}
        status = arena_report.season_status(season, windowed_rounds(),
                                            now_ms=7 * 86400000)
        self.assertEqual(status["state"], "OPEN")
        self.assertEqual(status["elapsed_days"], 7.0)
        self.assertEqual(status["remaining_days"], 7.0)
        # All 110 timestamped rounds fall in this 14-day window; the
        # timestamp-less round is excluded, never guessed into it.
        self.assertEqual(status["rounds_done_in_window"], 110)
        self.assertEqual(status["committed_in_window"], 120)  # m1x110 + m2x10
        self.assertEqual(status["min_resolved_target"], 300)
        self.assertEqual(arena_report.season_state(season, 14 * 86400000),
                         "CLOSED")


class FakeOllamaHandler(BaseHTTPRequestHandler):
    calls = []  # reset per test in setUp

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        type(self).calls.append({"path": self.path, "body": body})
        raw = json.dumps({"done": True}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, *args):
        pass


class WarmLanesTest(unittest.TestCase):
    def setUp(self):
        FakeOllamaHandler.calls = []
        self.server = HTTPServer(("127.0.0.1", 0), FakeOllamaHandler)
        thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(thread.join)
        self.addCleanup(self.server.shutdown)
        self.endpoint = f"http://127.0.0.1:{self.server.server_port}"

    def _lane(self, i, **overrides):
        lane = {"id": f"m{i}", "kind": "ollama", "model": f"model-{i}",
                "endpoint": self.endpoint, "epoch_id": f"e{i}",
                "enabled": True, "warm_timeout_s": 5}
        lane.update(overrides)
        return lane

    def test_warm_hits_every_ollama_lane_and_skips_others(self):
        lanes = [self._lane(1), self._lane(2),
                 self._lane(3, kind="openai_compat")]
        statuses = arena_loop.warm_lanes(lanes)
        self.assertEqual(statuses, {"m1": "OK", "m2": "OK", "m3": "SKIPPED"})
        self.assertEqual([c["path"] for c in FakeOllamaHandler.calls],
                         ["/api/generate", "/api/generate"])
        self.assertEqual([c["body"]["model"] for c in FakeOllamaHandler.calls],
                         ["model-1", "model-2"])
        for call in FakeOllamaHandler.calls:
            self.assertEqual(call["body"]["options"]["num_predict"], 1)
            self.assertFalse(call["body"]["stream"])

    def test_warm_failure_does_not_abort_round_and_is_journaled(self):
        lanes = [self._lane(1),
                 self._lane(2, endpoint="http://127.0.0.1:1", warm_timeout_s=1)]
        appended = []
        real = (arena_loop.load_registry, arena_loop.pick_auto_ticker,
                arena_loop.append_round)
        arena_loop.load_registry = lambda path=None: lanes
        arena_loop.pick_auto_ticker = lambda *a, **k: None
        arena_loop.append_round = lambda rec, path=None: appended.append(rec)
        try:
            rec = arena_loop.run_round()
        finally:
            (arena_loop.load_registry, arena_loop.pick_auto_ticker,
             arena_loop.append_round) = real
        # The dead lane's warm failed, yet the round proceeded past the warm
        # step to contract picking instead of aborting.
        self.assertEqual(rec["status"], "NO_CONTRACT")
        self.assertEqual(rec["warm"]["m1"], "OK")
        self.assertTrue(rec["warm"]["m2"].startswith("FAILED"))
        self.assertEqual([c["body"]["model"] for c in FakeOllamaHandler.calls],
                         ["model-1"])
        self.assertEqual(appended, [rec])  # journal carries per-lane warm status


class ExportHtmlTest(unittest.TestCase):
    def _report(self):
        lanes = [{"id": "m1", "kind": "ollama", "model": "good-model",
                  "epoch_id": "e1"},
                 {"id": "m2", "kind": "ollama",
                  "model": "<script>alert(1)</script>", "epoch_id": "e2"}]
        rounds = [{"status": "DONE", "opened_at_ms": 100 + i, "lanes": [
            {"id": "m1", "status": "COMMITTED_BLIND"},
            {"id": "m2", "status": "COMMITTED_BLIND"}]} for i in range(10)]
        return arena_report.build_report(
            lanes, rounds, SeasonWindowTest.separated_score,
            min_coverage=0.8, min_resolved=50, now_ms=150,
            season=SeasonWindowTest.SEASON)

    def test_html_is_self_contained_and_escaped(self):
        page = arena_report.render_html(self._report())
        self.assertIn("m1", page)
        self.assertIn("good-model", page)
        self.assertIn("LEADER: m1", page)
        self.assertIn("s-test", page)
        # No scripts, no external assets — the page must stand alone.
        self.assertNotIn("<script", page)
        self.assertNotIn("src=", page)
        self.assertNotIn("href=", page)
        self.assertNotIn("<link", page)
        self.assertNotIn("http", page)
        self.assertIn("&lt;script&gt;alert(1)&lt;/script&gt;", page)

    def test_missing_values_render_as_missing(self):
        report = arena_report.build_report(
            ReportTest.LANES, [], lambda lane: None,
            min_coverage=0.8, min_resolved=50, now_ms=0)
        page = arena_report.render_html(report)
        self.assertIn("—", page)
        self.assertIn("INSUFFICIENT_DATA", page)

    def test_export_command_roundtrip_and_missing_report(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        out_html = os.path.join(tmp, "arena-leaderboard.html")
        self.assertEqual(
            arena_report.export_html_command(os.path.join(tmp, "absent.json"),
                                             out_html), 1)
        self.assertFalse(os.path.exists(out_html))
        report_path = os.path.join(tmp, "arena-report.json")
        with open(report_path, "w", encoding="utf-8") as fh:
            json.dump(self._report(), fh)
        self.assertEqual(arena_report.export_html_command(report_path, out_html), 0)
        with open(out_html, encoding="utf-8") as fh:
            page = fh.read()
        self.assertTrue(page.startswith("<!DOCTYPE html>"))
        self.assertIn("LEADER: m1", page)


def synthetic_pairs(distort_a=1.0, distort_b=0.0, n=400, seed=7):
    """Deterministic (predicted, outcome) pairs whose TRUE probability is q
    but whose prediction is sigmoid(distort_a*logit(q) + distort_b) — i.e. a
    forecaster with a known, learnable miscalibration."""
    rng = random.Random(seed)
    pairs = []
    for _ in range(n):
        q = rng.uniform(0.02, 0.98)
        predicted = arena_report.platt_apply(distort_a, distort_b, q)
        outcome = 1 if rng.random() < q else 0
        pairs.append((predicted, outcome))
    return pairs


class ReliabilityBinsTest(unittest.TestCase):
    def test_bin_assignment_counts_and_rates(self):
        pairs = [(0.05, 0), (0.05, 1), (0.62, 1), (0.68, 1), (1.0, 1)]
        bins = arena_report.reliability_bins(pairs, n_bins=10)
        self.assertEqual(len(bins), 10)
        self.assertEqual(bins[0]["count"], 2)
        self.assertAlmostEqual(bins[0]["mean_predicted"], 0.05)
        self.assertAlmostEqual(bins[0]["observed_rate"], 0.5)
        self.assertEqual(bins[6]["count"], 2)
        self.assertAlmostEqual(bins[6]["mean_predicted"], 0.65)
        self.assertAlmostEqual(bins[6]["observed_rate"], 1.0)
        # p == 1.0 belongs to the last bin, not an out-of-range eleventh.
        self.assertEqual(bins[9]["count"], 1)

    def test_empty_bins_read_missing_never_zero(self):
        bins = arena_report.reliability_bins([(0.95, 1)], n_bins=10)
        for b in bins[:9]:
            self.assertEqual(b["count"], 0)
            self.assertIsNone(b["mean_predicted"])
            self.assertIsNone(b["observed_rate"])

    def test_bin_totals_conserve_samples(self):
        pairs = synthetic_pairs()
        bins = arena_report.reliability_bins(pairs)
        self.assertEqual(sum(b["count"] for b in bins), len(pairs))


class PlattFitTest(unittest.TestCase):
    def test_fit_improves_miscalibrated_forecaster(self):
        # Overconfident lane: predictions pushed away from 0.5.
        pairs = synthetic_pairs(distort_a=2.2, distort_b=0.4)
        overlay = arena_report.calibration_overlay(pairs)
        self.assertTrue(overlay["available"])
        self.assertLess(overlay["brier_calibrated"], overlay["brier_raw"])

    def test_fit_does_not_wreck_a_calibrated_forecaster(self):
        pairs = synthetic_pairs(distort_a=1.0, distort_b=0.0)
        overlay = arena_report.calibration_overlay(pairs)
        # In-sample Platt can only match-or-beat by a hair; it must never
        # make a well-calibrated lane read materially worse.
        self.assertLessEqual(overlay["brier_calibrated"],
                             overlay["brier_raw"] + 1e-3)

    def test_fit_recovers_known_distortion(self):
        # Undoing sigmoid(a0*logit(q)+b0) needs a ~= 1/a0, b ~= -b0/a0.
        pairs = synthetic_pairs(distort_a=2.0, distort_b=0.6, n=4000)
        a, b = arena_report.platt_fit(pairs)
        self.assertAlmostEqual(a, 0.5, delta=0.15)
        self.assertAlmostEqual(b, -0.3, delta=0.15)

    def test_degenerate_one_sided_data_stays_finite(self):
        pairs = [(0.9, 1)] * 50
        a, b = arena_report.platt_fit(pairs)
        self.assertTrue(math.isfinite(a) and math.isfinite(b))
        overlay = arena_report.calibration_overlay(pairs)
        self.assertTrue(math.isfinite(overlay["brier_calibrated"]))


class CalibrationOverlayTest(unittest.TestCase):
    LANES = ReportTest.LANES

    @staticmethod
    def rounds():
        return [{"status": "DONE", "lanes": [
            {"id": "m1", "status": "COMMITTED_BLIND"},
            {"id": "m2", "status": "COMMITTED_BLIND"}]}] * 10

    @staticmethod
    def score_with_rows(lane):
        rows = [{"p_pre": p, "outcome_yes": y}
                for p, y in synthetic_pairs(distort_a=2.0, n=100)]
        return {"brier_pre": 0.10 if lane["id"] == "m1" else 0.30,
                "ci_low": 0.08 if lane["id"] == "m1" else 0.25,
                "ci_high": 0.12 if lane["id"] == "m1" else 0.35,
                "coverage": {"resolved": 100}, "rows": rows}

    @staticmethod
    def score_without_rows(lane):
        score = CalibrationOverlayTest.score_with_rows(lane)
        del score["rows"]
        return score

    def _report(self, score_fn):
        return arena_report.build_report(self.LANES, self.rounds(), score_fn,
                                         min_coverage=0.8, min_resolved=50,
                                         now_ms=0)

    def test_overlay_present_and_labeled(self):
        report = self._report(self.score_with_rows)
        self.assertEqual(report["calibration_note"],
                         arena_report.CALIBRATION_LABEL)
        for entry in report["leaderboard"]:
            overlay = entry["calibrated_unofficial"]
            self.assertTrue(overlay["available"])
            self.assertEqual(overlay["label"], arena_report.CALIBRATION_LABEL)
            self.assertIn("UNOFFICIAL", overlay["label"])
            self.assertEqual(overlay["n"], 100)
            self.assertEqual(len(overlay["reliability_bins"]), 10)

    def test_official_brier_and_rank_untouched_by_overlay(self):
        with_rows = self._report(self.score_with_rows)
        without = self._report(self.score_without_rows)
        official = ("brier", "brier_ci", "comparable", "rank", "coverage",
                    "resolved")
        for a, b in zip(with_rows["leaderboard"], without["leaderboard"]):
            for key in official:
                self.assertEqual(a.get(key), b.get(key), key)
        self.assertEqual(with_rows["verdict"], without["verdict"])
        # The overlay's calibrated Brier differs from the official Brier —
        # proof the official field is not silently the calibrated one.
        m1 = with_rows["leaderboard"][0]
        self.assertNotAlmostEqual(
            m1["calibrated_unofficial"]["brier_calibrated"], m1["brier"])

    def test_too_few_pairs_reads_unavailable_not_fabricated(self):
        def sparse(lane):
            return {"brier_pre": 0.2, "coverage": {"resolved": 5},
                    "rows": [{"p_pre": 0.5, "outcome_yes": 1}] * 5}
        report = self._report(sparse)
        overlay = report["leaderboard"][0]["calibrated_unofficial"]
        self.assertFalse(overlay["available"])
        self.assertIn("5/30", overlay["reason"])
        self.assertNotIn("brier_calibrated", overlay)

    def test_missing_rows_reads_unavailable(self):
        report = self._report(self.score_without_rows)
        overlay = report["leaderboard"][0]["calibrated_unofficial"]
        self.assertFalse(overlay["available"])
        self.assertIn("rows unavailable", overlay["reason"])

    def test_invalid_rows_are_skipped_never_guessed(self):
        score = {"rows": [
            {"p_pre": 0.4, "outcome_yes": 1},
            {"p_pre": 1.4, "outcome_yes": 1},      # out of range
            {"p_pre": 0.4, "outcome_yes": 2},      # not binary
            {"p_pre": True, "outcome_yes": 1},     # bool is not a probability
            {"p_pre": 0.4, "outcome_yes": True},   # bool is not an outcome
            "junk", {"p_pre": None, "outcome_yes": None}]}
        self.assertEqual(arena_report.score_rows_to_pairs(score),
                         [(0.4, 1)])

    def test_cli_score_requests_rows_and_falls_back(self):
        calls = []

        class FakeResult:
            def __init__(self, stdout):
                self.stdout = stdout

        def fake_run(cmd, **kwargs):
            calls.append(cmd)
            # An old CLI rejects --include-rows (usage error, empty stdout).
            if "--include-rows" in cmd:
                return FakeResult("")
            return FakeResult('{"brier_pre": 0.2}')

        real_run = arena_report.subprocess.run
        arena_report.subprocess.run = fake_run
        try:
            out = arena_report.cli_score_with_rows(
                {"kind": "ollama", "model": "m"}, cli="cli")
        finally:
            arena_report.subprocess.run = real_run
        self.assertEqual(len(calls), 2)
        self.assertIn("--include-rows", calls[0])
        self.assertNotIn("--include-rows", calls[1])
        # The official score survives the overlay being unavailable.
        self.assertEqual(out, {"brier_pre": 0.2})


if __name__ == "__main__":
    unittest.main()
