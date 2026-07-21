import importlib.util
import json
import os
import subprocess
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts", "kalshi_advise")
sys.path.insert(0, SCRIPTS)
import blind_prompt
from competition_report import compute_result_state, build_report


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(SCRIPTS, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class CompetitionTest(unittest.TestCase):
    def test_prompt_hash_is_key_order_independent_and_shared(self):
        left = {"strike": 66250, "spot": 66180, "seconds_left": 320}
        right = {"seconds_left": 320, "spot": 66180, "strike": 66250}
        self.assertEqual(blind_prompt.prompt_hash(left), blind_prompt.prompt_hash(right))
        self.assertEqual(load("codex_forecaster").PROMPT_VERSION,
                         load("claude_cli_forecaster").PROMPT_VERSION)

    def test_competition_context_is_bounded_and_excludes_nested_feed_arrays(self):
        context = {"spot": 1, "strike_floor": 2, "spot_microstructure": {
            "direction": "up", "tape_pressure": .2, "sources": [{"price": 99}],
            "windows": [{"move_pct": 1}], "divergence": {"contract": 1}}}
        compact = blind_prompt.competition_context(context)
        self.assertEqual(compact["spot_microstructure"], {"direction": "up", "tape_pressure": .2})

    def test_forecaster_identities_are_frozen_and_distinct(self):
        identities = []
        for script in ("codex_forecaster.py", "claude_cli_forecaster.py"):
            run = subprocess.run([sys.executable, os.path.join(SCRIPTS, script), "identify"],
                                 capture_output=True, text=True, check=True)
            identities.append(json.loads(run.stdout))
        self.assertEqual(identities[0]["prompt_version"], identities[1]["prompt_version"])
        self.assertNotEqual(identities[0]["epoch_id"], identities[1]["epoch_id"])
        self.assertEqual(identities[0]["effort"], identities[1]["effort"])

    def test_claude_lockdown_drift_fails_closed(self):
        claude = load("claude_cli_forecaster")
        with self.assertRaisesRegex(RuntimeError, "version"):
            claude.assert_locked_surface("9.9.9", claude.SURFACE_HASH)
        with self.assertRaisesRegex(RuntimeError, "surface"):
            claude.assert_locked_surface(claude.CLI_VERSION, "bad")

    def test_result_states_are_mechanical(self):
        self.assertEqual(compute_result_state(0, 1, 1, -.1, -.01), "INSUFFICIENT_PAIRED_DATA")
        self.assertEqual(compute_result_state(200, .79, 1, -.1, -.01), "INSUFFICIENT_PAIRED_DATA")
        self.assertEqual(compute_result_state(200, .8, .8, -.1, .01), "STATISTICAL_TIE")
        self.assertEqual(compute_result_state(200, .8, .8, -.1, -.01), "CLAUDE_WINS")
        self.assertEqual(compute_result_state(200, .8, .8, .01, .1), "CODEX_WINS")
        self.assertEqual(compute_result_state(500, 1, 1, -.1, -.01, True), "INVALID_EPOCH")

    def test_prompt_divergence_invalidates_epoch(self):
        lanes = [
            {"forecaster":{"provider":"anthropic-claude-cli"},"status":"ABSTAINED",
             "context_hash":"same","forecast":{"prompt_hash":"left"}},
            {"forecaster":{"provider":"openai-codex-cli"},"status":"ABSTAINED",
             "context_hash":"same","forecast":{"prompt_hash":"right"}},
        ]
        report = build_report([{"event":"shadow_opportunity","lanes":lanes}], {})
        self.assertEqual(report["result_state"], "INVALID_EPOCH")
        self.assertIn("PROMPT_HASH_DIVERGENCE", report["invalid_reasons"])


if __name__ == "__main__":
    unittest.main()
