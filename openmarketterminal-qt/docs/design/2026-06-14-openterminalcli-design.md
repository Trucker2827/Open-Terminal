# openterminalcli — Phase 1 "Attach Mode" Design Spec

**Status:** Approved design — ready for implementation plan
**Date:** 2026-06-14
**Scope:** Phase 1 only (read-only attach to a running GUI). Phases 2–3 sketched for context.

## Goal

A headless command-line front door to the same terminal brain the Qt GUI runs — markets, DataHub, MCP tools — by attaching to a **running** `OpenTerminal` instance over its existing localhost `TerminalMcpBridge`. One binary that later grows a headless mode and a daemon mode without changing its command surface.

## Why (three first-class audiences)

1. **Automation / scripting / CI** — run terminal capabilities from shell, cron, or another repo without keeping the GUI in focus.
2. **Claude-as-operator (first-class goal).** Today an AI agent (Claude Code in the user's terminal) can *read, edit, and build* the codebase but cannot *drive the running app* — verifying a feature at runtime means building it and asking a human to click around. A CLI with `--json` output turns the build→verify loop into something the agent can execute and assert on directly: call any MCP tool, run self-tests, inspect live DataHub state, all from the shell. This directly serves the project's verification discipline ("a passing test proves nothing until you've confirmed it RAN").
3. **Operator ergonomics** — inspect the tool catalog, peek DataHub topics, check feed health without the GUI.

## Non-goals (Phase 1)

- **No destructive / live actions.** No live order placement, no arbitrary Python exec, no workflow/agent mutation. The CLI never receives the bridge's `destructive_token`. (Deliberate safety boundary — see §6.)
- **No headless in-process mode** — Phase 1 requires a running GUI. Headless is Phase 2.
- **No daemon / `serve`** — Phase 3.
- **No streaming** (`hub watch`) — needs a persistent connection; later phase.
- **No new trading/market logic** — the CLI only calls existing C++ services via existing MCP tools.

## Background — the bridge as it exists today

`src/mcp/TerminalMcpBridge.{h,cpp}` is a hand-rolled HTTP/1.1 server (no QHttpServer module in this Qt build):

- Binds `127.0.0.1` on an **ephemeral port** (`listen(QHostAddress::LocalHost, 0)`).
- **Auth:** every request must carry `X-MCP-Token: <uuid>` (the `token_` minted at `start()`); mismatch → `401`. A *separate* `destructive_token_` is required via `X-MCP-Allow-Destructive` to run destructive tools.
- **Routes:**
  - `GET /tools` → `{ "tools": [ { name, description, inputSchema, serverId }, … ] }`. Applies a default filter excluding categories `navigation, system, settings, ai-chat, meta`.
  - `POST /tool` body `{ "id", "tool", "args": {…}, "serverId"? }` → the tool's `ToolResult.to_json()` with `"id"` echoed back.
- `Connection: close` — one request per socket.
- Public accessors already exist: `endpoint()`, `token()`, `destructive_token()`, `is_active()`, idempotent `start()`, `stop()`.

**Two gaps that block external attach:**

- **G1 — not always running.** `start()` is only called from `AgentService` (`AgentService.cpp:89`), so the bridge listens only once the agent subsystem runs, not whenever the GUI is up.
- **G2 — no discovery.** The endpoint + token reach Python agents today via in-process config injection (`AgentService.cpp:289–297`). An external process has no way to learn the port or token.

## Design

### 1. Bridge changes (C++, in the GUI binary)

**1a. Start the bridge at GUI boot (fixes G1).** Call `TerminalMcpBridge::instance().start()` during app init (after DB + core services are up, alongside the other service registrations in `main.cpp`). `start()` is idempotent, so `AgentService`'s existing call becomes a no-op when already active. Gate behind an existing/new setting `bridge.autostart` (default **true**) so it can be disabled.

**1b. Write a discovery file on start, remove on stop (fixes G2).** On successful `start()`, write `<profile_root>/bridge.json`; on `stop()` (and `aboutToQuit`), delete it. File contents:

```json
{
  "schema": 1,
  "endpoint": "http://127.0.0.1:54923",
  "token": "<uuid>",
  "pid": 12345,
  "started_at": "2026-06-14T12:34:56Z"
}
```

- **Location:** `ProfileManager::profile_root()/bridge.json` (macOS: `~/Library/Application Support/OpenMarketTerminal[/profiles/<name>]/bridge.json`). Per-profile, so multiple profiles don't collide.
- **Permissions:** `0600` (owner-only). Localhost-only token; same-user trust boundary.
- **Deliberately excludes `destructive_token`** — a file-based reader gets read/non-destructive access only.
- **Staleness:** the file includes `pid`; a reader treating it as stale if the PID is dead is a CLI-side concern (§4).

### 2. The CLI binary (`openterminalcli`)

A new C++ executable target linking only `Qt6::Core` + `Qt6::Network` (a `QCoreApplication`, no Widgets, no display). Source under `src/cli/`. Internally three small units:

- **`BridgeDiscovery`** — resolves `profile_root()` (honoring `--profile`), reads + validates `bridge.json`, checks PID liveness, yields `{endpoint, token}` or a typed "no running instance" error.
- **`BridgeClient`** — a minimal HTTP/1.1 client (QTcpSocket or QNetworkAccessManager) that issues `GET /tools` and `POST /tool` with the `X-MCP-Token` header, parses the JSON response, and maps transport/HTTP/tool errors to typed results. Never sends `X-MCP-Allow-Destructive`.
- **`CommandDispatch`** — argv → command → `BridgeClient` call → formatted output.

The same target is the seed that, in Phase 2, additionally links `openterminal_core` and gains `--headless` (the command tree and dispatch are unchanged; only the transport behind them swaps from "HTTP to a running app" to "in-process services").

### 3. Command tree (Phase 1 subset)

```
openterminalcli [--json] [--profile <name>] <group> <command> [args]

mcp   list                      # GET /tools  → catalog
      describe <tool>           # one tool's schema (from the catalog)
      call <tool> '<json-args>' # POST /tool  (non-destructive only; see §6)
hub   topics                    # mcp call of the DataHub list tool
      peek    <topic>           # mcp call datahub peek
      request <topic> [k=v...]  # mcp call datahub request
quote <SYM...>                  # thin alias → mcp call get_quote
selftest <name>                 # see §3a
status                          # is an instance attached? endpoint, pid, tool count
version
```

Aliases (`quote`, `hub *`) are thin wrappers that construct the right `mcp call`. The generic `mcp call` is the backbone; everything else is sugar.

**3a. `selftest` in Phase 1** maps to the data-only self-tests the bridge can drive via tools (e.g. a catalog/`/tools` sanity check, a DataHub peek round-trip). The richer one-shot binaries (`--selftest-feeds`, `--selftest-paper`, …) currently require constructing the app; those become true CLI subcommands in **Phase 2** (headless), not Phase 1. Phase 1 `selftest` is explicitly the attach-drivable subset; the doc must not imply otherwise.

### 4. Output & error contract

- **stdout = data only; stderr = logs/diagnostics.** Enables `openterminalcli --json … | jq` and clean agent parsing.
- **Human mode (default):** compact tables / key-value lines.
- **`--json` mode:** the tool's JSON result (or a `{ "ok": false, "error": {...} }` envelope) printed compact to stdout.
- **Exit codes:** `0` success; `3` no running instance / stale `bridge.json`; `4` auth/token mismatch; `5` tool returned `success:false`; `6` transport/HTTP error; `2` usage error. (Distinct codes so scripts and agents can branch.)
- **Error surfaces:**
  - No `bridge.json` or PID dead → exit 3, stderr "No running OpenTerminal instance for profile '<p>'. Start the app, or check `bridge.autostart`." (`--json` → error envelope).
  - `401` → exit 4 (stale token; suggest the app was restarted — re-read file).
  - Tool `success:false` → exit 5, the tool's error message surfaced.

### 5. Profile resolution

`--profile <name>` (default `default`) selects which `bridge.json` to read, mirroring how `main.cpp` parses `--profile`. The CLI computes `profile_root()` with the same rule (`AppPaths::root()` for `default`, else `…/profiles/<name>`). No env vars required; `OPENTERMINAL_PROFILE` may be honored as a convenience later.

### 6. Claude-operability as an explicit design goal

The CLI is the programmatic surface that lets an AI operator (Claude) **drive, test, and inspect** the running terminal from the shell — not just build it. Concretely it enables, via the agent's Bash tool:

- `openterminalcli mcp call <tool> '<json>'` — exercise any non-destructive capability and assert on the result.
- `openterminalcli --json …` — machine-parseable output for assertions.
- `openterminalcli selftest …` / `hub peek …` — runtime verification the agent runs itself, closing the build→verify loop that is otherwise human-in-the-loop.

**The boundary is intentional and enforced by design, not by policy:**

- The CLI **never holds the `destructive_token`** (it is excluded from `bridge.json`), so the file-discovery path **cannot** place live orders, run arbitrary Python, or mutate workflows/agents — even if the calling agent is prompt-injected. This mirrors the project's safety floor: "the AI is the brain but cannot lift its own leash."
- Destructive access, if ever wanted from the CLI, is a **separate later phase** behind an explicit, human-supplied opt-in token — never the default, never silent.
- The CLI is a *programmatic* surface, not a *visual* one: it cannot validate rendered UI. GUI-visual testing remains human (or screenshot-based).

This makes "full access to implement and test" precise: **full read + non-destructive operate/verify access; live/destructive actions deliberately out of reach.**

## Testing strategy

Phase 1 is runtime-testable end-to-end on a dev Mac:

1. Build the GUI (with bridge autostart) + `openterminalcli`.
2. Launch `OpenTerminal.app`; confirm `bridge.json` appears at `profile_root()` with `0600` perms and a live PID.
3. `openterminalcli status` → reports attached endpoint + tool count.
4. `openterminalcli mcp list` → non-empty catalog; matches `GET /tools`.
5. `openterminalcli --json mcp call get_quote '{"symbol":"AAPL"}'` → valid JSON quote; exit 0.
6. Negative: quit the app → `bridge.json` removed → CLI exits 3 with a clear message. Corrupt/stale file → exit 3. Tamper token → exit 4.
7. A regression check that asserts `bridge.json` is removed on clean quit (no stale creds left behind).

These are scriptable, so they double as the first things the Claude operator runs against itself.

## Phasing (context only — out of Phase-1 scope)

- **Phase 2 — headless one-shot.** Extract an `openterminal_core` static lib (the non-Widgets source groups: `NETWORK/DATAHUB/STORAGE/PYTHON/LLM_SERVICE/TRADING/ALGO_ENGINE/SERVICE` wholesale, plus the non-GUI subsets of `CORE/AUTH/MCP` by include-triage). `openterminalcli` links it and gains `--headless`; the existing `--selftest-*` one-shots become real subcommands running with no `QApplication`. Split `McpInit` into `register_core_tools()` (lib) + `register_gui_tools()` (exe); CLI installs a non-GUI (deny-all or stdin) `ToolConfirmationGate` presenter.
- **Phase 3 — `serve` daemon.** Long-lived headless DataHub + feeds + brokers, no GUI. Heaviest lift; only if demand is real.

## Open questions (not blocking Phase 1)

- Binary name `openterminalcli` vs a short alias (`ot`) — cosmetic; defer.
- Whether to honor `OPENTERMINAL_PROFILE` / a `--port`+`--token` manual override for CI without a profile manifest.
- Destructive opt-in mechanism for a future phase (human-supplied token, per-invocation confirm).
