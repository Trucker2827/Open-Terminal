#!/usr/bin/env python3
"""Issue #86 — openterminal_paths is the single source of truth for the
evidence dir and journal DB across python scripts.

Two checks:
  1. The module's defaults and env-override semantics match what the
     migrated scripts did individually before (override value verbatim,
     defaults expanduser'd).
  2. No python script under scripts/ hardcodes either Application Support
     path any more — except the module itself and the frozen v5 duel files,
     which are untouchable by charter invariant.
"""
import os
import sys
import unittest

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
SCRIPTS_DIR = os.path.abspath(os.path.join(TESTS_DIR, "..", "scripts"))
sys.path.insert(0, SCRIPTS_DIR)
import openterminal_paths as otp

ENV_VARS = ("OPENTERMINAL_EVIDENCE_DIR", "OPENTERMINAL_DATA_DB",
            "OPENTERMINAL_BRIDGE_JSON")

# Frozen v5 duel files (charter invariant): they predate the module and may
# not be edited mid-epoch, so their hardcoded literals are exempt.
FROZEN = {"advisor_core.py", "advisor_loop.py", "blind_prompt.py",
          "claude_cli_forecaster.py", "codex_forecaster.py",
          "competition_report.py", "prediction_kalshi.py"}

LITERALS = ("Application Support/Open Terminal/Open Terminal",
            "Application Support/org.openterminal.OpenTerminal")


class DefaultsTest(unittest.TestCase):
    def setUp(self):
        self._saved = {v: os.environ.pop(v, None) for v in ENV_VARS}

    def tearDown(self):
        for var, val in self._saved.items():
            if val is None:
                os.environ.pop(var, None)
            else:
                os.environ[var] = val

    def test_evidence_dir_default(self):
        d = otp.evidence_dir()
        self.assertTrue(d.endswith("Library/Application Support/Open Terminal/Open Terminal"))
        self.assertFalse(d.startswith("~"))

    def test_journal_db_default(self):
        db = otp.journal_db()
        self.assertTrue(db.endswith(
            "Library/Application Support/org.openterminal.OpenTerminal/data/openmarketterminal.db"))
        self.assertFalse(db.startswith("~"))

    def test_bridge_json_default(self):
        bj = otp.bridge_json()
        self.assertTrue(bj.endswith(
            "Library/Application Support/org.openterminal.OpenTerminal/bridge.json"))

    def test_evidence_file_joins(self):
        self.assertEqual(otp.evidence_file("kalshi-ws-books.json"),
                         os.path.join(otp.evidence_dir(), "kalshi-ws-books.json"))

    def test_env_overrides_used_verbatim(self):
        os.environ["OPENTERMINAL_EVIDENCE_DIR"] = "~/ev-dir"   # deliberately un-expanded
        os.environ["OPENTERMINAL_DATA_DB"] = "/tmp/j.db"
        os.environ["OPENTERMINAL_BRIDGE_JSON"] = "/tmp/b.json"
        self.assertEqual(otp.evidence_dir(), "~/ev-dir")
        self.assertEqual(otp.evidence_file("x.json"), os.path.join("~/ev-dir", "x.json"))
        self.assertEqual(otp.journal_db(), "/tmp/j.db")
        self.assertEqual(otp.bridge_json(), "/tmp/b.json")


class NoHardcodedLiteralsTest(unittest.TestCase):
    def test_scripts_tree_is_clean(self):
        offenders = []
        for root, dirs, files in os.walk(SCRIPTS_DIR):
            dirs[:] = [d for d in dirs
                       if not d.startswith(".") and d not in ("__pycache__", "node_modules")]
            for name in files:
                if not name.endswith(".py") or name in FROZEN:
                    continue
                if name == "openterminal_paths.py" and root == SCRIPTS_DIR:
                    continue
                path = os.path.join(root, name)
                try:
                    with open(path, encoding="utf-8", errors="replace") as fh:
                        text = fh.read()
                except OSError:
                    continue
                for lit in LITERALS:
                    if lit in text:
                        offenders.append(f"{os.path.relpath(path, SCRIPTS_DIR)}: {lit}")
        self.assertEqual(offenders, [],
                         "hardcoded evidence/data path literals — import openterminal_paths instead:\n"
                         + "\n".join(offenders))


if __name__ == "__main__":
    unittest.main()
