# Reliable tools + web browsing for local (Ollama) models — design

**Date:** 2026-06-17
**Status:** approved (brainstorm), pre-implementation

## Problem

Local models (e.g. `llama3.3:latest` via Ollama) hallucinate live data instead of
calling the app's tools. Observed: asked for AAPL, the model replied "$182.01 …
iPhone 15" — stale training-data, not a tool result.

Root cause (verified in code): **Tool-RAG is on by default** (`mcp/use_tool_rag`,
`LlmOpenMarketTerminalAsync.cpp:53`). With RAG on, the model is handed only the
Tier-0 tools (`tool_list`, `tool_describe`, nav) and must first call
`tool_list("price of AAPL")` to *discover* `get_quote`, then call it — a two-step
dance. Capable models do it; weak local models skip it and answer from memory.
The app already HAS the live-data tools (`get_quote`, `get_equity_quote`,
`get_equity_news`, `search_news`, `get_candles`, EDGAR, …) — they're just not
reachable in one step for a weak model.

## Goals

1. Local/Ollama models reliably **call** the live-data tools they already have
   (one-step), and stop answering current facts from memory.
2. Add **web browsing** so the model can also get *general* (non-finance) current
   info: `web_search` + `web_fetch`, powered by the **bundled QtWebEngine**
   (headless/offscreen Chromium) querying **DuckDuckGo**.

Non-goals: changing premium-Claude behaviour; a settings UI redesign; Google/Bing
scraping (DuckDuckGo only, Bing fallback is a future follow-up); headless-CLI web
tools (web tools are GUI-tier by necessity).

## Part 1 — Local-model tool reliability

### 1a. Curated "local essentials" toolset, Tool-RAG bypassed for local models

When the active provider is local — `is_local_model` (already computed in
`LlmService.cpp:148`: provider == "ollama" or base URL contains
`localhost`/`127.0.0.1`) — attach a fixed, curated set of tools **directly** to the
request instead of the RAG Tier-0-only set, so no discovery step is needed.

Define a single source of truth:

```
// mcp::McpProvider (or McpService) — new
const QSet<QString>& local_essentials_tool_names();
```

Initial set (~18, read-only/low-risk, high-utility):
`get_quote, get_equity_quote, get_equity_news, search_news, get_candles,
get_equity_info, get_equity_peers, get_equity_technicals, edgar_get_financials,
edgar_search_filings, get_top_news, get_news, get_observations, lookup_symbol,
search_equity_symbols, web_search, web_fetch, tool_list`.

(`tool_list` stays so the model can still reach the other ~600 tools when a request
falls outside the essentials.)

**Wiring:** in the request-building path, when `is_local_model`, build the tool
list / text catalog from `local_essentials_tool_names()` (RAG off for this case)
rather than the Tier-0 RAG set. This applies to BOTH the structured-`tools` path
(Ollama OpenAI-compatible at `localhost:11434/v1/chat/completions`) and the text
catalog (`build_tool_catalog_for_prompt`). `mcp/use_tool_rag` is unchanged for
non-local providers; no new user setting.

### 1b. Anti-hallucination rule in the lean local prompt

Append to the lean local system prompt (`LlmService.cpp:158-170`):

> "NEVER state a price, quote, market level, holding, or recent news/event from
> memory or training data — that data is stale. For ANYTHING current you MUST call
> a tool (e.g. `get_quote`, `get_equity_news`, `web_search`) and answer only from
> the tool result. If unsure which tool, call `tool_list`."

Keep it short (small models degrade with long prompts — the existing lean-prompt
rationale at `LlmService.cpp:144-147`).

## Part 2 — Web tools (headless QtWebEngine → DuckDuckGo)

### Tools (GUI-tier, read-only / Public auth)

- **`web_search(query: string, max_results?: int = 5)`** → returns
  `{ "results": [ {"title","url","snippet"}, … ] }`. Loads
  `https://html.duckduckgo.com/html/?q=<urlencoded query>` offscreen and extracts
  result rows.
- **`web_fetch(url: string, max_chars?: int = 6000)`** → returns
  `{ "title", "url", "text" }` where `text` is readable page text (Chromium renders
  JS first), truncated to `max_chars` with an ellipsis.

Both registered in `register_gui_tools()` (they require QtWebEngine + a GUI event
loop; not available in the headless CLI — acceptable).

### Component: `HeadlessBrowser` (GUI/main-thread singleton)

