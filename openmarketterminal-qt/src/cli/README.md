# openterminalcli (Phase 1 — attach mode)

Headless client that attaches to a **running** OpenTerminal GUI over its
localhost bridge. Read-only / non-destructive.

## Usage
    openterminalcli [--json] [--profile <name>] <group> <command> [args]

    status                          # is an instance attached?
    version
    mcp list | describe <tool> | call <tool> '<json>'
    hub topics | peek <topic> | request <topic>
    quote <SYM...>

## Exit codes
    0 ok · 2 usage · 3 no running instance · 4 token rejected
    5 tool returned success:false · 6 transport/HTTP error

## Notes
- Requires the GUI running with `bridge.autostart` on (default).
- stdout = data; stderr = diagnostics. Use `--json … | jq`.
- The CLI never sends the destructive token, and bridge.json carries no
  destructive token; live/destructive actions in attach mode are out of scope
  (a future phase). See docs/design/2026-06-14-openterminalcli-design.md.
