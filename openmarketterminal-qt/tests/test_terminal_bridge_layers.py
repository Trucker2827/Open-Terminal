import json
import os
import sqlite3
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts"))
sys.path.insert(0, os.path.join(SCRIPTS, "ai_quant_lab"))
import terminal_data as td
import terminal_bridge as tb


def make_tick_db(path, symbol="BTC", start_ms=1_000_000, minutes=120, base=65000.0):
    conn = sqlite3.connect(path)
    conn.execute("""CREATE TABLE edge_prediction_raw_ticks (
        id TEXT PRIMARY KEY, symbol TEXT NOT NULL DEFAULT '',
        source TEXT NOT NULL DEFAULT '', price REAL NOT NULL DEFAULT 0,
        exchange_ts INTEGER NOT NULL DEFAULT 0, received_ts INTEGER NOT NULL DEFAULT 0)""")
    rows = []
    for i in range(minutes):
        ts = start_ms + i * 60_000
        rows.append((f"t{i}a", symbol, "coinbase_ws", base + i, ts, ts))
        rows.append((f"t{i}b", symbol, "kraken_ws", base + i + 1.0, ts + 1000, ts + 1000))
        rows.append((f"t{i}x", symbol, "gemini_ws", 1.0, ts, ts))   # excluded source
    conn.executemany("INSERT INTO edge_prediction_raw_ticks VALUES (?,?,?,?,?,?)", rows)
    conn.commit()
    conn.close()


class TerminalDataTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db = os.path.join(self.tmp.name, "t.db")
        make_tick_db(self.db)
        os.environ["OPENTERMINAL_DATA_DB"] = self.db
        os.environ["OPENTERMINAL_EVIDENCE_DIR"] = self.tmp.name

    def tearDown(self):
        os.environ.pop("OPENTERMINAL_DATA_DB", None)
        os.environ.pop("OPENTERMINAL_EVIDENCE_DIR", None)
        self.tmp.cleanup()

    def test_symbol_normalization(self):
        for raw in ("BTC-USD", "btc-usd", "BTC/USD", "BTCUSD", "BTC"):
            self.assertEqual(td.normalize_symbol(raw), "BTC")

    def test_spot_series_resamples_and_filters_sources(self):
        now = 1_000_000 + 120 * 60_000
        series = td.spot_series("BTC-USD", window_hours=3, resample_sec=60, now_ms=now)
        self.assertEqual(len(series), 120)
        # gemini decoy price (1.0) excluded: bucket average is coinbase/kraken only
        self.assertAlmostEqual(series[0][1], 65000.5)
        self.assertLess(series[0][0], series[-1][0])

    def test_spot_returns_are_log_by_default(self):
        now = 1_000_000 + 120 * 60_000
        returns = td.spot_returns("BTC", window_hours=3, now_ms=now)
        self.assertEqual(len(returns), 119)
        self.assertTrue(all(abs(r) < 0.001 for _, r in returns))

    def test_resolver_fills_values(self):
        now = 1_000_000 + 120 * 60_000
        data = {"source": "terminal", "symbol": "BTC-USD", "window_hours": 3}
        # resolve uses wall clock; inject via spot_series' now param by patching
        td_now = td.spot_series
        try:
            td.spot_series = lambda s, w, r, n=None: td_now(s, w, r, now)
            td.resolve_terminal_series(data)
        finally:
            td.spot_series = td_now
        self.assertEqual(len(data["values"]), 120)
        self.assertEqual(data["_terminal_series"]["symbol"], "BTC")

    def test_resolver_fails_honestly_on_empty(self):
        data = {"source": "terminal", "symbol": "DOGE", "window_hours": 1}
        with self.assertRaisesRegex(ValueError, "only 0 points"):
            td.resolve_terminal_series(data)

    def test_resolver_passthrough_when_not_terminal(self):
        data = {"values": [1, 2, 3]}
        self.assertEqual(td.resolve_terminal_series(data), data)

    def test_evidence_readers_return_none_when_absent(self):
        self.assertIsNone(td.kalshi_evidence())
        self.assertIsNone(td.calibrator_report())
        with open(os.path.join(self.tmp.name, "calibrator.json"), "w") as fh:
            json.dump({"schema": 1}, fh)
        self.assertEqual(td.calibrator_report(), {"schema": 1})


class FakeBridgeHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.headers.get("X-MCP-Token") != "tok123":
            payload = {"success": False, "error": "bad token"}
        elif body["tool"] == "get_app_info":
            payload = {"success": True, "data": {"echo_args": body["args"]}}
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


class TerminalBridgeTest(unittest.TestCase):
    def setUp(self):
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

    def test_call_tool_round_trips(self):
        r = tb.call_tool("get_app_info", {"x": 1})
        self.assertEqual(r["data"]["echo_args"], {"x": 1})

    def test_tool_failure_raises(self):
        with self.assertRaisesRegex(RuntimeError, "unknown tool"):
            tb.call_tool("nope")

    def test_missing_bridge_file_is_unavailable(self):
        os.environ["OPENTERMINAL_BRIDGE_JSON"] = os.path.join(self.tmp.name, "absent.json")
        self.assertFalse(tb.bridge_available())
        with self.assertRaises(tb.BridgeUnavailable):
            tb.call_tool("get_app_info")


if __name__ == "__main__":
    unittest.main()
