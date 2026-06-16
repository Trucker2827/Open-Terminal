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
