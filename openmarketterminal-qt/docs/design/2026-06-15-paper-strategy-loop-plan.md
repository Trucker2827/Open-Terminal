# Paper Strategy-Loop Driver — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`). The loop must ONLY drive the gated substrate (`prepare_order`/`submit_order paper`) — never a new execution path. Paper-only in v1.

**Goal:** `openterminalcli ai run strategy <name> --mode paper` — a continuous loop that ticks, reads market state, asks a pluggable Strategy for intents, and runs each through `prepare_order` → `submit_order(paper)`. Ships a deterministic reference strategy + a Claude/LLM strategy. Inherits the whole constitution (risk floor, kill switch, audit) by riding the substrate.

**Architecture:** New `src/services/ai_strategy/` (Strategy interface, MarketSnapshot, TradeIntent, ToolCaller seam, StrategyRunner loop, the two strategies) + a CLI `ai` group. The loop calls tools via the same routing the CLI uses (in-process `HeadlessRuntime::call_tool` or attached `BridgeClient`) — single profile owner, no bypass.

**Tech Stack:** C++20, Qt6, CMake+Ninja, QtTest.

**Spec:** `docs/design/2026-06-15-paper-strategy-loop-design.md`.
**Repo:** `~/src/Open-Terminal/openmarketterminal-qt` · test build `/tmp/ot-build-test` · GUI build `/tmp/ot-build-ht` (CLI `/tmp/ot-build-ht/openterminalcli`) · **Branch:** create `feat/paper-strategy-loop` off `main` (`4b19083`) before Task 1.

---

## Verified facts (grounded)
- **ToolResult** (`src/mcp/McpTypes.h`): `struct ToolResult { bool success; QJsonValue data; QString message; QString error; }`. `prepare_order`/`submit_order` return `ToolResult::ok_data(QJsonObject{...})` → read `res.data.toObject()`; prepare → `{status:"prepared"|"rejected", draft_id, reason?, ...}`; submit → `{status:"filled"|"rejected", reason?, ...}`.
- **Tool calling:** the CLI routes via `exec_tool` (`src/cli/CommandDispatch.cpp:~106`): `opts.headless ? headless_runtime().call_tool(tool,args) : client->call_tool(tool,args)`. `HeadlessRuntime::call_tool(const QString&, const QJsonObject&) -> mcp::ToolResult`. `BridgeClient::call_tool(name,args) -> ClientResult` (attached). The loop's `ToolCaller` wraps whichever transport the run chose.
- **Read tools:** `get_quote` ({symbol}→ price), `pt_get_portfolio` ({portfolio_id}), `pm_paper_portfolio` ({}), `pm_get_order_book` ({venue,asset_id}). Snapshot uses `get_quote` per symbol (+ portfolio best-effort).
- **LLM:** `openmarketterminal::ai_chat::LlmService` (`src/services/llm/LlmService.h`) — **synchronous** `LlmResponse chat(const QString& user_message, const std::vector<ConversationMessage>& history, ...)`. Active provider/key via `LlmConfigRepository::get_active_provider()` (`get_active_provider().is_err()` ⇒ none configured). READ `LlmResponse` for the text/content field + `ConversationMessage`.
- **Kill switch:** `mcp::cli_kill_switch_engaged()` (`SettingsGate.h`) — cheap settings read; the loop checks it each tick AND the substrate rejects with "kill switch engaged".
- **CLI dispatch:** `src/cli/CommandDispatch.cpp::dispatch(QStringList args)` with `if (group == "...")` blocks (version/status/serve/mcp/quote/hub). Add an `ai` group. `GlobalOpts` carries `--profile`, `--headless`, `--json`.
- **Migrations/storage:** none needed — the loop persists nothing new; results live in the substrate's `order_drafts`/`trade_audit` + the paper portfolio.

---

## File Structure
- Create: `src/services/ai_strategy/Strategy.h` (interface + MarketSnapshot + TradeIntent), `StrategyRunner.{h,cpp}`, `ToolCaller.{h,cpp}` (interface + real impl), `MeanReversionStrategy.{h,cpp}`, `LlmStrategy.{h,cpp}`; `src/cli/AiRunCommand.{h,cpp}`.
- Modify: `src/cli/CommandDispatch.cpp` (the `ai` group), `CMakeLists.txt` (core lib sources + CLI), `tests/CMakeLists.txt`.
- Tests: `tests/tst_strategy_loop.cpp`.

