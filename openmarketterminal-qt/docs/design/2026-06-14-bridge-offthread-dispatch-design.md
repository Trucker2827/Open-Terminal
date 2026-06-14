# Off-Thread Bridge Tool Dispatch — Design Spec

**Status:** Approved design (pending user spec review) — precedes the plan.
**Date:** 2026-06-14
**Builds on:** the shipped Phase-3 `serve` daemon (on `main`). Fixes daemon production-readiness debt #1.

## Goal

Make the `serve` daemon production-ready by dispatching bridge tool calls **off the event-loop thread**, so a slow/blocking tool can never freeze the daemon's loop (which today makes SIGTERM / `serve --stop` unresponsive). Plus an interim safety net: `serve --stop` SIGKILL-after-grace.

## Background — the deadlock (verified)

`TerminalMcpBridge::handle_post_tool` (internal branch) calls `McpProvider::call_tool_async(name,args)` **on the main/event-loop thread**. In `call_tool_async`:
- **async handlers** run non-blocking with a watchdog timer — fine.
- **legacy sync handlers** (the line-177 path) are invoked **inline on the calling thread**, result wrapped in a resolved future.

`get_quote` is a sync handler; on a cold cache it calls `detail::run_async_wait`, which (off the main thread) blocks on a `QWaitCondition` while the **main loop** delivers the network/process reply that releases it. Called on the main thread, the loop is blocked waiting on itself → **hard deadlock**. The `QSocketNotifier` that turns SIGTERM into `quit()` can't fire, so `serve --stop` can't stop the daemon (needs SIGKILL). The GUI hides this with a warm DataHub cache; the daemon exposes it. (`HeadlessRuntime::call_tool` already avoids this by running on a worker — that's the pattern to mirror.)

## Design

### Bridge-scoped, single-worker off-thread dispatch
- **Scope:** fix `TerminalMcpBridge::handle_post_tool` only (the shared bridge — used by the daemon AND the GUI's in-app Python agents). Do **not** change `McpProvider::call_tool_async` centrally — GUI *in-process* tool calls stay exactly as today (lower blast radius).
- **Concurrency:** a **single dedicated worker** — a `QThreadPool` member on `TerminalMcpBridge` with `setMaxThreadCount(1)`. Tool calls run one-at-a-time off the main loop, preserving today's serialized semantics (no new concurrent access to services/DataHub that were never audited for parallel tool execution).
- **Dispatch (internal branch):** replace the inline
  ```cpp
  CallFlagGuard guard(destructive_ok);
  future = McpProvider::instance().call_tool_async(tool_name, args);
  ```
  with
  ```cpp
  future = QtConcurrent::run(&tool_pool_, [tool_name, args, destructive_ok]() {
      CallFlagGuard guard(destructive_ok);                 // set on the WORKER thread
      return McpProvider::instance().call_tool(tool_name, args);  // sync; runs off the main loop
  });
  ```
  The existing `QFutureWatcher<ToolResult>` (owned by the bridge, on the main thread) fires `finished` on the main thread when the worker future completes → the socket write happens on the main thread exactly as today.
- **`CallFlagGuard` moves onto the worker:** the thread-local `is_destructive_allowed()`/`is_call_in_progress()` flags are read by the auth-checker *during* `call_tool_async`'s synchronous auth phase. Since dispatch now runs on the worker, the guard must be set inside the worker lambda (before `call_tool`), so the auth-checker — running on the worker — reads the correct per-thread flags. (This is the one behavioral subtlety; today the guard is on the main thread because dispatch was on the main thread.)
- **External `serverId` branch unchanged:** `McpService::execute_openai_function_async` already wraps blocking JSON-RPC in its own `QtConcurrent::run` (non-blocking on the main loop), so it isn't part of the deadlock and is left as-is.

### `serve --stop` SIGKILL-after-grace (interim safety net)
`serve_stop`: after `SIGTERM`, poll `is_pid_alive(pid)` for a short grace window (e.g. up to 5s, 100ms steps); if still alive, `SIGKILL`. So even a daemon wedged by some *other* blocking path is always stoppable. (With the off-thread fix the main loop is responsive and SIGTERM alone suffices; this is belt-and-suspenders.)

## Why this is thread-safe
- **Serialized:** one worker → no two tool handlers run concurrently → no new concurrent access to services/DataHub vs today (which serialized them on the main loop).
- **Guard correctness:** the thread-locals are set and read on the same (worker) thread within one dispatch.
- **Result marshaling:** `ToolResult` is a value type (bool + QString + QJsonObject); the `QFutureWatcher` delivers it to the main thread; the socket write stays on the main thread (Qt sockets are not thread-safe — keep all socket I/O on the main thread).
- **DataHub cross-thread:** DataHub already marshals via queued connections; a tool on the worker reading/peeking it is the same as `HeadlessRuntime::call_tool` does today.

## What is NOT changed
- `McpProvider::call_tool_async` (and its async-handler watchdog) — untouched; central behavior identical for in-process callers.
- GUI in-process tool calls — unchanged.
- The external MCP server path — unchanged.
- The read-only daemon auth-checker, gates, single-owner, `kind` — unchanged.

## Error handling / known limits
- A **sync** handler that truly never returns still blocks the single worker → subsequent bridge tool calls queue behind it (throughput stalls), BUT the **main loop stays responsive** → `serve --stop`/SIGTERM work, and the daemon is stoppable. This is the production-readiness win.
- Per-tool **timeout for sync handlers** (so a wedged tool frees the worker) is harder (can't safely cancel a blocked sync handler) → **future debt**, not in this scope. Async handlers already have the watchdog.

## Testing strategy
- **Daemon stays responsive under a slow tool (the headline):** start `serve`; fire a tool that exercises the off-main dispatch; assert `serve --status` responds and `serve --stop` stops the daemon **promptly** (within a couple seconds) and removes `bridge.json` — i.e. the loop isn't wedged. (Network-dependent `get_quote` may return rc 0/5; the gate is "the daemon remained controllable," which the old code failed.)
- **Off-main proof:** a unit/integration check that the tool handler does NOT run on the main thread (e.g. a test tool records `QThread::currentThread()` != main).
- **SIGKILL-after-grace:** simulate a daemon that ignores SIGTERM (or a wedged one) → `serve --stop` escalates to SIGKILL and returns success; `bridge.json` cleaned (the dead-PID is detected).
- **GUI no-regression:** the GUI still builds + all `--selftest-*` pass; in-app agent tool calls via the bridge still work (the bridge path now off-thread — agents must still get correct results). Phase-1/2 attach + headless one-shot + the daemon e2e all still pass.

## Risks
- **Thread-affinity of a specific tool handler** that (incorrectly) assumes the main thread — serialized single-worker + the existing `HeadlessRuntime::call_tool` precedent (which already runs tools off-main) make this low; the GUI-no-regression + agent-path tests are the backstop.
- **`CallFlagGuard` placement** — the one real behavioral change; covered by an agent-destructive-path test (a destructive call with the right token still permitted; without it denied) to confirm the thread-local still gates correctly on the worker.

## Open questions (non-blocking)
- Per-sync-handler timeout (free a wedged worker) — future hardening.
- Whether to later make the worker a small fixed pool (>1) after auditing service thread-safety — deferred; serialized is the safe default now.
