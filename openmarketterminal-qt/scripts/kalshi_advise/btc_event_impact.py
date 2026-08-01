#!/usr/bin/env python3
"""LLM event-impact scorer: pulse stories -> {direction, magnitude, half_life}
per significant BTC event. Mirrors claude_forecaster.py's frozen-identity,
lazy-SDK, no-key-logged contract. Pure stdin->stdout; the model call is
injectable so tests never touch the network."""
import json, os, sys

MODEL = os.environ.get("ANTHROPIC_MODEL", "claude-opus-4-8")
PROMPT_VERSION = "event-impact-v1"     # edit the prompt => bump this
EFFORT = os.environ.get("BTC_EVENT_IMPACT_EFFORT", "low")
_MODEL_CALL_FOR_TEST = None            # test seam; None in production

SYSTEM_PROMPT = (
    "You are a disciplined market-impact analyst for Bitcoin. Given recent news "
    "stories, identify ONLY events likely to move BTC's price materially over the "
    "next minutes-to-hours, and score each as a signed, decaying impulse. Abstain "
    "on routine or ambiguous news (return no event for it). A hack, exploit, or "
    "large theft from an exchange or bridge is bearish with a multi-hour half-life; "
    "an ETF inflow or favorable ruling is bullish. direction in [-1,1] "
    "(-1 strongly bearish, +1 strongly bullish); magnitude in [0,1] is expected "
    "price impact, NOT headline drama; half_life_hours > 0 is how fast it fades. "
    "event_ts_ms is the story's own publish time. Do not invent events not present "
    "in the stories."
)

SCHEMA = {  # json_schema for the model's structured output (events array)
    "type": "object",
    "properties": {"events": {"type": "array", "items": {
        "type": "object",
        "properties": {
            "event_ts_ms": {"type": "integer"},
            "direction": {"type": "number"}, "magnitude": {"type": "number"},
            "half_life_hours": {"type": "number"},
            "kind": {"type": "string"}, "headline": {"type": "string"},
            "rationale": {"type": "string"},
        },
        "required": ["event_ts_ms", "direction", "magnitude", "half_life_hours", "kind", "headline", "rationale"],
        "additionalProperties": False,
    }}},
    "required": ["events"],
    "additionalProperties": False,
}

def identify() -> dict:
    return {"provider": "anthropic", "model": MODEL, "prompt_version": PROMPT_VERSION}

def _clamp(x, lo, hi, default=None):
    try:
        x = float(x)
    except (TypeError, ValueError):
        return default
    return lo if x < lo else hi if x > hi else x

def _live_model_call(system, user):
    import anthropic  # lazy: identify() works without the SDK
    client = anthropic.Anthropic()
    resp = client.messages.create(
        model=MODEL, max_tokens=2048, system=system,
        messages=[{"role": "user", "content": user}],
        output_config={"format": {"type": "json_schema", "schema": SCHEMA}, "effort": EFFORT})
    if resp.stop_reason == "refusal":
        raise SystemExit("event-impact: model refused")
    text = next((b.text for b in resp.content if b.type == "text"), None)
    if not text:
        raise SystemExit("event-impact: empty response")
    return json.loads(text)

def score(ctx: dict, model_call=None) -> dict:
    as_of_ms = int(ctx.get("as_of_ms"))
    stories = ctx.get("stories") or []
    call = model_call or _MODEL_CALL_FOR_TEST or _live_model_call
    user = ("Stories (JSON). Score only materially price-moving BTC events; abstain otherwise.\n\n"
            + json.dumps({"as_of_ms": as_of_ms, "stories": stories}, sort_keys=True, indent=2))
    raw = call(SYSTEM_PROMPT, user) or {}
    events = []
    for e in (raw.get("events") or []):
        try:
            ts = int(e["event_ts_ms"])
        except (KeyError, TypeError, ValueError):
            continue
        if ts > as_of_ms:                      # no look-ahead
            continue
        try:
            hl_raw = float(e.get("half_life_hours"))
        except (TypeError, ValueError):
            continue
        if hl_raw <= 0.0:                      # no honest decay without a positive half-life
            continue
        hl = _clamp(hl_raw, 1e-6, 1e9)
        d = _clamp(e.get("direction"), -1.0, 1.0)
        m = _clamp(e.get("magnitude"), 0.0, 1.0)
        if d is None or m is None:
            continue
        events.append({"event_ts_ms": ts, "direction": d, "magnitude": m,
                       "half_life_hours": hl, "kind": str(e.get("kind", ""))[:60],
                       "headline": str(e.get("headline", ""))[:300],
                       "rationale": str(e.get("rationale", ""))[:500]})
    return {"as_of_ms": str(as_of_ms), "events": events,
            "model": MODEL, "prompt_version": PROMPT_VERSION}

def main(argv=None, stdin=None, stdout=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    stdin = stdin or sys.stdin
    stdout = stdout or sys.stdout
    mode = argv[0] if argv else "score"
    if mode == "identify":
        stdout.write(json.dumps(identify()) + "\n"); return 0
    if mode == "score":
        raw = stdin.read()
        try:
            ctx = json.loads(raw) if raw.strip() else {"as_of_ms": 0, "stories": []}
        except json.JSONDecodeError as exc:
            sys.stderr.write(json.dumps({"error": f"bad context json: {exc}"}) + "\n"); return 2
        stdout.write(json.dumps(score(ctx)) + "\n"); return 0
    sys.stderr.write(f"usage: {sys.argv[0]} identify|score\n"); return 2

if __name__ == "__main__":
    raise SystemExit(main())
