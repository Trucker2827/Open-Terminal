import importlib.util
import io
import pathlib
import sys
import unittest
from unittest.mock import Mock, patch


SCRIPT_DIR = pathlib.Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPT_DIR))
SPEC = importlib.util.spec_from_file_location(
    "prediction_kalshi", SCRIPT_DIR / "prediction_kalshi.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class QueuePositionsTest(unittest.TestCase):
    def test_stdin_payload_mode_does_not_need_payload_in_argv(self):
        emitted = []
        handler = Mock()
        with patch.dict(MODULE.COMMANDS, {"fee_quote": handler}), \
             patch.object(MODULE, "_emit", emitted.append), \
             patch.object(sys, "argv", ["prediction_kalshi.py", "fee_quote", "--stdin-json"]), \
             patch.object(sys, "stdin", io.StringIO('{"price": 0.50, "count": 1}')):
            self.assertEqual(MODULE.main(), 0)

        handler.assert_called_once_with({"price": 0.50, "count": 1})
        self.assertEqual(emitted, [])

    def test_discovers_resting_markets_and_joins_order(self):
        requests = []

        def fake_request(payload, method, path, params=None, body=None):
            requests.append((method, path, params))
            if path == "/portfolio/orders":
                return True, 200, {"orders": [{
                    "order_id": "order-1",
                    "ticker": "KXBTC-TEST",
                    "side": "yes",
                    "action": "buy",
                    "yes_price_dollars": "0.4200",
                    "remaining_count_fp": "3.00",
                }]}
            return True, 200, {"queue_positions": [{
                "order_id": "order-1",
                "market_ticker": "KXBTC-TEST",
                "queue_position_fp": "7.00",
            }]}

        emitted = []
        with patch.object(MODULE, "_request", fake_request), \
             patch.object(MODULE, "_emit", emitted.append):
            MODULE.cmd_queue_positions({"subaccount": 0})

        self.assertEqual(requests[1][2]["market_tickers"], "KXBTC-TEST")
        row = emitted[0]["queue_positions"][0]
        self.assertEqual(row["side"], "YES")
        self.assertEqual(row["price"], 0.42)
        self.assertEqual(row["remaining"], 3.0)
        self.assertEqual(row["queue_position"], 7.0)
        self.assertEqual(row["readiness"], "NEAR")

    def test_without_resting_orders_skips_bulk_request(self):
        requests = []

        def fake_request(payload, method, path, params=None, body=None):
            requests.append(path)
            return True, 200, {"orders": []}

        emitted = []
        with patch.object(MODULE, "_request", fake_request), \
             patch.object(MODULE, "_emit", emitted.append):
            MODULE.cmd_queue_positions({})

        self.assertEqual(requests, ["/portfolio/orders"])
        self.assertEqual(
            emitted,
            [{"ok": True, "queue_positions": [], "resting_orders": 0}])


if __name__ == "__main__":
    unittest.main()
