# openterminalcli Phase 3 — `serve` Daemon (Design Spec)

**Status:** Design spec only (no implementation this round — per the roadmap, item 4 is the spec).
**Date:** 2026-06-14
**Builds on:** Phase 1 (attach + `bridge.json`), Phase 2a (`openterminal_core`, `HeadlessRuntime`, `--headless`, gates + `>=Verified` floor), Phase 2b (10 data services headless, the AgentService one-shot guard) — all shipped on `main`.

## Goal

A long-lived **headless daemon** — `openterminalcli serve` — that hosts the DataHub, the data services, and the `TerminalMcpBridge` with no GUI, so the CLI and Python agents can attach and run tools **including live, streaming subscriptions** (feeds) that one-shot `--headless` can't sustain. On a server/CI box with no display, `serve` is the always-on terminal brain.

## Why (vs one-shot `--headless`)

One-shot `--headless` brings everything up, runs one command, exits — so it can't sustain a subscription (Maritime AIS stream, Polymarket WS, rolling quote updates), and pays full init cost per call. The daemon initializes once and stays up, so: (a) **live feeds/subscriptions run continuously**, (b) attach calls are instant (no per-call init), (c) `hub watch <topic>` streaming becomes possible, (d) agents get a persistent tool host without a GUI.

## Scope (user-chosen: live-feed host, single bridge owner)

- A persistent process that **runs live feeds/subscriptions**, not just a warm service host.
- **Single `bridge.json` owner per profile:** `serve` refuses to start if a GUI **or** another daemon already owns the profile's `bridge.json` (PID-alive). The CLI attaches to whichever instance is up — transparently (it already discovers `bridge.json`).
- Reuse the existing bridge + discovery + gates. **No new client protocol** — `openterminalcli mcp call …` (attach mode) works against the daemon unchanged.

### Non-goals
- Remote / non-localhost access (the bridge is 127.0.0.1-only; the token is a same-user secret). Multi-user/network serving is out of scope.
- Attach-mode CLI **destructive** execution (still deferred from 2b — read + non-destructive by default; the revocable-token mechanism is described below as future, not built).
- Running on the same port alongside a GUI (rejected the "separate port, coexist" option — single owner avoids shared-SQLite write contention).
- GUI-control categories (navigation/workspace/dashboard) — no window; remain GUI-only.

## Architecture

