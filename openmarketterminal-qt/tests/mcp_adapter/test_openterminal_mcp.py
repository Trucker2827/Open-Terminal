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

    def test_tools_list_has_read_tools_no_execution(self):
        objs, _, _ = self._run([{"jsonrpc": "2.0", "id": 2, "method": "tools/list"}])
        names = [t["name"] for t in objs[0]["result"]["tools"]]
        # Passthrough catalog tool present, AND the explicit read tools the capped
        # catalog would drop are surfaced via the allowlist.
        for r in ("get_ticker", "get_order_book", "get_candles", "get_exchange_info",
                  "get_crypto_balance", "get_crypto_open_orders", "get_crypto_trades"):
            self.assertIn(r, names)
        # get_ticker is in BOTH the stub catalog and the allowlist → must appear once.
        self.assertEqual(names.count("get_ticker"), 1)
        # READ-ONLY: no execution tools advertised.
        for ex in ("crypto_submit_order", "crypto_cancel_order", "fast_submit_order", "cancel_order"):
            self.assertNotIn(ex, names)

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
