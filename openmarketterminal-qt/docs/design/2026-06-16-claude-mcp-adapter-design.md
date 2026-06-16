# OpenTerminal → Claude Code MCP Adapter — Design Spec

**Status:** Approved direction (1a + 3a: bridge the running terminal into Claude Code as native MCP tools). **Real-money-capable surface (gated server-side).**
**Date:** 2026-06-16

## Goal
Let Claude Code (this agent) call the running OpenTerminal's tools as **native `mcp__openterminal__*` tools** instead of shelling out to `openterminalcli mcp call` through Bash. Pure convenience/ergonomics layer — it adds **no capability** that the CLI doesn't already have, and changes **no** server-side gate.

## Why an adapter (not a direct connection)
The terminal's bridge is a **custom HTTP API** (`GET /tools`, `POST /tool` on `127.0.0.1:<port>`), not the MCP protocol. MCP clients (Claude Code) speak MCP JSON-RPC. So we need a thin translator. The translator wraps the existing **`openterminalcli`** (which already does bridge discovery + auth + protocol), rather than re-implementing the HTTP bridge.

## Architecture
```
Claude Code ──(MCP stdio, newline-delimited JSON-RPC 2.0)──► adapter (python)
   adapter ──(subprocess)──► openterminalcli --json mcp list | mcp call ──► running OpenTerminal bridge
```
- **One file:** `scripts/mcp/openterminal_mcp.py` — Python 3, **stdlib only** (no deps, no venv pollution). Hand-rolled MCP stdio server (newline-delimited JSON-RPC; MCP stdio framing, not LSP Content-Length).
- Registered once: `claude mcp add openterminal -- /usr/bin/python3 <repo>/scripts/mcp/openterminal_mcp.py` (user scope). The CLI path is resolved from `$OPENTERMINAL_CLI` env (default `/tmp/ot-build-ht/openterminalcli`), so a future stable install just changes the env.

## MCP methods implemented (minimal server)
- `initialize` → `{protocolVersion: "2024-11-05", capabilities: {tools: {}}, serverInfo: {name: "openterminal", version: "0.1"}}`.
- `notifications/initialized` → no response (notification).
- `tools/list` → run `openterminalcli --json mcp list` → parse `{"tools":[{name, description, inputSchema, serverId}]}` → return MCP `{tools: [{name, description, inputSchema}]}`. **Merge** the static execution-tool allowlist (below). De-dupe by name.
- `tools/call` (`{name, arguments}`) → run `openterminalcli --json mcp call <name> '<arguments-json>'`.
  - exit 0 → stdout is the result JSON; return `{content: [{type: "text", text: <stdout>}]}`.
  - non-zero / stderr `tool error: <msg>` → return `{content: [{type: "text", text: <msg>}], isError: true}` (an MCP tool error, not a protocol error — so the agent sees the failure and can react).
- Unknown method → JSON-RPC error `-32601`.

## The hidden-execution-tools allowlist (the one real wrinkle)
`openterminalcli mcp list` returns a **curated catalog that omits the gated execution tools** (`crypto_submit_order`, `crypto_cancel_order`, `fast_submit_order`, `cancel_order`) — though `mcp call` executes them fine. For Claude Code to invoke a tool natively it must appear in `tools/list`. So the adapter carries a **small static table** of those tools' names + input schemas (hand-written, mirroring their real schemas) and merges them into `tools/list`.

**Decision (flag for user):** this exposes **real-money execution** as native Claude Code tools. The server-side gates (kill-switch → arms → venue/allowed-account → risk floor → audit) apply identically regardless of whether a tool is invoked via Bash or native MCP — invocation path does not weaken any gate. v1 includes the execution tools (consistent with "drive the terminal"); if the user prefers, v1 can ship **read-only** (omit the allowlist) and execution stays via Bash.

## Data flow (example)
```
Claude Code → tools/call {name: "get_crypto_balance", arguments: {}}
  → adapter: openterminalcli --json mcp call get_crypto_balance '{}'
  → stdout {"data":{"balances":{...}},"success":true}
  → {content:[{type:"text", text:"{...}"}]}
```

## Error handling
- **Terminal not running:** adapter runs `openterminalcli --json status`; if `attached:false` (or discovery error), `tools/list` still returns the static allowlist (so the schema is visible) and `tools/call` returns `isError` with "OpenTerminal is not running — launch the app first." Never crashes the MCP session.
- **Malformed arguments JSON:** returned as an `isError` tool result.
- **`openterminalcli` not found at the resolved path:** every call returns a clear `isError`; `tools/list` returns the static set.
- Adapter never writes to stdout except framed JSON-RPC (logs go to stderr only — stdout pollution breaks the MCP transport).

## Testing
- **Unit (stdio protocol, no Claude Code, no terminal):** pipe a scripted JSON-RPC session into the adapter (`initialize` → `notifications/initialized` → `tools/list` → `tools/call`) with `openterminalcli` stubbed via a fake on `$PATH`/`$OPENTERMINAL_CLI`; assert: valid `initialize` result; `tools/list` includes a stub tool **and** the static execution tools; `tools/call` success maps stdout→content; `tool error:` maps to `isError`; unknown method → `-32601`; stdout contains only JSON-RPC (no stray prints). A Python `unittest` driving the adapter as a subprocess.
- **Integration (manual):** `claude mcp add openterminal -- python3 <path>` with the app running → confirm `mcp__openterminal__get_ticker` etc. appear and a read call returns live data. (Done by the operator/me in-session, not a subagent.)

## Files
- **Create** `scripts/mcp/openterminal_mcp.py` — the adapter.
- **Create** `scripts/mcp/README.md` — one-paragraph: what it is + the exact `claude mcp add` command + the `$OPENTERMINAL_CLI` note.
- **Create** `tests/mcp_adapter/test_openterminal_mcp.py` (or `scripts/mcp/test_openterminal_mcp.py`) — the stdio protocol unittest.
- **No C++ changes.** No change to the terminal, the bridge, or any tool.

## Risks
- **Real-money tools become native** — mitigated: gates are server-side and invocation-path-independent; documented; user can opt read-only.
- **Process-spawn per call** (~tens of ms latency) — acceptable for interactive use; a direct-HTTP-to-bridge optimization is a possible follow-up (would re-implement discovery + the GET/POST bridge protocol — deferred).
- **Ephemeral CLI path** (`/tmp/ot-build-ht`) — mitigated by `$OPENTERMINAL_CLI`; a stable install path is a follow-up.
- **Catalog drift** — if the hidden execution-tool schemas change, the static allowlist goes stale; mitigated by keeping the allowlist minimal (name + the few required params) and noting it in the README.

## Follow-ups (out of scope)
- Direct HTTP-to-bridge (skip the `openterminalcli` subprocess).
- Auto-resolve a stable installed `openterminalcli`.
- Surface the full (uncurated) tool catalog if the bridge later exposes one.