### The daemon = persistent HeadlessRuntime + bridge host
`openterminalcli serve` runs, under a long-lived `QCoreApplication`:
1. **Single-owner check:** read `<profile_root>/bridge.json`; if present and its PID is alive → refuse (`exit 3`, "an instance already owns this profile"). (Reuse `BridgeDiscoveryFile::read` + `is_pid_alive`.)
2. `HeadlessRuntime::init(profile)` — DB + migrations + SecureStorage + DataHub + `register_all_data_services()` + `register_core_tools()` + the headless auth-checker (gates + `>=Verified` floor).
3. **Start the bridge explicitly:** `TerminalMcpBridge::instance().start()` (binds 127.0.0.1:ephemeral, writes `bridge.json` with the daemon's PID). The daemon owns the bridge — it does not rely on AgentService's auto-start.
4. **Enter the event loop** (`app.exec()`) — feeds/subscriptions now live; the bridge services concurrent client requests.

### "Daemon mode" — a third execution mode (the key new concept)
2b established two modes via the AgentService ctor guard (`gui_mode = inherits("QApplication")`): **GUI** (start bridge + GUI auth-checker) and **one-shot headless** (neither). The daemon is a **third**: it IS a `QCoreApplication` (so `gui_mode==false` → AgentService does NOT auto-start the bridge or install the GUI auth-checker — correct), but the daemon **explicitly** starts the bridge and installs the **headless** auth-checker. So:
- The daemon's headless auth-checker is the sole gate (no GUI-checker conflict — the gui_mode guard already prevents AgentService from installing the GUI one under `QCoreApplication`).
- Feeds DO run (unlike one-shot, the daemon has a persistent event loop + subscriptions). The 2b one-shot guard isn't bypassed — feeds start on **subscription**, which only happens because the daemon stays alive long enough for clients to subscribe; a feature, not a guard violation.

### Client side (minimal change)
- `openterminalcli serve [--profile X] [--foreground]` — start the daemon. `--status` — report the owning instance (from `bridge.json`). `--stop` — signal the owning daemon to shut down (only if it's a daemon, not a GUI — distinguish via a `"kind":"daemon"` field added to `bridge.json`).
- `mcp`/`hub`/`quote`/`status` attach unchanged. The daemon being up makes **`hub watch <topic>`** (streaming) viable — a new client command that holds a connection and prints updates (the daemon pushes DataHub changes). (Streaming over the current request/response bridge needs a small protocol addition — flagged in Open Questions; the non-streaming commands need nothing new.)

### Lifecycle
- **Foreground by default** (`serve` runs in the terminal; Ctrl-C/SIGINT/SIGTERM → clean shutdown). Daemonizing (background/detach, logfile) is an Open Question — recommend foreground-first; users can `nohup`/systemd it.
- **Clean shutdown:** a Unix signal handler (SIGTERM/SIGINT) posts `QCoreApplication::quit()`; `aboutToQuit` → stop feeds + `TerminalMcpBridge::stop()` (removes `bridge.json`). Same removal path the GUI uses.
- **Crash/stale `bridge.json`:** if the daemon dies uncleanly, `bridge.json` is left with a dead PID; the next `serve` (or any attach) detects the dead PID (`is_pid_alive`) → treats it as stale (attach → `exit 3`; `serve` → overwrites and starts). Mirrors Phase-1 stale handling.

### Concurrency
The bridge already accepts concurrent localhost connections; the daemon's services + DataHub are the shared in-process state (single-threaded event loop + the existing worker-thread tool dispatch from 2a). Multiple CLI clients / agents share one daemon. No new locking beyond what the services already use.

## Security
- **Localhost + token only** (unchanged): `bridge.json` is `0600`, the token is same-user. No remote exposure.
- **Gates server-enforced** by the daemon's headless auth-checker: `>=Verified` denied; destructive → `cli.allow_trading`; settings-write → `cli.allow_settings_write`. Default-deny holds.
- **Destructive over the daemon (future):** like 2b attach, the daemon does NOT expose a destructive path by default. If later wanted, the design is a **revocable** capability: the daemon writes the `destructive_token` into `bridge.json` only while `cli.allow_trading` is true AND re-evaluates the bridge gate **live** (`cli_trading_allowed()` at dispatch, not just token-match) so a GUI/daemon toggle-off revokes mid-session — fixing the replay gap noted in 2a. Out of scope for this spec's build; documented so it's not re-litigated.
- **Risk floor preserved:** the daemon gates tool *admission* only; the deterministic trading risk-limit/kill-switch is untouched.

## Error handling & exit codes
Reuse the CLI's scheme. `serve`: `0` clean exit; `3` an instance already owns the profile (refuse); `7` init failure (DB/migrations/service bring-up). `--stop`: `0` stopped; `3` no daemon to stop (or the owner is a GUI, not a daemon → refuse, "use the GUI to quit it"). Attach clients unchanged (`3` no instance, `4` token, `5` tool fail, `6` transport).

## Testing strategy (for the eventual plan)
- **Unit:** single-owner refusal (write a live-PID `bridge.json` → `serve` refuses); stale-PID overwrite; the `"kind":"daemon"` discriminator; clean-shutdown removes `bridge.json`.
- **Integration (the real proof):** start `serve` in the background; `openterminalcli status` shows attached-to-daemon; `mcp call get_quote` returns via the daemon; a **subscription stays live** across two calls (the daemon's value — e.g. peek a streaming topic, see it update); SIGTERM → `bridge.json` removed + process exits. A second `serve` while one is up → refused.
- **No-regression:** GUI still builds + selftests pass + Phase-1/2 attach + headless one-shot all unchanged (the daemon is additive).

## Open questions (for the plan, non-blocking the spec)
1. **Streaming transport for `hub watch`:** the current bridge is request/response (`Connection: close`). Streaming needs either chunked/SSE responses or a WebSocket endpoint (Qt6::WebSockets is available). Decide during the plan; the non-streaming daemon is fully useful without it.
2. **Daemonization:** foreground-only (recommend) vs a `--daemon` detach + logfile + pidfile. Recommend foreground-first.
3. **Which feeds auto-start vs subscription-driven:** likely all subscription-driven (no client subscribed → no feed running, even in the daemon) to avoid needless connections; confirm in the plan.
4. **`serve` on Linux/CI:** the daemon is the natural CI/server use; ensure it runs under the same Qt the regression job uses.
