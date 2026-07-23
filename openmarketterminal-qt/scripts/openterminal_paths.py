#!/usr/bin/env python3
"""Single source of truth for Open Terminal's evidence/data paths (issue #86).

Two DIFFERENT Application Support directories exist by history, and both are
correct — do not "unify" them to look consistent:

  evidence dir  ~/Library/Application Support/Open Terminal/Open Terminal/
                JSON evidence the Qt app exports for scripts to read
                (kalshi-ws-books.json, quant-signals.json, calibrator.json, ...)
  app data dir  ~/Library/Application Support/org.openterminal.OpenTerminal/
                the app's own data root: bridge.json plus the journal/tick DB
                at data/openmarketterminal.db

Env overrides keep their historical names and semantics: the override value
is used verbatim (no expanduser), exactly as each script behaved before this
module existed.

  OPENTERMINAL_EVIDENCE_DIR  -> evidence_dir()
  OPENTERMINAL_DATA_DB       -> journal_db()
  OPENTERMINAL_BRIDGE_JSON   -> bridge_json()
"""
import os

_EVIDENCE_DIR_DEFAULT = "~/Library/Application Support/Open Terminal/Open Terminal"
_APP_DATA_DIR_DEFAULT = "~/Library/Application Support/org.openterminal.OpenTerminal"


def evidence_dir():
    """Directory the Qt app exports evidence JSON into."""
    return os.environ.get("OPENTERMINAL_EVIDENCE_DIR",
                          os.path.expanduser(_EVIDENCE_DIR_DEFAULT))


def evidence_file(name):
    """Path of one file inside the evidence dir (e.g. "kalshi-ws-books.json")."""
    return os.path.join(evidence_dir(), name)


def app_data_dir():
    """The app's data root (QStandardPaths AppDataLocation)."""
    return os.path.expanduser(_APP_DATA_DIR_DEFAULT)


def journal_db():
    """SQLite journal/tick database (ticks, edge journal, crypto dataset)."""
    return os.environ.get("OPENTERMINAL_DATA_DB",
                          os.path.join(app_data_dir(), "data", "openmarketterminal.db"))


def bridge_json():
    """bridge.json written by the running app/daemon (endpoint + token)."""
    return os.environ.get("OPENTERMINAL_BRIDGE_JSON",
                          os.path.join(app_data_dir(), "bridge.json"))
