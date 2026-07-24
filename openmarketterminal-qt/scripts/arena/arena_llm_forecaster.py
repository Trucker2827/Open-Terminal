#!/usr/bin/env python3
"""Alpha Arena generic LLM forecaster adapter.

Drives any registry contestant (ollama today, openai_compat-ready) with the
SAME canonical instruction + blind packet the duel uses (imported from the
frozen blind_prompt.py — importing does not modify it), and returns the same
forecast contract the duel forecasters return:

  {"decision": "predict"|"abstain", "probability", "confidence", "rationale",
   "prompt_hash", "epoch_id", "reason_code"?}

Honesty rules: any transport/parse/shape failure is an ABSTAIN with a reason
code — never a made-up probability. Ollama is asked for format=json and
temperature 0 so weak models stay parseable and deterministic-ish.
"""
import json
import os
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "kalshi_advise")))
from blind_prompt import PROMPT_VERSION, build_prompt, prompt_hash  # frozen; import-only


def parse_model_reply(text):
    """Extract {probability, confidence, rationale} from a model reply.

    Accepts a bare JSON object or JSON embedded in prose (first balanced
    object). Returns (dict, None) or (None, reason)."""
    text = (text or "").strip()
    candidates = []
    if text.startswith("{"):
        candidates.append(text)
    depth, start, in_str = 0, -1, False
    i = 0
    while i < len(text):
        c = text[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == "{":
            if depth == 0:
                start = i
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0 and start >= 0:
                candidates.append(text[start:i + 1])
                start = -1
        i += 1
    for cand in candidates:
        try:
            obj = json.loads(cand)
        except ValueError:
            continue
        if not isinstance(obj, dict):
            continue
        prob = obj.get("probability", obj.get("p", obj.get("prob")))
        try:
            prob = float(prob)
        except (TypeError, ValueError):
            continue
        if not 0.0 <= prob <= 1.0:
            continue
        conf = obj.get("confidence", -1)
        try:
            conf = float(conf)
        except (TypeError, ValueError):
            conf = -1.0
        return ({"probability": prob, "confidence": conf,
                 "rationale": str(obj.get("rationale", ""))[:400]}, None)
    return (None, "UNPARSEABLE_REPLY")


def ollama_digest(entry):
    """The installed model's digest — version pinning for local lanes."""
    try:
        with urllib.request.urlopen(entry["endpoint"].rstrip("/") + "/api/tags",
                                    timeout=10) as resp:
            tags = json.loads(resp.read().decode("utf-8"))
        for m in tags.get("models", []):
            if m.get("name") == entry["model"]:
                return m.get("digest", "")[:16]
    except Exception:
        pass
    return ""


def identify(entry):
    return {
        "provider": f"arena-{entry['kind']}",
        "model": entry["model"],
        "prompt_version": PROMPT_VERSION,
        "epoch_id": entry["epoch_id"],
        "agent_id": f"arena/{entry['id']}",
        "model_digest": ollama_digest(entry) if entry["kind"] == "ollama" else "",
        "timeout_s": int(entry.get("timeout_s", 88)),
    }


def _chat(entry, prompt, timeout_s):
    if entry["kind"] == "ollama":
        body = json.dumps({
            "model": entry["model"],
            "messages": [{"role": "user", "content": prompt}],
            "stream": False,
            "format": "json",
            "options": {"temperature": 0},
        }).encode("utf-8")
        req = urllib.request.Request(
            entry["endpoint"].rstrip("/") + "/api/chat", data=body,
            headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
        return (payload.get("message") or {}).get("content", "")
    if entry["kind"] == "openai_compat":
        body = json.dumps({
            "model": entry["model"],
            "messages": [{"role": "user", "content": prompt}],
            "temperature": 0,
            "response_format": {"type": "json_object"},
        }).encode("utf-8")
        headers = {"Content-Type": "application/json"}
        api_key = os.environ.get(entry.get("api_key_env", ""), "")
        if api_key:
            headers["Authorization"] = f"Bearer {api_key}"
        req = urllib.request.Request(
            entry["endpoint"].rstrip("/") + "/v1/chat/completions", data=body,
            headers=headers, method="POST")
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
        choices = payload.get("choices") or [{}]
        return (choices[0].get("message") or {}).get("content", "")
    raise ValueError(f"unknown forecaster kind: {entry['kind']}")


def predict(entry, context):
    """Run one blind forecast. Never raises; failures abstain with a reason."""
    epoch = entry["epoch_id"]
    ph = prompt_hash(context)
    timeout_s = int(entry.get("timeout_s", 88))
    import socket
    try:
        reply = _chat(entry, build_prompt(context), timeout_s)
    except (socket.timeout, TimeoutError) as exc:
        return {"decision": "abstain", "reason_code": "FORECAST_TIMEOUT",
                "confidence": 0,
                "rationale": f"exceeded the {timeout_s}s blind prediction budget",
                "prompt_hash": ph, "epoch_id": epoch}
    except Exception as exc:
        # urllib wraps socket timeouts in URLError(reason=timeout) — classify.
        reason = getattr(exc, "reason", None)
        if isinstance(reason, (socket.timeout, TimeoutError)) or "timed out" in str(exc):
            return {"decision": "abstain", "reason_code": "FORECAST_TIMEOUT",
                    "confidence": 0,
                    "rationale": f"exceeded the {timeout_s}s blind prediction budget",
                    "prompt_hash": ph, "epoch_id": epoch}
        return {"decision": "abstain", "reason_code": "MODEL_UNAVAILABLE",
                "confidence": 0, "rationale": str(exc)[:300],
                "prompt_hash": ph, "epoch_id": epoch}
    parsed, reason = parse_model_reply(reply)
    if parsed is None:
        return {"decision": "abstain", "reason_code": reason,
                "confidence": 0, "rationale": (reply or "")[:300],
                "prompt_hash": ph, "epoch_id": epoch}
    return {"decision": "predict", "probability": parsed["probability"],
            "confidence": parsed["confidence"], "rationale": parsed["rationale"],
            "prompt_hash": ph, "epoch_id": epoch}
