"""Hermetic tests for the HFT module's 'terminal' exchange source (#82).

Runs qlib_high_frequency's analyze path against a fake bridge HTTP server
serving a fixture order book — no app, no network beyond loopback, no ccxt.
"""
import contextlib
import io
import json
import os
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts"))
sys.path.insert(0, os.path.join(SCRIPTS, "ai_quant_lab"))
import qlib_high_frequency as hft
import terminal_bridge as tb

# Fixture book: best bid 100.0, best ask 100.2 (bids sent worst-first on
# purpose — the module must sort best-first before computing metrics).
FIXTURE_BOOK = {
    "symbol": "BTC/USD",
    "best_bid": 100.0,
    "best_ask": 100.2,
    "spread": 0.2,
    "spread_pct": 0.2,
    "bids": [{"price": 99.5, "amount": 3.0},
             {"price": 100.0, "amount": 2.0},
             {"price": 99.8, "amount": 1.5}],
    "asks": [{"price": 100.5, "amount": 2.5},
             {"price": 100.2, "amount": 1.0},
             {"price": 100.4, "amount": 4.0}],
}


class FakeBridgeHandler(BaseHTTPRequestHandler):
    requests_seen = []

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        FakeBridgeHandler.requests_seen.append(body)
        if self.headers.get("X-MCP-Token") != "tok123":
            payload = {"success": False, "error": "bad token"}
        elif body["tool"] == "get_order_book":
            payload = {"success": True, "data": FIXTURE_BOOK}
        else:
            payload = {"success": False, "error": "unknown tool"}
        raw = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, *args):
        pass


def run_analyze(params):
    """Invoke cmd_analyze and parse the single JSON object it prints."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        hft.cmd_analyze(params)
    return json.loads(out.getvalue())


class HftTerminalSourceTest(unittest.TestCase):
    def setUp(self):
        FakeBridgeHandler.requests_seen = []
        self.tmp = tempfile.TemporaryDirectory()
        self.server = HTTPServer(("127.0.0.1", 0), FakeBridgeHandler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.bridge_json = os.path.join(self.tmp.name, "bridge.json")
        with open(self.bridge_json, "w") as fh:
            json.dump({"endpoint": f"http://127.0.0.1:{self.server.server_port}",
                       "token": "tok123"}, fh)
        os.environ["OPENTERMINAL_BRIDGE_JSON"] = self.bridge_json

    def tearDown(self):
        os.environ.pop("OPENTERMINAL_BRIDGE_JSON", None)
        self.server.shutdown()
        self.thread.join()
        self.tmp.cleanup()

    def test_analyze_terminal_computes_metrics_from_bridge_book(self):
        r = run_analyze({"exchange": "terminal", "symbol": "BTC-USD"})
        self.assertTrue(r["success"], r)
        self.assertEqual(r["exchange"], "terminal")
        m = r["book_metrics"]
        # Best-first even though the fixture bids arrived unsorted.
        self.assertEqual(m["best_bid"], 100.0)
        self.assertEqual(m["best_ask"], 100.2)
        self.assertAlmostEqual(m["mid_price"], 100.1)
        self.assertAlmostEqual(m["spread"], 0.2)
        self.assertAlmostEqual(m["bid_volume"], 6.5)
        self.assertAlmostEqual(m["ask_volume"], 7.5)
        # Market making quotes derive from the same fixture book.
        self.assertNotIn("error", r["market_making"])
        self.assertLess(r["market_making"]["bid_price"], r["market_making"]["ask_price"])
        # Slippage walk: buy 1.0 fills entirely at best ask 100.2.
        self.assertAlmostEqual(r["slippage"]["average_price"], 100.2)

    def test_analyze_terminal_maps_symbol_to_pair(self):
        run_analyze({"exchange": "terminal", "symbol": "BTC-USD"})
        book_reqs = [b for b in FakeBridgeHandler.requests_seen
                     if b["tool"] == "get_order_book"]
        self.assertEqual(len(book_reqs), 1)
        self.assertEqual(book_reqs[0]["args"]["symbol"], "BTC/USD")
        self.assertEqual(book_reqs[0]["args"]["limit"], 20)

    def test_analyze_terminal_reports_toxic_flow_as_unavailable(self):
        r = run_analyze({"exchange": "terminal", "symbol": "BTC-USD"})
        self.assertTrue(r["success"], r)
        self.assertIn("error", r["toxic_flow"])
        self.assertIn("no public trade feed", r["toxic_flow"]["error"])
        self.assertEqual(r["trade_count"], 0)

    def test_analyze_terminal_honest_error_when_bridge_down(self):
        os.environ["OPENTERMINAL_BRIDGE_JSON"] = os.path.join(self.tmp.name, "absent.json")
        r = run_analyze({"exchange": "terminal", "symbol": "BTC-USD"})
        self.assertFalse(r["success"])
        self.assertIn("terminal bridge unavailable", r["error"])
        self.assertIn("is the app running?", r["error"])

    def test_analyze_terminal_honest_error_on_tool_failure(self):
        # Wrong token → bridge answers success:false → RuntimeError path.
        with open(self.bridge_json, "w") as fh:
            json.dump({"endpoint": f"http://127.0.0.1:{self.server.server_port}",
                       "token": "wrong"}, fh)
        r = run_analyze({"exchange": "terminal", "symbol": "BTC-USD"})
        self.assertFalse(r["success"])
        self.assertIn("terminal bridge order book fetch failed", r["error"])

    def test_non_terminal_exchange_does_not_touch_bridge(self):
        # Stub the ccxt factory so no real network is used; the bridge must
        # still see zero requests on the non-terminal path.
        if hft._CCXT_AVAILABLE:
            def _no_network(*a, **k):
                raise RuntimeError("offline test — ccxt disabled")
            orig = hft.make_exchange
            hft.make_exchange = _no_network
            try:
                run_analyze({"exchange": "coinbase", "symbol": "BTC/USD"})
            finally:
                hft.make_exchange = orig
        else:
            run_analyze({"exchange": "coinbase", "symbol": "BTC/USD"})
        self.assertEqual(FakeBridgeHandler.requests_seen, [])


if __name__ == "__main__":
    unittest.main()
