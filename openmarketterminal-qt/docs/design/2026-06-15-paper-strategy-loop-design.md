# Paper Strategy-Loop Driver — Design Spec

**Status:** Draft design — pending user spec review. Precedes the plan.
**Date:** 2026-06-15
**Builds on:** the shipped AI-trading substrate (`prepare_order`/`submit_order`, the deterministic risk floor, the GUI-only constitution incl. the kill switch, `trade_audit`), the headless/daemon tool path (`exec_tool` → in-process `HeadlessRuntime::call_tool` OR attached `BridgeClient`), and the LLM stack (`services/llm/LlmService`, `LlmConfigRepository`).

## Goal

A continuous **paper** strategy-loop driver — `openterminalcli ai run strategy <name> --mode paper` — that ticks on an interval, reads market state, asks a pluggable **Strategy** for trade intents, and runs each intent through the **gated substrate** (`prepare_order` → `submit_order paper`). The loop is a thin orchestrator: it NEVER bypasses the substrate, so the risk floor, the paper-trading toggle, the kill switch, and the full audit trail all apply unchanged. Ship two brains: a deterministic reference strategy (provable + testable, watch the audit fill) and a **Claude/LLM-driven** strategy (the LLM proposes; the daemon disposes).

**User decision:** build the harness + the Strategy seam + BOTH brains.

## The governing principle (unchanged)
The loop, like any client, can only **propose** (via `prepare_order`) and **transmit** (via `submit_order`). The daemon remains the authority: every intent — coded or LLM-generated — passes the deterministic risk floor and the constitution. An LLM hallucinating a 1M-share order is just *rejected at prepare*. The loop adds automation, not authority.

## Architecture

### Components (new: `src/services/ai_strategy/`)
- **`MarketSnapshot`** — the per-tick view a strategy reasons over: `{ ts, quotes: {sym→price}, portfolio: <json>, (optional) order_books }`. Gathered via the substrate read tools.
- **`TradeIntent`** — a thin wrapper over the `prepare_order` args JSON (equity or `asset_class:"prediction"`).
- **`ToolCaller`** — an abstraction with `ToolResult call(name, args)`; the real impl reuses the CLI's attach-or-headless routing (talk to the single profile owner — the running daemon/GUI if present, else in-process). A fake impl backs the tests.
- **`Strategy`** (interface): `QString name()`, `QStringList universe()` (symbols/markets to fetch), `QVector<QJsonObject> propose(const MarketSnapshot&)` (0..N intents). Pure-ish: a function of the snapshot + the strategy's own accumulated state.
- **`StrategyRunner`** — the loop harness (strategy-agnostic):
  ```
  for iter in 0..max_iters (or until SIGINT / duration):
    if kill_switch_engaged (cheap settings read) → log "halted" + STOP
    snapshot = gather(strategy.universe())          # via ToolCaller read tools
    for intent in strategy.propose(snapshot):
      prep = call("prepare_order", intent)
      if prep.status == "prepared":
        sub = call("submit_order", {draft_id, mode:"paper"})
        log(intent, prep, sub)                      # filled / rejected
      else:
        log(intent, prep)                           # rejected at prepare (risk/validation)
    sleep(interval)
  print summary (ticks, proposed, filled, rejected, final portfolio)
  ```
  It detects a `kill switch engaged` / `paper trading disabled` rejection and stops cleanly (no spin). It is bounded by `--max-iters` and/or `--duration-sec`, and SIGINT triggers a clean stop + summary.

### The two strategies
- **`MeanReversionStrategy` (deterministic reference):** keeps a rolling window of the last K observed prices per symbol; when `price < mean*(1-band)` and flat → a small BUY intent; when `price > mean*(1+band)` and holding → a SELL-to-close intent. Small fixed qty, within the default caps. Deterministic given the tick sequence → unit-testable. Its only job is to *exercise the loop + audit end-to-end*, not to have edge.
- **`LlmStrategy` (Claude/LLM-driven):** on each tick, builds a prompt from the snapshot + portfolio + the allowed universe + the `prepare_order` JSON schema + the configured caps, and asks the active LLM provider (`LlmConfigRepository::get_active_provider` → `LlmService`) for a **JSON array of intents** (or empty). Parses + returns them as intents. Untrusted: every returned intent still goes through `prepare_order`'s risk floor. Graceful failure: no provider configured / malformed LLM output → log + return no intents (the loop continues). The LLM never sees credentials or the `cli.*` controls; it only proposes intents.