- Owns one reusable offscreen `QWebEnginePage` (no visible widget). Lives on the
  GUI thread.
- `QString load_and_extract(const QUrl&, const QString& extraction_js, int timeout_ms)`
  conceptually: `page->setUrl()`, await `loadFinished(ok)`, a short settle
  (~400 ms for late JS), then `page->runJavaScript(extraction_js, cb)`; return the
  JS result (a JSON string). Hard timeout (default **15 s**) → return an error
  marker. **Serialized**: one in-flight load at a time (queue or reject concurrent
  calls) to keep the single page safe.
- JS/modal dialogs suppressed; a normal desktop User-Agent set; images/autoplay
  disabled where possible for speed.

### Extraction

- **Search:** injected JS walks `html.duckduckgo.com/html` result nodes
  (`.result` → `.result__a` title+href, `.result__snippet`), returns a JSON array.
  DuckDuckGo's redirect hrefs (`/l/?uddg=`) are decoded to the real URL.
- **Fetch:** injected JS returns `{title: document.title, text:
  document.body.innerText}` after stripping `script/style/nav/header/footer` noise
  (lightweight readability; full Readability.js is a future option).

### Threading (key constraint)

MCP tool handlers run on a worker thread (the bridge/agent off-thread dispatch).
QtWebEngine MUST run on the GUI/main thread. So each web tool handler:

1. Packages the request and calls `QMetaObject::invokeMethod(headless_browser,
   …, Qt::QueuedConnection)` to run the load on the main thread.
2. Blocks the worker on a `QEventLoop` / `std::promise` with the same hard timeout,
   so a hung load returns an error rather than wedging the worker pool. (Mirrors the
   existing get_quote main↔worker marshaling pattern.)

### Parser isolation (for testability)

The DuckDuckGo result parsing (HTML/JSON → `[{title,url,snippet}]`) lives in a pure
function `parse_ddg_results(html_or_json)` separate from the QtWebEngine plumbing,
so it can be unit-tested against a saved HTML fixture with no browser.

## Settings / safety

- No new user toggle. Local models always get the essentials set.
- Web tools: **Public** auth tier, read-only, non-destructive. No API key, no cost;
  network egress only when the model calls them.
- `web_fetch` only fetches the URL it's given; no credential injection, no local
  file access.

## Testing

- **Unit (no browser):** `tst_web_search_parser` — feed a saved
  `tests/fixtures/ddg_results.html` to `parse_ddg_results`, assert ≥3 rows with
  non-empty title/url/snippet and decoded (non-`/l/?uddg=`) URLs. Neuter-verify.
- **Unit:** `local_essentials_tool_names()` contains `get_quote`, `web_search`,
  `web_fetch`, `tool_list`; the local request builder selects from it (not Tier-0).
- **Live (manual):** with Ollama llama3.3 — "what's AAPL trading at?" → a
  `get_quote` call (no $182 hallucination); "what's the latest on <non-finance
  topic>?" → a `web_search` call; `web_fetch` of a known URL returns readable text.
- **Regression:** full ctest stays green; premium-Claude path unchanged (the
  essentials/anti-hallucination changes are gated on `is_local_model`).

## Risks & mitigations

- **QtWebEngine threading / hangs** — main-thread marshal + 15 s hard timeout +
  serialized loads; a hung load returns an error, never wedges the worker.
- **DuckDuckGo markup drift** — parser isolated + fixture-tested; Bing fallback is a
  scoped follow-up, not in this change.
- **Small-model prompt sensitivity** — keep the anti-hallucination addition short;
  re-check with `scripts/dev/probe_local_model_toolloop.py`.
- **Essentials set too big/small** — start at ~18; it's a single list, easy to tune.

## Files (anticipated)

- `src/services/llm/LlmService.cpp` — local-essentials tool selection + prompt rule.
- `src/services/llm/LlmOpenMarketTerminalAsync.cpp` — local branch in
  `build_tool_catalog_for_prompt`.
- `src/mcp/` — `local_essentials_tool_names()`; register `web_search`/`web_fetch` in
  `register_gui_tools` (McpInitGui).
- `src/web/HeadlessBrowser.{h,cpp}` (new) — offscreen QtWebEngine + extraction.
- `src/web/WebSearchParser.{h,cpp}` (new, pure) — `parse_ddg_results`.
- `tests/tst_web_search_parser.cpp` + `tests/fixtures/ddg_results.html` (new);
  `tests/CMakeLists.txt`, `CMakeLists.txt` wiring.
