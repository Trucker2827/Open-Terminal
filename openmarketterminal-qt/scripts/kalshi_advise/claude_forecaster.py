#!/usr/bin/env python3
"""Reference forecaster for the LLM advisory scoring trial (Claude API).

Contract (see README.md):
  * `claude_forecaster.py identify`
        -> stdout JSON {"provider","model","prompt_version","temperature"}
  * `claude_forecaster.py predict`  (blind context JSON on stdin)
        -> stdout JSON {"probability":0..1, "confidence":0..1, "rationale":"..."}

The blind context is the PRICE-FREE packet emitted by `kalshi auto advise open`.
This forecaster is handed ONLY that packet; it never sees the Kalshi quote,
market-implied probability, daemon probability, or contract order flow. Its job
is to form an INDEPENDENT probability that the contract settles YES.

This is the *frozen experimental unit* — provider + model + prompt_version are
recorded on every challenge so results can be attributed. Freeze them for the
duration of a trial epoch. Change the model/prompt only at an epoch boundary.

Env overrides (so the frozen identity is explicit and versioned):
  ANTHROPIC_MODEL                default: claude-opus-4-8
  KALSHI_FORECASTER_PROMPT_VER   default: kalshi-blind-v1
  KALSHI_FORECASTER_EFFORT       default: low        (low|medium|high|xhigh|max)
  KALSHI_FORECASTER_FAST         default: 0          (1 = Opus fast mode, for the ~30s window)

Auth: the Anthropic() client resolves ANTHROPIC_API_KEY, ANTHROPIC_AUTH_TOKEN,
or an `ant auth login` profile automatically. No key is read or logged here.
"""
import json
import os
import sys

MODEL = os.environ.get("ANTHROPIC_MODEL", "claude-opus-4-8")
PROMPT_VERSION = os.environ.get("KALSHI_FORECASTER_PROMPT_VER", "kalshi-blind-v1")
EFFORT = os.environ.get("KALSHI_FORECASTER_EFFORT", "low")
FAST = os.environ.get("KALSHI_FORECASTER_FAST", "0") == "1"

# --- FROZEN prompt (prompt_version pins this text; edit => bump the version) ---
SYSTEM_PROMPT = (
    "You are a disciplined probabilistic forecaster for a single Kalshi crypto "
    "settlement contract. You are given ONLY price-free context about the "
    "underlying and the contract's structure. You do NOT see the contract's "
    "market price, the market-implied probability, any order-flow, or any model "
    "output. Form your own INDEPENDENT estimate of the probability that this "
    "contract settles YES.\n\n"
    "Reason from fundamentals: the underlying spot level, the strike, the "
    "distance to the strike, the required move to cross it, the time remaining, "
    "realized recent movement, and the realized-volatility estimate. Under a "
    "roughly driftless short-horizon random walk, a contract far from its strike "
    "with little time left is unlikely to cross; one near its strike is closer "
    "to a coin flip. Do not anchor to 0.5 out of caution — commit to the "
    "estimate the evidence supports. Return your probability that the contract "
    "settles YES, a confidence in your own estimate, and one sentence of "
    "rationale. Do not ask for the price; you will never receive it."
)

SCHEMA = {
    "type": "object",
    "properties": {
        "probability": {"type": "number", "description": "P(contract settles YES), 0..1"},
        "confidence": {"type": "number", "description": "Your confidence in this estimate, 0..1"},
        "rationale": {"type": "string", "description": "One sentence, no price references"},
    },
    "required": ["probability", "confidence", "rationale"],
    "additionalProperties": False,
}


def identify() -> dict:
    return {
        "provider": "anthropic",
        "model": MODEL,
        "prompt_version": PROMPT_VERSION,
        "temperature": -1,  # sampling params are not accepted on Opus 4.8; sentinel
    }


def _clamp01(x: float) -> float:
    try:
        x = float(x)
    except (TypeError, ValueError):
        return 0.5
    if x < 0.0:
        return 0.0
    if x > 1.0:
        return 1.0
    return x


def predict(blind_ctx: dict) -> dict:
    import anthropic  # imported lazily so `identify` works without the SDK

    client = anthropic.Anthropic()
    user = (
        "Price-free context for the contract (JSON). Estimate P(settles YES).\n\n"
        + json.dumps(blind_ctx, sort_keys=True, indent=2)
    )
    output_config = {"format": {"type": "json_schema", "schema": SCHEMA}, "effort": EFFORT}
    kwargs = dict(
        model=MODEL,
        max_tokens=1024,
        system=SYSTEM_PROMPT,
        messages=[{"role": "user", "content": user}],
        output_config=output_config,
    )
    # Thinking is off by default on Opus 4.8 — omit it for latency inside the
    # short prediction window. Fast mode is the extra lever when the window is
    # tight (Opus 4.8/4.7 only; premium pricing).
    if FAST:
        resp = client.beta.messages.create(speed="fast", betas=["fast-mode-2026-02-01"], **kwargs)
    else:
        resp = client.messages.create(**kwargs)

    if resp.stop_reason == "refusal":
        raise SystemExit("forecaster: model refused the request")
    text = next((b.text for b in resp.content if b.type == "text"), None)
    if not text:
        raise SystemExit("forecaster: empty model response")
    parsed = json.loads(text)
    return {
        "probability": _clamp01(parsed.get("probability")),
        "confidence": _clamp01(parsed.get("confidence")),
        "rationale": str(parsed.get("rationale", ""))[:500],
    }


def main() -> int:
    mode = sys.argv[1] if len(sys.argv) > 1 else "predict"
    if mode == "identify":
        print(json.dumps(identify()))
        return 0
    if mode == "predict":
        raw = sys.stdin.read()
        try:
            blind_ctx = json.loads(raw) if raw.strip() else {}
        except json.JSONDecodeError as exc:
            print(json.dumps({"error": f"bad blind context json: {exc}"}), file=sys.stderr)
            return 2
        print(json.dumps(predict(blind_ctx)))
        return 0
    print(f"usage: {sys.argv[0]} identify|predict", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
