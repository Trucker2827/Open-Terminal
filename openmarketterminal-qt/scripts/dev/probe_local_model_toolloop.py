#!/usr/bin/env python3
"""Probe a local (Ollama) model against OpenMarketTerminal's *actual* tool loop.

Why this exists
---------------
OpenMarketTerminal's AI features (analysis playbooks, notebooks, report builder)
run a multi-turn tool loop: send the system prompt + structured tools, the model
emits tool calls, we feed results back, repeat until it answers. Premium Claude is
the reliable engine; local models (llama3.x via Ollama, etc.) are a best-effort
fallback. The open question is always: *where* does a given local model break down
on the real path — and a single-turn "what's AAPL?" call cannot answer it.

This harness mirrors the live path faithfully so the answer is measured, not guessed:
  * the OpenAI-compatible endpoint (/v1/chat/completions) the app uses for Ollama
  * a STRUCTURED `tools` array (the app sends these via format_tools_for_openai)
  * MULTI-TURN: tool results are fed back as role:tool messages and the loop continues
  * a real MULTI-TOOL task (a mini comps flow), not a single lookup
  * canned, deterministic tool results (no network/SEC dependency)

It logs, per turn, whether the model used a structured tool_call, emitted a tool
call as bare text JSON (the shape the C++ TextToolCallParser salvages), or stopped
with prose — so you can see exactly where degradation occurs (turn 1 vs mid-loop).

It A/Bs two system prompts:
  * "full"  — the heavy, Claude-tuned default prompt the app ships
  * "lean"  — a tight local-model-friendly prompt
so you can tell whether the giant prompt is itself the thing tripping a small model.

Usage
-----
    python3 probe_local_model_toolloop.py                      # both prompts, llama3.3
    python3 probe_local_model_toolloop.py --model qwen2.5:14b  # any loaded model
    python3 probe_local_model_toolloop.py --prompt lean        # just the lean prompt
    python3 probe_local_model_toolloop.py --host http://localhost:11434

Exit code is 0 if at least one prompt variant completed the task (model produced a
final answer after using the required tools), else 1.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import urllib.request

# --------------------------------------------------------------------------- #
# The two system prompts under test.
# --------------------------------------------------------------------------- #

# A faithful condensation of the app's shipped default prompt (LlmService.cpp).
# Kept long on purpose — this is the "is the giant prompt the problem?" arm.
FULL_PROMPT = (
    "You are OpenMarketTerminal AI, the intelligent assistant embedded inside the "
    "OpenMarketTerminal — a professional desktop financial intelligence application. "
    "You have access to tools that let you interact with the terminal directly: "
    "navigate screens, fetch live market data, manage watchlists, query portfolios, "
    "paper-trade, run Python analytics, search SEC Edgar filings, fetch news, and "
    "BUILD REPORTS LIVE in the Report Builder.\n\n"
    "Behaviour rules:\n"
    "• ALWAYS use a tool when one can fulfil the request — never decline an action that "
    "a tool exists for. Never tell the user you cannot navigate or open screens.\n"
    "• Building a report: your job is to WRITE THE REPORT INTO THE REPORT BUILDER USING "
    "TOOLS. Do not narrate the report into the chat. STEP 0 (mandatory): call "
    "report_session_context to see whether this chat is already authoring a report. If "
    "linked, CONTINUE editing it. If unlinked and the canvas has components, call "
    "report_clear first UNLESS the user said 'edit this report'. The flow is: (1) DESIGN "
    "A UNIQUE STRUCTURE for the subject — every ticker, sector, or topic deserves its own "
    "outline. Decide which sections actually matter for THIS subject (Catalysts, Bear "
    "Case, Capital Structure, Unit Economics, Regulatory Overhang, Insider Activity, "
    "Optionality, etc.) — do NOT default to a generic template. (2) call "
    "report_set_metadata with a subject-specific title, author, company, date. (3) build "
    "the structure by calling report_add_component for each section. (4) gather data with "
    "tools like get_equity_info, get_quote, edgar_get_financials, edgar_10k_sections, "
    "edgar_calc_multiples, get_equity_news, search_news. (5) populate each section by "
    "calling report_update_component with the gathered data. Use stable component ids "
    "returned by report_add_component — never indices. COVERAGE: before declaring the "
    "report done, verify every section has real content.\n"
    "• Report formatting: text/list/quote/callout content SUPPORTS MARKDOWN. Use **bold** "
    "to highlight key figures. For tables, ALWAYS pass real data via "
    "config={'csv':'Header1,Header2|Cell1,Cell2|...'}. For charts, pass "
    "config={'chart_type':'bar'|'line'|'pie','title':...,'data':'1,2,3','labels':'Q1,Q2'}. "
    "Set proper metadata FIRST via report_set_metadata. Avoid one-line ramblings.\n"
    "• Python scripts: ONLY pass script names returned by list_python_scripts. Never "
    "invent or guess script names. If list_python_scripts returns nothing useful, fall "
    "back to other tools (get_quote, edgar_*, get_candles, etc.).\n"
    "• When you have completed the user's request, reply with a concise summary in chat. "
    "Do not paste the full report content into chat.\n"
    "Be concise, accurate, and finance-focused.\n\n"
    "[Tool discovery] You see only a small subset of tools each turn. To find a tool for "
    "any action you don't already have, call tool_list(query=\"<description>\"). It returns "
    "the top 5 most relevant tools. Then call tool_describe(name) for the full input "
    "schema, then invoke it. For requests with multiple intents, call tool_list MULTIPLE "
    "TIMES. Never decline an action you can fulfil via a discoverable tool."
)

# A tight, local-model-friendly prompt: the minimum the loop actually needs.
LEAN_PROMPT = (
    "You are OpenMarketTerminal AI, a finance assistant with tools.\n"
    "Rules:\n"
    "1. To get data, CALL A TOOL. Do not guess numbers.\n"
    "2. After each tool result, decide: if you still need more data, call the next tool; "
    "if you have everything, give the final answer in plain text.\n"
    "3. Always either call a tool or give a final answer — never stop in between.\n"
    "Be concise and finance-focused."
)

PROMPTS = {"full": FULL_PROMPT, "lean": LEAN_PROMPT}

# --------------------------------------------------------------------------- #
# Structured tools (shape the app sends) + canned deterministic results.
# --------------------------------------------------------------------------- #

TOOLS = [
    {"type": "function", "function": {
        "name": "get_quote",
        "description": "Latest price for a stock ticker.",
        "parameters": {"type": "object",
                       "properties": {"symbol": {"type": "string", "description": "Ticker, e.g. AAPL"}},
                       "required": ["symbol"]}}},
    {"type": "function", "function": {
        "name": "edgar_get_financials",
        "description": "Latest annual revenue and net income from SEC filings.",
        "parameters": {"type": "object",
                       "properties": {"ticker": {"type": "string", "description": "Ticker, e.g. MSFT"}},
                       "required": ["ticker"]}}},
]

CANNED = {
    ("get_quote", "AAPL"): {"symbol": "AAPL", "price": 212.45, "change_pct": 0.83},
    ("get_quote", "MSFT"): {"symbol": "MSFT", "price": 467.10, "change_pct": -0.21},
    ("edgar_get_financials", "AAPL"): {"ticker": "AAPL", "revenue_usd": 391035000000, "net_income_usd": 93736000000, "fy": 2024},
    ("edgar_get_financials", "MSFT"): {"ticker": "MSFT", "revenue_usd": 245122000000, "net_income_usd": 88136000000, "fy": 2024},
}

TASK = ("Compare Apple (AAPL) and Microsoft (MSFT): fetch the current quote AND the "
        "latest annual revenue for BOTH companies, then give me a two-sentence comparison "
        "of their size and momentum.")

# Tools that must fire for the task to count as "completed".
REQUIRED_CALLS = {("get_quote", "AAPL"), ("get_quote", "MSFT"),
                  ("edgar_get_financials", "AAPL"), ("edgar_get_financials", "MSFT")}


def canned_result(name: str, args: dict) -> dict:
    key = (name, str(args.get("symbol") or args.get("ticker") or "").upper())
    return CANNED.get(key, {"error": f"no canned data for {name}{args}"})


# --------------------------------------------------------------------------- #
# Tool-RAG mode — mirrors the LIVE path (LlmRequestPolicy.h): the model is sent
# only Tier-0 meta-tools (tool_list / tool_describe) and must DISCOVER the data
# tools, which are then injected into the active set (note_tool_activations).
# This is the link the direct mode skips — and the one the lean prompt's rule #2
# ("call tool_list to find more") actually depends on.
# --------------------------------------------------------------------------- #

META_TOOLS = [
    {"type": "function", "function": {
        "name": "tool_list",
        "description": "Search for tools by a natural-language description of what you need. "
                       "Returns the most relevant tool names.",
        "parameters": {"type": "object",
                       "properties": {"query": {"type": "string"}},
                       "required": ["query"]}}},
    {"type": "function", "function": {
        "name": "tool_describe",
        "description": "Get the full input schema for a tool by name before calling it.",
        "parameters": {"type": "object",
                       "properties": {"name": {"type": "string"}},
                       "required": ["name"]}}},
]

# The catalog tool_list searches over (keyed by name → its structured definition).
CATALOG = {t["function"]["name"]: t for t in TOOLS}


def tool_list_result(query: str) -> dict:
    q = (query or "").lower()
    hits = [name for name, t in CATALOG.items()
            if any(w in (name + " " + t["function"]["description"]).lower()
                   for w in q.split() if len(w) > 2)]
    return {"tools": [{"name": n, "description": CATALOG[n]["function"]["description"]}
                      for n in (hits or list(CATALOG))[:5]]}


def tool_describe_result(name: str) -> dict:
    t = CATALOG.get(name)
    return t["function"] if t else {"error": f"unknown tool {name}"}


# --------------------------------------------------------------------------- #
# Bare-JSON tool-call detection — a faithful Python port of the C++
# TextToolCallParser, so the harness reports the same shapes the app salvages.
# --------------------------------------------------------------------------- #

_FENCE = re.compile(r"```(?:json|tool_call)?\s*\n?([\[{][\s\S]*?[\]}])\s*```", re.MULTILINE)


def _name_of(o: dict) -> str:
    if isinstance(o.get("name"), str):
        return o["name"]
    fn = o.get("function")
    if isinstance(fn, str):
        return fn
    if isinstance(fn, dict) and isinstance(fn.get("name"), str):
        return fn["name"]
    return ""


def _args_of(o: dict) -> dict:
    for src in (o, o.get("function") if isinstance(o.get("function"), dict) else {}):
        for k in ("arguments", "parameters", "input"):
            v = src.get(k)
            if isinstance(v, dict):
                return v
            if isinstance(v, str):
                try:
                    d = json.loads(v)
                    if isinstance(d, dict):
                        return d
                except json.JSONDecodeError:
                    pass
    return {}


def parse_bare_json_tool_calls(content: str, known: set[str]) -> list[tuple[str, dict]]:
    """Mirror of the C++ salvage: find bare-JSON tool calls, guarded to known names."""
    if not known:
        return []
    candidates: list[str] = []
    trimmed = content.strip()
    if trimmed[:1] in "{[":
        candidates.append(trimmed)
    candidates += [m.group(1).strip() for m in _FENCE.finditer(content)]

    def collect(v, out):
        if isinstance(v, list):
            for e in v:
                collect(e, out)
        elif isinstance(v, str):  # a stringified JSON tool-call object/array
            try:
                collect(json.loads(v), out)
            except json.JSONDecodeError:
                pass
        elif isinstance(v, dict):
            nm = _name_of(v)
            if nm and nm in known:
                out.append((nm, _args_of(v)))

    for cand in candidates:
        out: list[tuple[str, dict]] = []
        try:
            collect(json.loads(cand), out)
        except json.JSONDecodeError:
            continue
        if out:
            return out
    return []


# --------------------------------------------------------------------------- #
# The loop — mirrors LlmService::do_tool_loop.
# --------------------------------------------------------------------------- #

def post(host: str, model: str, messages: list, tools: list | None) -> dict:
    body = {"model": model, "stream": False, "messages": messages, "options": {"temperature": 0.2}}
    if tools:
        body["tools"] = tools
    req = urllib.request.Request(host.rstrip("/") + "/v1/chat/completions",
                                 data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=600) as r:
        return json.load(r)


def _dispatch(fn: str, args: dict, active: dict) -> dict:
    """Execute a tool call. tool_list/tool_describe grow `active` (RAG activation,
    mirroring note_tool_activations); everything else returns canned data."""
    if fn == "tool_list":
        res = tool_list_result(args.get("query", ""))
        for row in res["tools"]:                       # activate surfaced candidates
            active[row["name"]] = CATALOG[row["name"]]
        return res
    if fn == "tool_describe":
        name = args.get("name", "")
        if name in CATALOG:
            active[name] = CATALOG[name]               # activate the committed tool
        return tool_describe_result(name)
    return canned_result(fn, args)


def run_variant(host: str, model: str, prompt_name: str, max_rounds: int, rag: bool) -> bool:
    known = {t["function"]["name"] for t in TOOLS} | {"tool_list", "tool_describe"}
    messages = [{"role": "system", "content": PROMPTS[prompt_name]},
                {"role": "user", "content": TASK}]
    fired: set[tuple[str, dict]] = set()
    # In RAG mode the model starts with ONLY the meta-tools and must discover the
    # rest; active grows as it does. In direct mode all data tools are present.
    active: dict = {} if rag else dict(CATALOG)
    mode = "RAG-discovery" if rag else "direct"
    print(f"\n{'='*70}\n### PROMPT={prompt_name!r}  MODE={mode}  MODEL={model}\n{'='*70}")

    for rnd in range(1, max_rounds + 1):
        tools = (META_TOOLS + list(active.values())) if rag else list(CATALOG.values())
        try:
            resp = post(host, model, messages, tools)
        except Exception as e:  # noqa: BLE001 — diagnostic tool, surface anything
            print(f"  [turn {rnd}] HTTP ERROR: {e}")
            return False
        msg = resp["choices"][0]["message"]
        tcs = msg.get("tool_calls") or []
        content = msg.get("content") or ""

        if tcs:
            messages.append(msg)
            names = []
            for tc in tcs:
                fn = tc["function"]["name"]
                try:
                    args = json.loads(tc["function"].get("arguments") or "{}")
                except json.JSONDecodeError:
                    args = {}
                names.append(f"{fn}({args})")
                if fn in CATALOG:
                    fired.add((fn, str(args.get("symbol") or args.get("ticker") or "").upper()))
                messages.append({"role": "tool", "tool_call_id": tc.get("id", ""),
                                 "content": json.dumps(_dispatch(fn, args, active))})
            print(f"  [turn {rnd}] STRUCTURED tool_calls: {', '.join(names)}")
            continue

        # No structured call. Does the prose hide a bare-JSON tool call?
        # NOTE: the C++ app salvages these too, but via a one-shot "execute + summarize
        # without tools" path (try_extract_and_execute_text_tool_calls) — it does NOT
        # continue the multi-turn loop. This harness feeds the result back and continues,
        # so a "COMPLETED via salvage" here means the calls are recoverable, not that the
        # app would necessarily finish a long multi-tool chain off the degraded path. The
        # clean path (and the app default for local models) is the lean prompt above.
        salvaged = parse_bare_json_tool_calls(content, known)
        if salvaged:
            print(f"  [turn {rnd}] !! DEGRADED to bare-JSON text (salvageable): "
                  f"{[(n, a) for n, a in salvaged]}")
            messages.append({"role": "assistant", "content": content})
            for fn, args in salvaged:
                if fn in CATALOG:
                    fired.add((fn, str(args.get("symbol") or args.get("ticker") or "").upper()))
                messages.append({"role": "tool", "tool_call_id": "",
                                 "content": json.dumps(_dispatch(fn, args, active))})
            continue

        # Genuine final answer (or a stall).
        print(f"  [turn {rnd}] FINAL TEXT ({len(content)} chars):\n      "
              + content.strip().replace("\n", "\n      ")[:600])
        completed = REQUIRED_CALLS.issubset(fired)
        missing = REQUIRED_CALLS - fired
        print(f"\n  RESULT: required tools fired = {sorted(fired)}")
        if completed and content.strip():
            print("  ✅ COMPLETED: all required tools used and a final answer produced.")
            return True
        print(f"  ❌ INCOMPLETE: missing {sorted(missing)} or empty answer "
              f"(stopped after {rnd} turn(s)).")
        return False

    print(f"  ❌ HIT MAX ROUNDS ({max_rounds}) without a final answer. fired={sorted(fired)}")
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", default="llama3.3:latest", help="Ollama model tag")
    ap.add_argument("--host", default="http://localhost:11434", help="Ollama base URL")
    ap.add_argument("--prompt", choices=["full", "lean", "both"], default="both")
    ap.add_argument("--rag", choices=["off", "on", "both"], default="both",
                    help="off=all tools pre-loaded; on=Tier-0 meta-tools + tool_list "
                         "discovery (mirrors the live app); both=run each")
    ap.add_argument("--max-rounds", type=int, default=12)
    args = ap.parse_args()

    prompts = ["full", "lean"] if args.prompt == "both" else [args.prompt]
    rags = [False, True] if args.rag == "both" else [args.rag == "on"]

    results: dict[str, bool] = {}
    for rag in rags:
        for p in prompts:
            label = f"{p}/{'RAG' if rag else 'direct'}"
            results[label] = run_variant(args.host, args.model, p, args.max_rounds, rag)

    print(f"\n{'='*70}\nSUMMARY ({args.model})")
    for label, ok in results.items():
        print(f"  {label:>14}: {'COMPLETED ✅' if ok else 'INCOMPLETE ❌'}")
    return 0 if any(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
