#!/bin/bash
# End-to-end: launch the GUI, attach with the CLI, assert, quit, assert cleanup.
set -u
APP=/tmp/ot-build-ht/OpenTerminal.app
CLI=/tmp/ot-build-ht/openterminalcli
ROOT="$HOME/Library/Application Support/org.openterminal.OpenTerminal"
fail() { echo "FAIL: $1"; exit 1; }

rm -f "$ROOT/bridge.json"
open "$APP"; sleep 7
test -f "$ROOT/bridge.json" || fail "bridge.json not written"

"$CLI" status | grep -q "attached" || fail "status not attached"
"$CLI" mcp list | grep -q '"name"' || fail "mcp list empty"
"$CLI" --json mcp call get_quote '{"symbol":"AAPL"}' | grep -q '"success"' || fail "quote call failed"
"$CLI" status >/dev/null; [ $? -eq 0 ] || fail "status exit nonzero"

osascript -e 'quit app "OpenTerminal"' 2>/dev/null; sleep 3
test -f "$ROOT/bridge.json" && fail "bridge.json not removed on quit"

# Negative: with no app, status exits 3.
"$CLI" status; [ $? -eq 3 ] || fail "status should exit 3 when not running"
echo "PASS: e2e attach smoke"
