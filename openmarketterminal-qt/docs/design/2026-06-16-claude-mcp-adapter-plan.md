# OpenTerminal → Claude Code MCP Adapter — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A small Python stdlib MCP stdio server that bridges Claude Code to the running OpenTerminal (via `openterminalcli`), exposing the terminal's curated read-only tool catalog as native `mcp__openterminal__*` tools.

**Architecture:** Hand-rolled MCP stdio server (newline-delimited JSON-RPC 2.0). `tools/list` and `tools/call` shell out to `$OPENTERMINAL_CLI --json mcp list|call`. Pure passthrough of the curated catalog — read-only v1, no execution tools. stdout carries only framed JSON-RPC; logs go to stderr.

**Tech Stack:** Python 3 (stdlib only — `json`, `os`, `subprocess`, `sys`). No build system. Tested with `unittest` driving the adapter as a subprocess against a fake CLI stub.

**Spec:** `docs/design/2026-06-16-claude-mcp-adapter-design.md`

---

## Test command
`python3 -m unittest discover -s tests/mcp_adapter -v` (or `python3 tests/mcp_adapter/test_openterminal_mcp.py`)

## File structure
- **Create** `scripts/mcp/openterminal_mcp.py` — the adapter (one file, one responsibility: MCP stdio ↔ openterminalcli).
- **Create** `tests/mcp_adapter/test_openterminal_mcp.py` — subprocess protocol test with a fake CLI.
- **Create** `scripts/mcp/README.md` — what it is + the `claude mcp add` command + `$OPENTERMINAL_CLI` note.
- No C++ / terminal / bridge changes.

---

### Task 0: Branch

- [ ] **Step 1:**
```bash
cd ~/src/Open-Terminal/openmarketterminal-qt
git checkout -b feat/claude-mcp-adapter
git rev-parse --abbrev-ref HEAD   # expect: feat/claude-mcp-adapter
```

---

### Task 1: The adapter + protocol test (TDD)

**Files:**
- Create: `tests/mcp_adapter/test_openterminal_mcp.py`
- Create: `scripts/mcp/openterminal_mcp.py`

- [ ] **Step 1: Write the failing test** — `tests/mcp_adapter/test_openterminal_mcp.py`

