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
    def test_authenticated_get_retries_rate_limit_with_fresh_signature(self):
        limited = Mock(status_code=429, headers={"Retry-After": "0"})
        limited.json.return_value = {"error": "rate limited"}
        success = Mock(status_code=200, headers={})
        success.json.return_value = {"balance": 100}
        requests_module = Mock()
        requests_module.request.side_effect = [limited, success]
        with patch.dict(sys.modules, {"requests": requests_module}), \
             patch.object(MODULE, "_auth_headers", side_effect=[{"sig": "one"}, {"sig": "two"}]) as auth, \
             patch.object(MODULE.time, "sleep") as sleep:
            ok, code, data = MODULE._request(
                {"use_demo": True}, "GET", "/portfolio/balance")

        self.assertTrue(ok)
        self.assertEqual(code, 200)
        self.assertEqual(data["balance"], 100)
        self.assertEqual(requests_module.request.call_count, 2)
        self.assertEqual(auth.call_count, 2)
        sleep.assert_called_once()

    def test_non_idempotent_post_is_not_retried(self):
        failed = Mock(status_code=503, headers={})
        failed.json.return_value = {"error": "unavailable"}
        requests_module = Mock()
        requests_module.request.return_value = failed
        with patch.dict(sys.modules, {"requests": requests_module}), \
             patch.object(MODULE, "_auth_headers", return_value={"sig": "one"}), \
             patch.object(MODULE.time, "sleep") as sleep:
            ok, code, _ = MODULE._request(
                {"use_demo": True}, "POST", "/portfolio/orders", body={"ticker": "KX"})

        self.assertFalse(ok)
        self.assertEqual(code, 503)
        self.assertEqual(requests_module.request.call_count, 1)
        sleep.assert_not_called()

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
