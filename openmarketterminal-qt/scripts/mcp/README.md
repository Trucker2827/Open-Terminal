# OpenTerminal → Claude Code MCP adapter

`openterminal_mcp.py` is a tiny MCP stdio server that exposes the **running**
OpenTerminal's read-only tools (market data, balances, analysis) to Claude Code
as native `mcp__openterminal__*` tools. It forwards each call to
`openterminalcli --json mcp list|call`, so the terminal app must be running.

**Read-only:** it passes through the terminal's curated catalog, which excludes
the gated real-money execution tools. Live order placement stays on the explicit
`openterminalcli mcp call` path.

## Register with Claude Code

```bash
# OPENTERMINAL_CLI must point at your openterminalcli binary (default: /tmp/ot-build-ht/openterminalcli)
claude mcp add openterminal \
  --env OPENTERMINAL_CLI=/tmp/ot-build-ht/openterminalcli \
  -- python3 /ABSOLUTE/PATH/TO/openmarketterminal-qt/scripts/mcp/openterminal_mcp.py
```

Then, with the OpenTerminal app running, the `mcp__openterminal__*` tools become
available. Verify: `claude mcp list` (shows `openterminal`), then call e.g.
`get_ticker {"symbol":"BTC/USD"}`.

## Notes
- stdout is reserved for MCP JSON-RPC; all logs go to stderr.
- If the terminal isn't running, `tools/call` returns a tool error (not a crash).
- The `/tmp/ot-build-ht` CLI path is a dev build; point `$OPENTERMINAL_CLI` at a
  stable install when you have one.
- Run the tests: `python3 -m unittest tests/mcp_adapter/test_openterminal_mcp.py`.