```python
# test_openterminal_mcp.py — drives the MCP stdio adapter as a subprocess against
# a FAKE openterminalcli (set via $OPENTERMINAL_CLI) emitting canned --json output.
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

ADAPTER = str(pathlib.Path(__file__).resolve().parents[2] / "scripts" / "mcp" / "openterminal_mcp.py")

FAKE_CLI = '''#!/usr/bin/env python3
import sys, json
a = [x for x in sys.argv[1:] if x != "--json"]
if a[:2] == ["mcp", "list"]:
    print(json.dumps({"tools": [{"name": "get_ticker", "description": "Get ticker",
        "inputSchema": {"type": "object", "properties": {"symbol": {"type": "string"}},
        "required": ["symbol"]}, "serverId": "core"}]}))
    sys.exit(0)
if a[:2] == ["mcp", "call"]:
    name = a[2] if len(a) > 2 else ""
    if name == "get_ticker":
        print(json.dumps({"data": {"last": 65000}, "success": True})); sys.exit(0)
    sys.stderr.write("tool error: Tool not found: %s\\n" % name); sys.exit(2)
sys.exit(0)
'''


class TestAdapter(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fake = os.path.join(self.tmp.name, "fakecli")
        with open(self.fake, "w") as f:
            f.write(FAKE_CLI)
        os.chmod(self.fake, 0o755)

    def tearDown(self):
        self.tmp.cleanup()

    def _run(self, requests):
        env = dict(os.environ, OPENTERMINAL_CLI=self.fake)
        inp = "".join(json.dumps(r) + "\n" for r in requests)
        p = subprocess.run([sys.executable, ADAPTER], input=inp,
                           capture_output=True, text=True, env=env, timeout=30)
        lines = [l for l in p.stdout.splitlines() if l.strip()]
        objs = [json.loads(l) for l in lines]   # every stdout line MUST be valid JSON
        return objs, p.stdout, p.stderr

    def test_initialize(self):
        objs, _, _ = self._run([{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}])
        self.assertEqual(objs[0]["id"], 1)
        self.assertEqual(objs[0]["result"]["protocolVersion"], "2024-11-05")
        self.assertIn("tools", objs[0]["result"]["capabilities"])
        self.assertEqual(objs[0]["result"]["serverInfo"]["name"], "openterminal")

    def test_initialized_notification_has_no_response(self):
        objs, _, _ = self._run([{"jsonrpc": "2.0", "method": "notifications/initialized"}])
        self.assertEqual(objs, [])

    def test_tools_list_passthrough_no_execution_tools(self):
        objs, _, _ = self._run([{"jsonrpc": "2.0", "id": 2, "method": "tools/list"}])
        names = [t["name"] for t in objs[0]["result"]["tools"]]
        self.assertEqual(names, ["get_ticker"])  # exactly the catalog; nothing added
        for ex in ("crypto_submit_order", "crypto_cancel_order", "fast_submit_order", "cancel_order"):
            self.assertNotIn(ex, names)
        self.assertEqual(objs[0]["result"]["tools"][0]["inputSchema"]["required"], ["symbol"])

    def test_tools_call_success_maps_stdout_to_content(self):
        objs, _, _ = self._run([{"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                                 "params": {"name": "get_ticker", "arguments": {"symbol": "BTC/USD"}}}])
        res = objs[0]["result"]
        self.assertFalse(res.get("isError", False))
        self.assertIn("65000", res["content"][0]["text"])

    def test_tools_call_error_maps_to_iserror(self):
        objs, _, _ = self._run([{"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                                 "params": {"name": "nope", "arguments": {}}}])
        res = objs[0]["result"]
        self.assertTrue(res["isError"])
        self.assertIn("tool error", res["content"][0]["text"])

    def test_unknown_method_is_jsonrpc_error(self):
        objs, _, _ = self._run([{"jsonrpc": "2.0", "id": 5, "method": "foo/bar"}])
        self.assertEqual(objs[0]["error"]["code"], -32601)

    def test_stdout_is_only_jsonrpc(self):
        reqs = [{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
                {"jsonrpc": "2.0", "method": "notifications/initialized"},
                {"jsonrpc": "2.0", "id": 2, "method": "tools/list"}]
        objs, out, err = self._run(reqs)
        self.assertEqual(len(objs), 2)  # init + tools/list; the notification produced nothing
        for o in objs:
            self.assertEqual(o.get("jsonrpc"), "2.0")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it — confirm it FAILS (adapter doesn't exist yet)**

```bash
python3 -m unittest tests/mcp_adapter/test_openterminal_mcp.py 2>&1 | tail -5
# Expected: errors — adapter script missing (FileNotFoundError on the subprocess / non-zero).
```

- [ ] **Step 3: Write the adapter** — `scripts/mcp/openterminal_mcp.py`

```python
#!/usr/bin/env python3
"""OpenTerminal -> Claude Code MCP stdio adapter (read-only v1).

Bridges Claude Code (MCP stdio, newline-delimited JSON-RPC 2.0) to the running
OpenTerminal by shelling out to `openterminalcli --json mcp list|call`. Pure
passthrough of the terminal's curated tool catalog (which already excludes the
gated execution tools) — read-only. stdout carries ONLY framed JSON-RPC;
diagnostics go to stderr.
"""
import json
import os
import subprocess
import sys

CLI = os.environ.get("OPENTERMINAL_CLI", "/tmp/ot-build-ht/openterminalcli")
PROTOCOL_VERSION = "2024-11-05"


def log(msg):
    print("[openterminal-mcp] " + str(msg), file=sys.stderr, flush=True)


def run_cli(args, timeout=30):
    """Run `CLI --json <args>`; return (returncode, stdout, stderr). Never raises."""
    try:
        p = subprocess.run([CLI, "--json", *args], capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except FileNotFoundError:
        return 127, "", "openterminalcli not found at " + CLI + " (set $OPENTERMINAL_CLI)"
    except subprocess.TimeoutExpired:
        return 124, "", "openterminalcli timed out"
    except Exception as e:  # never crash the loop
        return 1, "", str(e)


def list_tools():
    rc, out, err = run_cli(["mcp", "list"])
    if rc != 0:
        log("mcp list failed rc=%d: %s" % (rc, err.strip()))
        return []
    try:
        data = json.loads(out)
    except Exception as e:
        log("mcp list parse error: %s" % e)
        return []
    raw = data.get("tools", []) if isinstance(data, dict) else []
    tools = []
    for t in raw:
        if not isinstance(t, dict) or not t.get("name"):
            continue
        tools.append({
            "name": t["name"],
            "description": t.get("description", ""),
            "inputSchema": t.get("inputSchema") or {"type": "object", "properties": {}},
        })
    return tools


def call_tool(name, arguments):
    rc, out, err = run_cli(["mcp", "call", name, json.dumps(arguments or {})])
    if rc == 0:
        text = out.strip() or "(no output)"
        return {"content": [{"type": "text", "text": text}], "isError": False}
    msg = err.strip() or out.strip() or ("openterminalcli exited %d" % rc)
    return {"content": [{"type": "text", "text": msg}], "isError": True}


def ok(req_id, result):
    return {"jsonrpc": "2.0", "id": req_id, "result": result}


def err(req_id, code, message):
    return {"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": message}}


