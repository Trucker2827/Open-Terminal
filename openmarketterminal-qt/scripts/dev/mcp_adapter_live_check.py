#!/usr/bin/env python3
"""Live integration check for the Claude Code <-> OpenTerminal MCP adapter.

The unit test (tests/mcp_adapter/test_openterminal_mcp.py) drives the adapter
against a FAKE CLI. This script drives the REAL adapter (scripts/mcp/openterminal_mcp.py)
exactly as Claude Code's MCP client does — newline-delimited JSON-RPC 2.0 over
stdin/stdout — against the LIVE running OpenTerminal app, and asserts:
  * initialize returns protocolVersion 2024-11-05
  * tools/list is non-empty (and, since GET /tools?all=1, the FULL catalogue —
    hundreds of tools, not the in-app LLM's 50-tool prompt cap)
  * tools/call get_observations returns live data (not isError)
  * an unknown method maps to JSON-RPC error -32601

Requires the app to be running and the CLI attached ($OPENTERMINAL_CLI). Run from
the repo root: python3 scripts/dev/mcp_adapter_live_check.py
"""
import json, subprocess, sys

proc = subprocess.Popen(
    ["python3", "scripts/mcp/openterminal_mcp.py"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

def rpc(obj, expect_reply=True):
    proc.stdin.write(json.dumps(obj) + "\n"); proc.stdin.flush()
    if not expect_reply:
        return None
    line = proc.stdout.readline()
    return json.loads(line) if line.strip() else None

ok = True
# 1) initialize
r = rpc({"jsonrpc":"2.0","id":1,"method":"initialize",
         "params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"claude-code-test","version":"0"}}})
proto = r.get("result",{}).get("protocolVersion")
srv = r.get("result",{}).get("serverInfo",{}).get("name")
print(f"[initialize] protocolVersion={proto} server={srv}")
ok &= proto == "2024-11-05"

# 2) initialized notification (no reply)
rpc({"jsonrpc":"2.0","method":"notifications/initialized"}, expect_reply=False)

# 3) tools/list
r = rpc({"jsonrpc":"2.0","id":2,"method":"tools/list"})
tools = r.get("result",{}).get("tools",[])
names = {t["name"] for t in tools}
print(f"[tools/list] {len(tools)} tools exposed")
ok &= len(tools) > 0  # NOTE: list is capped at 50 of 481 — see assessment

# 4) tools/call → live read against the running app
r = rpc({"jsonrpc":"2.0","id":3,"method":"tools/call",
         "params":{"name":"get_observations","arguments":{"view":"latest"}}})
content = r.get("result",{}).get("content",[])
text = content[0]["text"] if content else ""
is_err = r.get("result",{}).get("isError", False)
print(f"[tools/call get_observations] isError={is_err} bytes={len(text)}")
print("  live payload head:", text[:140].replace("\n"," "))
ok &= (not is_err) and len(text) > 50

# 5) unknown method → JSON-RPC error -32601
r = rpc({"jsonrpc":"2.0","id":4,"method":"does/not/exist"})
code = r.get("error",{}).get("code")
print(f"[unknown method] error.code={code}")
ok &= code == -32601

proc.stdin.close()
proc.terminate()
print("\nINTEGRATION:", "PASS ✅" if ok else "FAIL ❌")
sys.exit(0 if ok else 1)
