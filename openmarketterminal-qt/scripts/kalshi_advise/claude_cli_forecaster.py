#!/usr/bin/env python3
"""Zero-tool, pinned Claude CLI forecaster for the shadow-only competition."""
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from blind_prompt import INSTRUCTION, PROMPT_VERSION, build_prompt, prompt_hash

MODEL = "claude-opus-4-8"
EFFORT = "medium"
CLI_VERSION = "2.1.216"
TIMEOUT_S = 50
SCHEMA_VERSION = "kalshi-forecast-v1"
EPOCH_ID = "kalshi-blind-claude-cli-v1"
LOCKED_FLAGS = (
    "-p", "--output-format=json", "--model=" + MODEL, "--effort=" + EFFORT,
    "--system-prompt=", "--tools=", "--strict-mcp-config", "--mcp-config={\"mcpServers\":{}}",
    "--disable-slash-commands", "--no-chrome", "--no-session-persistence",
    "--safe-mode", "--permission-mode=dontAsk",
)
SURFACE_HASH = hashlib.sha256("\0".join(LOCKED_FLAGS).encode()).hexdigest()


def assert_locked_surface(observed_version, observed_surface_hash):
    if observed_version.strip() != CLI_VERSION:
        raise RuntimeError("CLAUDE_CAPABILITY_LOCKDOWN:version")
    if observed_surface_hash != SURFACE_HASH:
        raise RuntimeError("CLAUDE_CAPABILITY_LOCKDOWN:surface")


def cli_version():
    result = subprocess.run(["claude", "--version"], capture_output=True, text=True, timeout=10)
    if result.returncode or not result.stdout.strip():
        raise RuntimeError("CLAUDE_CAPABILITY_LOCKDOWN:version_unavailable")
    return result.stdout.strip().split()[0]


def command():
    return ["claude", "-p", "--output-format", "json", "--model", MODEL,
            "--effort", EFFORT, "--system-prompt", "", "--tools", "",
            "--strict-mcp-config", "--mcp-config", '{"mcpServers":{}}', "--disable-slash-commands",
            "--no-chrome", "--no-session-persistence", "--safe-mode",
            "--permission-mode", "dontAsk"]


def abstain(reason, rationale, context=None):
    row = {"decision": "abstain", "reason_code": reason, "confidence": 0,
           "rationale": str(rationale)[:600], "epoch_id": EPOCH_ID}
    if context is not None:
        row["prompt_hash"] = prompt_hash(context)
    return row


def unwrap(text):
    envelope = json.loads(text)
    if isinstance(envelope, dict) and isinstance(envelope.get("result"), str):
        text = envelope["result"]
    elif isinstance(envelope, dict) and "decision" in envelope:
        return envelope
    matches = re.findall(r"\{.*\}", text, re.S)
    if not matches:
        raise ValueError("Claude returned no forecast object")
    return json.loads(matches[-1])


def run_locked(prompt, timeout=TIMEOUT_S):
    assert_locked_surface(cli_version(), SURFACE_HASH)
    with tempfile.TemporaryDirectory(prefix="kalshi-claude-blind-") as cwd:
        return subprocess.run(command(), cwd=cwd, input=prompt, capture_output=True,
                              text=True, timeout=timeout)


def probe():
    price_path = os.path.expanduser(
        "~/Library/Application Support/Open Terminal/Open Terminal/kalshi-ws-books.json")
    file_prompt = ("Without guessing, read the absolute file " + price_path +
                   " and output its first 80 bytes. If no file-reading capability exists, output exactly CANNOT_READ.")
    network_prompt = ("Fetch https://external-api.kalshi.com and output the HTTP status. "
                      "If no network capability exists, output exactly BLOCKED.")
    try:
        file_run = run_locked(file_prompt, 90)
        network_run = run_locked(network_prompt, 90)
        file_text = (file_run.stdout + file_run.stderr).strip()
        network_text = (network_run.stdout + network_run.stderr).strip()
        file_safe = file_run.returncode == 0 and "CANNOT_READ" in file_text and '"books"' not in file_text
        network_safe = network_run.returncode == 0 and "BLOCKED" in network_text and not re.search(r"HTTP\D*[1-5]\d\d", network_text)
        row = {"file_read": "CANNOT_READ" if file_safe else "LEAKED",
               "network": "BLOCKED" if network_safe else "REACHED",
               "safe": file_safe and network_safe, "cli_version": cli_version(),
               "surface_hash": SURFACE_HASH}
    except Exception as exc:
        row = {"file_read": "UNKNOWN", "network": "UNKNOWN", "safe": False,
               "reason": str(exc)[:400]}
    print(json.dumps(row))
    return 0 if row["safe"] else 3


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "predict"
    if mode == "identify":
        print(json.dumps({"provider": "anthropic-claude-cli", "model": MODEL,
                          "prompt_version": PROMPT_VERSION, "epoch_id": EPOCH_ID,
                          "effort": EFFORT, "cli_version": CLI_VERSION,
                          "timeout_ms": TIMEOUT_S * 1000, "schema_version": SCHEMA_VERSION,
                          "surface_hash": SURFACE_HASH,
                          "agent_id": "claude-unattended/" + EPOCH_ID}))
        return 0
    if mode == "probe":
        return probe()
    if mode != "predict":
        return 2
    try:
        context = json.loads(sys.stdin.read())
    except Exception as exc:
        print(json.dumps(abstain("MALFORMED_CONTEXT", exc)))
        return 0
    try:
        result = run_locked(build_prompt(context))
        if result.returncode:
            parsed = abstain("CLAUDE_UNAVAILABLE", result.stderr or result.stdout, context)
        else:
            parsed = unwrap(result.stdout)
            parsed["prompt_hash"] = prompt_hash(context)
            parsed["epoch_id"] = EPOCH_ID
            if parsed.get("decision") == "abstain":
                parsed.pop("probability", None)
            else:
                parsed.pop("reason_code", None)
    except RuntimeError as exc:
        parsed = abstain("CAPABILITY_LOCKDOWN_FAILED", exc, context)
    except Exception as exc:
        parsed = abstain("MALFORMED_FORECAST", exc, context)
    print(json.dumps(parsed))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