### CLI surface (new `ai` group in `CommandDispatch`)
`openterminalcli [--profile P] ai run strategy <name> --mode paper [--interval-sec N=15] [--max-iters M] [--duration-sec D] [--symbols A,B,C]`
- `<name>` selects the strategy: `meanrev` (deterministic) | `claude` (LLM). `--symbols` overrides the universe.
- **`--mode` is paper-only in v1** — `--mode live` is refused with "live strategy loop not supported (paper-first)". (Live would be gated by the substrate anyway, but the loop won't drive it.)
- Stop: `--max-iters` / `--duration-sec` / SIGINT (clean). The kill switch halts it mid-run.
- Prints each decision (intent → prepare verdict → submit result) and a final summary; everything is also in `trade_audit`.

### Safety / non-bypass (the load-bearing properties)
- The loop calls ONLY `prepare_order`, `submit_order(paper)`, and read tools — through the same `call_tool` path everything else uses. No new execution path, no carve-out change.
- The kill switch halts the loop (both: the substrate rejects, AND the loop detects + stops).
- Caps/validation rejections are normal, logged, non-fatal.
- Paper-only; the loop cannot place live orders (it never sends `mode:"live"`).
- Reuses the attach-or-headless routing so it talks to the single profile owner (no double-DB-writer conflict).

## Data flow (one tick)
`StrategyRunner` → `ToolCaller.call(get_quote/pm_get_market/portfolio…)` → builds `MarketSnapshot` → `Strategy.propose(snapshot)` → for each intent: `call(prepare_order)` → if prepared `call(submit_order, paper)` → log + (substrate audits) → loop.

## Error handling
A read-tool failure for one symbol → that symbol is omitted from the snapshot (loop continues). A `prepare_order` rejection → logged, skip submit. A `submit_order` rejection (gate/cap) → logged. Kill-switch / paper-disabled → clean stop. LLM no-provider/parse-error → log + zero intents this tick. Nothing throws the loop.

## Testing strategy
- **Harness (`StrategyRunner`) with fakes:** a fake `ToolCaller` (scripts prepare/submit results) + a fake `Strategy`: assert the loop calls prepare then submit for `prepared` intents, skips submit for rejected, stops on a `kill switch engaged` result, and respects `--max-iters`.
- **`MeanReversionStrategy`:** feed a deterministic price sequence → assert it emits a BUY on the dip and a SELL-to-close on the rip, and nothing in-band.
- **`LlmStrategy` with a fake LLM:** a canned JSON array → asserts correct intent parsing; a malformed/no-provider case → asserts zero intents + no throw.
- **e2e:** with `cli.allow_paper_trading=true`, run `ai run strategy meanrev --mode paper --max-iters 3 --symbols AAPL` against a fake/seeded quote → assert `trade_audit` gains prepare/submit rows and the run prints a summary. Kill-switch e2e: engage `cli.kill_switch` → the loop halts on the next tick.
- **No-regression:** the substrate + existing CLI/daemon tests unchanged; the loop adds a CLI group only.

## Risks
- **LLM nondeterminism / cost** — bounded by `--max-iters`/`--interval`; the LLM strategy fails safe to zero intents; the substrate gates every intent regardless. Tests use a fake LLM (no live calls).
- **Double DB writer** — avoided by reusing the attach-or-headless routing (single owner).
- **Runaway loop** — bounded by max-iters/duration + SIGINT + the kill switch.
- **No edge** — explicitly not the goal of v1; this is the harness to *measure* edge in paper. The deterministic strategy is a demo, not a recommendation.

## Follow-ups (out of scope)
- Live mode for the loop (Phase-C-gated) — deferred; paper-first.
- Strategy persistence/scheduling as a managed background service (vs a foreground CLI run).
- Richer strategies, backtest integration (the existing `BacktestEngine`), multi-strategy portfolios.
- Per-strategy P&L attribution beyond the shared `trade_audit`/paper portfolio.

## Open questions (resolve in the plan)
- Default interval (proposed 15s) and whether a `--once` single-tick mode is worth adding for demos.
- Whether the deterministic strategy keeps its rolling window in memory only (proposed: yes — v1 is a foreground run; persistence is a follow-up).
