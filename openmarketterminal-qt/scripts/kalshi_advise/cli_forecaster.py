#!/usr/bin/env python3
"""Firewall-safe forecaster using the app's OWN configured LLM (no external key).

Implements the forecaster contract (see README.md) by shelling out to
`openterminalcli ai test`, which runs a PURE completion with tools disabled
(`chat(prompt, {}, use_tools=false)`). Because tools are off, the model sees
ONLY the price-free prompt this script hands it — it cannot look up the Kalshi
quote or any market data, so the blind firewall holds.

Advantage over claude_forecaster.py: uses whatever LLM the app has configured
(e.g. a local Ollama model), so it needs no ANTHROPIC_API_KEY and no `anthropic`
SDK. The trade-off is latency — a local model may be too slow for the tightest
crypto-market TTLs (15-30s); constrain the wrapper to longer-dated contracts
(`--auto-min-secs-left 900`) or point the app at a faster model (`ai use ...`).

Contract:
  * `cli_forecaster.py identify`  -> {"provider","model","prompt_version","temperature"}
       (does one tiny `ai test` completion to read the active provider/model)
  * `cli_forecaster.py predict`   (blind context JSON on stdin)
       -> {"probability":0..1, "confidence":0..1, "rationale":"..."}

Env:
  OT_CLI   path to openterminalcli (default: ../../build/openterminalcli)
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.environ.get("OT_CLI", os.path.abspath(os.path.join(HERE, "..", "..", "build", "openterminalcli")))
PROMPT_VERSION = "kalshi-blind-cli-v1"

# FROZEN prompt (prompt_version pins this; edit => bump the version at an epoch boundary)
INSTR = (
    "You are a disciplined probabilistic forecaster for ONE Kalshi crypto settlement "
    "contract. You are given ONLY price-free context (JSON). You do NOT see the contract "
    "price, market-implied probability, order flow, or any model output. Estimate the "
    "probability the contract settles YES from fundamentals (spot, strike, distance, "
    "required move, time left, realized move/vol). Under a roughly driftless short-horizon "
    "random walk, a contract far from its strike with little time left rarely crosses; one "
    "near its strike is closer to a coin flip. Do not anchor to 0.5 out of caution. "
    "Respond with ONLY a JSON object, no prose, no code fences: "
    '{"probability": <0..1>, "confidence": <0..1>, "rationale": "<one sentence, no price references>"}'
)


def ai_test(prompt: str) -> dict:
    r = subprocess.run(
        [CLI, "--json", "--headless", "ai", "test", prompt],
        capture_output=True, text=True, timeout=120,
    )
    if r.returncode != 0 and not r.stdout.strip():
        raise SystemExit("ai test failed: " + (r.stderr.strip() or "unknown"))
    return json.loads(r.stdout)


def clamp01(x) -> float:
    try:
        x = float(x)
    except (TypeError, ValueError):
        return 0.5
    return 0.0 if x < 0.0 else 1.0 if x > 1.0 else x


def main() -> int:
    mode = sys.argv[1] if len(sys.argv) > 1 else "predict"
    if mode == "identify":
        try:
            j = ai_test("Reply with exactly: OK")
        except SystemExit:
            j = {}
        print(json.dumps({
            "provider": j.get("provider", "openterminal-llm"),
            "model": j.get("model", "unknown"),
            "prompt_version": PROMPT_VERSION,
            "temperature": -1,
        }))
        return 0
    if mode == "predict":
        raw = sys.stdin.read()
        try:
            ctx = json.loads(raw) if raw.strip() else {}
        except json.JSONDecodeError as exc:
            print(json.dumps({"error": f"bad blind context json: {exc}"}), file=sys.stderr)
            return 2
        prompt = INSTR + "\n\nContext:\n" + json.dumps(ctx, sort_keys=True)
        j = ai_test(prompt)
        content = (j.get("content") or "").strip()
        m = re.search(r"\{.*\}", content, re.S)  # tolerate stray prose / code fences
        if not m:
            print(json.dumps({"error": "forecaster returned no JSON", "raw": content[:400]}),
                  file=sys.stderr)
            return 4
        parsed = json.loads(m.group(0))
        print(json.dumps({
            "probability": clamp01(parsed.get("probability")),
            "confidence": clamp01(parsed.get("confidence")),
            "rationale": str(parsed.get("rationale", ""))[:500],
        }))
        return 0
    print(f"usage: {sys.argv[0]} identify|predict", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