def handle(req):
    method = req.get("method")
    req_id = req.get("id")
    if method == "initialize":
        return ok(req_id, {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "openterminal", "version": "0.1"},
        })
    if method == "tools/list":
        return ok(req_id, {"tools": list_tools()})
    if method == "tools/call":
        params = req.get("params") or {}
        name = params.get("name", "")
        if not name:
            return ok(req_id, {"content": [{"type": "text", "text": "missing tool name"}], "isError": True})
        return ok(req_id, call_tool(name, params.get("arguments") or {}))
    # Notifications (no id) — including notifications/initialized — get no response.
    if req_id is None:
        return None
    return err(req_id, -32601, "Method not found: %s" % method)


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception as e:
            log("parse error: %s" % e)
            sys.stdout.write(json.dumps({"jsonrpc": "2.0", "id": None,
                "error": {"code": -32700, "message": "Parse error"}}) + "\n")
            sys.stdout.flush()
            continue
        resp = handle(req)
        if resp is not None:
            sys.stdout.write(json.dumps(resp) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
```

- [ ] **Step 4: Run the test — confirm all PASS**

```bash
python3 -m unittest tests/mcp_adapter/test_openterminal_mcp.py -v 2>&1 | tail -12
# Expected: OK (7 tests).
```

- [ ] **Step 5: Neuter-check the safety-relevant assertions**
  - Temporarily make `call_tool` always return `isError: False` → `test_tools_call_error_maps_to_iserror` FAILS. Revert.
  - Temporarily have `handle` return `ok(...)` for unknown methods → `test_unknown_method_is_jsonrpc_error` FAILS. Revert. Re-run green.

- [ ] **Step 6: Commit**

```bash
chmod +x scripts/mcp/openterminal_mcp.py
git add scripts/mcp/openterminal_mcp.py tests/mcp_adapter/test_openterminal_mcp.py
git commit -m "feat(mcp): OpenTerminal->Claude Code MCP stdio adapter (read-only v1) + protocol tests"
```

---

### Task 2: README

**Files:** Create `scripts/mcp/README.md`

- [ ] **Step 1: Write the README**

````markdown
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
````

- [ ] **Step 2: Commit**

```bash
git add scripts/mcp/README.md
git commit -m "docs(mcp): adapter README + claude mcp add instructions"
```

---

### Task 3: Registration + live smoke — OPERATOR/CONTROLLER ONLY (not a subagent)

> A subagent must NOT run this. The controller does it in-session.

- [ ] **Step 1:** With the OpenTerminal app running, register:
```bash
claude mcp add openterminal --env OPENTERMINAL_CLI=/tmp/ot-build-ht/openterminalcli \
  -- python3 "$(pwd)/scripts/mcp/openterminal_mcp.py"
claude mcp list   # expect: openterminal listed / connected
```
- [ ] **Step 2:** In a fresh Claude Code turn, confirm `mcp__openterminal__get_ticker` is available and a live read (`{"symbol":"BTC/USD"}`) returns real data, and that `mcp__openterminal__crypto_submit_order` is **absent** (read-only scope holds).

---

## Self-Review (author checklist)

**Spec coverage:** stdio MCP server (Task 1 adapter) ✓; initialize/initialized/tools/list/tools/call/unknown-method (Task 1, all asserted) ✓; `$OPENTERMINAL_CLI` resolution + default (adapter `CLI=...`) ✓; pure passthrough, no execution allowlist (Task 1 `test_tools_list_passthrough_no_execution_tools`) ✓; success→content, `tool error:`→isError (Task 1 tests) ✓; terminal-not-running graceful (adapter `run_cli` returns rc!=0 → `tools/list` []=, `tools/call` isError; covered structurally — the FileNotFoundError/timeout branches) ✓; stdout-only-JSON-RPC (Task 1 `test_stdout_is_only_jsonrpc` + every `_run` parses every line) ✓; README + `claude mcp add` (Task 2) ✓; registration/live smoke operator-only (Task 3) ✓.

**Placeholder scan:** none — full code in every step.

**Type/name consistency:** `run_cli`→(rc,out,err) used consistently by `list_tools`/`call_tool`; `ok`/`err` response builders consistent; tool names in the no-execution assertion match the real gated tool names; `OPENTERMINAL_CLI` env name identical in adapter, test, and README.

> Note: the adapter's "terminal not running" path is covered by code structure + the error-mapping test, not a dedicated live test (no terminal in unit scope). The operator smoke (Task 3) covers the live path.