---

## Task 1: Strategy framework + StrategyRunner loop (harness)

**Files:** Create `src/services/ai_strategy/Strategy.h`, `ToolCaller.{h,cpp}`, `StrategyRunner.{h,cpp}`; Modify `CMakeLists.txt`; Test `tests/tst_strategy_loop.cpp`.

- [ ] **Step 1: Types + seams** (`Strategy.h`, ns `openmarketterminal::ai_strategy`):
  - `struct MarketSnapshot { qint64 ts=0; QMap<QString,double> quotes; QJsonObject portfolio; };`
  - `using TradeIntent = QJsonObject;` (the prepare_order args).
  - `class ToolCaller { public: virtual ~ToolCaller()=default; virtual mcp::ToolResult call(const QString& name, const QJsonObject& args) = 0; };`
  - `class Strategy { public: virtual ~Strategy()=default; virtual QString name() const = 0; virtual QStringList universe() const = 0; virtual QVector<TradeIntent> propose(const MarketSnapshot&) = 0; virtual void on_fill(const TradeIntent&, const QJsonObject& submit_result) {} };` (on_fill default no-op; the deterministic strategy uses it to track its own holdings).
- [ ] **Step 2: StrategyRunner** (`StrategyRunner.{h,cpp}`): `struct RunConfig { int interval_sec=15; int max_iters=0/*0=unbounded*/; int duration_sec=0; };` `struct RunSummary { int ticks, proposed, prepared, filled, rejected, errors; bool halted_by_kill_switch; };`
  - `RunSummary run(Strategy& s, ToolCaller& tc, const RunConfig& cfg, std::function<bool()> stop_requested);`
  - Loop per the spec: each iter — if `mcp::cli_kill_switch_engaged()` → log "halted by kill switch" + set `halted_by_kill_switch` + break; build `MarketSnapshot` (for each sym in `s.universe()`: `tc.call("get_quote", {{"symbol",sym}})` → parse price; best-effort portfolio via `pm_paper_portfolio`/`pt_get_portfolio` — skip on failure); `auto intents = s.propose(snap)`; for each: `auto prep = tc.call("prepare_order", intent)`; if `prep.success && prep.data.toObject()["status"]=="prepared"` → `auto sub = tc.call("submit_order", {{"draft_id", draft_id},{"mode","paper"}})`; if the submit/prepare reason contains "kill switch engaged" or "paper trading disabled" → log + halt; `s.on_fill(intent, sub.data.toObject())`; tally; log each decision via a `log()` callback (so the CLI prints + tests capture). Sleep `interval_sec` between ticks (skippable when `interval_sec<=0` for tests); stop on max_iters/duration/`stop_requested()`.
  - Add a `std::function<void(const QString&)> on_log` member (default prints to stdout) so tests capture lines without sleeping.
  Add the sources to `CMakeLists.txt` (core lib — so both the CLI and tests link them).
- [ ] **Step 3: Failing test** `tests/tst_strategy_loop.cpp` (link `openterminal_core Qt6::Core Qt6::Test`; no DB/runtime needed for the harness test — use fakes; register in tests/CMakeLists.txt). Define a `FakeToolCaller` (a scripted `QQueue<ToolResult>` keyed by tool name, records calls) + a `FakeStrategy` (returns a fixed intent list, records on_fill). Assert with `interval_sec=0`:
  - a `prepared` prepare → the runner THEN calls `submit_order` with that draft_id + mode "paper"; a `rejected` prepare → NO submit; tallies correct.
  - a submit result with reason "kill switch engaged" → loop halts (`halted_by_kill_switch` or stops) and makes no further calls.
  - `max_iters=3` → exactly 3 ticks.
  - `on_fill` is invoked with the submit result for filled intents.
  RED → implement → GREEN. Full suite green.
- [ ] **Step 4: Commit** `feat(ai): strategy-loop harness (StrategyRunner + ToolCaller/Strategy seams)` + Co-Authored-By.

---

## Task 2: Deterministic reference strategy (MeanReversion)

**Files:** Create `src/services/ai_strategy/MeanReversionStrategy.{h,cpp}`; Modify `CMakeLists.txt`; Test extend `tests/tst_strategy_loop.cpp`.

- [ ] **Step 1: MeanReversionStrategy** (implements `Strategy`): ctor `(QStringList symbols, int window=5, double band=0.01, double qty=10)`. Keeps `QMap<QString, QList<double>>` rolling prices + a `QSet<QString> holding_` (updated in `on_fill`: a filled BUY → insert; a filled SELL → remove). `universe()` → symbols. `propose(snap)`: for each symbol with a quote, push the price into the window (cap to `window`); once the window is full, `mean = avg`; if `price < mean*(1-band)` and NOT holding → emit a BUY `{symbol, side:"buy", quantity:qty, order_type:"limit", limit_price: price}`; if `price > mean*(1+band)` and holding → emit a SELL `{symbol, side:"sell", quantity:qty, order_type:"limit", limit_price: price}`; else nothing. (Limit orders → deterministic resolved price, no market-data dependency in the floor.) `on_fill`: parse the submit result `status=="filled"` + the intent side → update `holding_`.
- [ ] **Step 2: Test** — extend `tst_strategy_loop.cpp`: feed the strategy a deterministic price sequence directly (call `propose` with hand-built snapshots): in-band prices → no intent; a dip below `mean*(1-band)` → a BUY intent (correct fields); after an `on_fill` BUY, a rip above `mean*(1+band)` → a SELL intent; without holding, a rip → no SELL. RED → implement → GREEN.
- [ ] **Step 3: Commit** `feat(ai): deterministic mean-reversion reference strategy` + Co-Authored-By.

---

## Task 3: Claude/LLM strategy

**Files:** Create `src/services/ai_strategy/LlmStrategy.{h,cpp}`; Modify `CMakeLists.txt`; Test extend `tests/tst_strategy_loop.cpp`.

- [ ] **Step 1: LlmStrategy** (implements `Strategy`): ctor `(QStringList symbols, CompletionFn complete)` where `using CompletionFn = std::function<QString(const QString& prompt)>;` — **a seam**: the real CLI passes a lambda that calls `ai_chat::LlmService::...chat(prompt, {})` and extracts the text; tests pass a fake returning canned JSON. (Default ctor wires the real LlmService; grep `LlmService.h` + `LlmContentExtractors.h` for the exact chat call + how to pull the assistant text from `LlmResponse`.) `propose(snap)`: build a prompt = a system instruction ("You are a paper-trading assistant. Given the market snapshot + portfolio, respond ONLY with a JSON array of order intents, each `{symbol, side:buy|sell, quantity, order_type, limit_price?}`, or `[]` to do nothing. Trade only within the universe. The system enforces risk limits; oversized/invalid intents are rejected.") + the snapshot/portfolio/universe as JSON. Call `complete(prompt)`; extract the first JSON array from the reply (tolerate prose around it — find the `[`…`]`); parse to `QVector<TradeIntent>`; on parse failure or empty → return `{}`. NEVER throws. If the active provider is unconfigured, the real CompletionFn returns an empty string → zero intents (log a one-time "LLM strategy: no provider configured").
- [ ] **Step 2: Test** — extend `tst_strategy_loop.cpp` with a fake CompletionFn: returns `[{"symbol":"AAPL","side":"buy","quantity":5,"order_type":"limit","limit_price":200}]` → `propose` yields one correct intent; returns prose+array (`Sure! [ {...} ]`) → still parses the array; returns malformed (`not json`) → zero intents, no throw; returns `[]` → zero intents. RED → implement → GREEN.
- [ ] **Step 3: Commit** `feat(ai): Claude/LLM-driven strategy (untrusted intents → gated substrate)` + Co-Authored-By.

---

## Task 4: CLI `ai run strategy` + e2e + regression

**Files:** Create `src/cli/AiRunCommand.{h,cpp}`; Modify `src/cli/CommandDispatch.cpp`, `CMakeLists.txt`; Test `tests/e2e_strategy_loop.sh`.

- [ ] **Step 1: AiRunCommand** — parse `ai run strategy <name> --mode paper [--interval-sec N=15] [--max-iters M] [--duration-sec D] [--symbols A,B,C]`. Refuse `--mode live` ("live strategy loop not supported (paper-first)", nonzero exit). Build the real `ToolCaller` (wrap the chosen transport: `--headless` → a caller over `headless_runtime().call_tool`; else attach via `BridgeClient` like `exec_tool` does — reuse that routing). Instantiate the strategy by name: `meanrev` → `MeanReversionStrategy(symbols)`, `claude` → `LlmStrategy(symbols, real LlmService CompletionFn)` (default symbols if `--symbols` absent, e.g. {"AAPL"}). Install a SIGINT handler (self-pipe or `std::signal` setting an atomic flag) → `stop_requested` returns true on Ctrl-C for a clean stop. Run `StrategyRunner::run(...)`; print each decision + the final `RunSummary`; exit 0 on a clean run.
- [ ] **Step 2: Wire the `ai` group** in `CommandDispatch.cpp::dispatch` (`if (group == "ai")` → require `args[1]=="run" && args[2]=="strategy"` → `AiRunCommand::run(...)`; else usage). Add `AiRunCommand.cpp` to `CMakeLists.txt` (CLI target). Update the CLI usage string to list `ai run strategy …`.
- [ ] **Step 3: e2e** `tests/e2e_strategy_loop.sh` (mirror `e2e_paper_trade.sh`: throwaway profile, cleanup trap, watchdog, leak-safe). NO GUI. Bootstrap the profile DB; `sqlite3` set `cli.allow_paper_trading=true`. Run `openterminalcli --headless --profile P ai run strategy meanrev --mode paper --interval-sec 0 --max-iters 3 --symbols AAPL` under a `timeout`; assert it exits 0 and prints a summary; then `sqlite3` count `trade_audit` rows > 0 (the loop drove prepare/submit through the substrate). **Kill-switch e2e:** set `cli.kill_switch=true`, run again with `--max-iters 5`, assert it halts (summary shows halted / 0 fills) and prints "kill switch". (Quotes may be unavailable headless → the strategy may emit nothing; that's fine — assert the loop RAN + audit/halt behavior, not specific fills. If `get_quote` returns nothing, note it; the harness + kill-switch + paper-gate are the hard asserts.)
- [ ] **Step 4: Run e2e + full regression (paste):** the e2e output; `ctest --test-dir /tmp/ot-build-test --output-on-failure` (incl. new `tst_strategy_loop`, all prior green); `cmake --build /tmp/ot-build-ht --target OpenMarketTerminal openterminalcli`; `--selftest-tools` exit 0; `--headless mcp list` works. Confirm the substrate/daemon e2e (`e2e_paper_trade.sh`, `e2e_pm_paper_trade.sh`) still rc 0.
- [ ] **Step 5: Commit** `feat(ai): ai run strategy CLI (paper loop) + e2e` + Co-Authored-By.

---

## Self-Review
**Spec coverage:** loop harness riding the gated substrate (T1) ✓; ToolCaller attach-or-headless (T1/T4) ✓; Strategy seam (T1) ✓; deterministic reference (T2) ✓; Claude/LLM brain with a fake-able seam (T3) ✓; CLI `ai run strategy --mode paper` + paper-only guard + SIGINT/bounds (T4) ✓; kill-switch halt (T1 detect + T4 e2e) ✓; audit fills via the substrate (T4 e2e) ✓; no new execution path / no bypass (loop calls prepare/submit only) ✓.

**Type consistency:** `ToolCaller`/`Strategy`/`MarketSnapshot`/`TradeIntent` (T1) used by both strategies (T2/T3) + the CLI (T4); `StrategyRunner::run` signature stable; `CompletionFn` seam (T3) wired real in T4.

**Placeholder scan:** code-shaped steps carry real signatures + tool names + result shapes; the few "grep the exact LlmResponse text field / BridgeClient routing" notes are grounded with a stated fallback.

**SAFETY NOTE:** the loop is paper-only and drives ONLY `prepare_order` + `submit_order(mode:"paper")` + read tools — it adds NO execution path and changes NO gate. LLM-proposed intents are untrusted and pass the same risk floor. The kill switch halts the loop. Do NOT add `--mode live` execution in v1.
