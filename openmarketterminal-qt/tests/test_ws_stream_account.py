"""Unit tests for ws_stream.py's authenticated account mode (Task 6).

Pure message-builder + command-surface tests — no network, no ccxt import
needed at test time (builders are plain functions).

Run from openmarketterminal-qt/: python3 -m unittest tests.test_ws_stream_account -v
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts", "exchange"))
import ws_stream  # noqa: E402


class TestAccountMode(unittest.TestCase):
    def test_order_message_shape(self):
        order = {
            "id": "o1", "symbol": "BTC/USD", "side": "buy", "type": "limit",
            "price": 118000.0, "amount": 0.001, "filled": 0.0,
            "remaining": 0.001, "status": "open", "timestamp": 1753000000000,
            "info": {"huge_raw_payload": "must not pass through"},
        }
        msg = ws_stream.make_account_order_msg(order)
        self.assertEqual(msg["type"], "account_order")
        self.assertEqual(msg["order"]["id"], "o1")
        self.assertEqual(msg["order"]["status"], "open")
        self.assertEqual(msg["order"]["symbol"], "BTC/USD")
        self.assertNotIn("info", msg["order"])  # raw exchange payload filtered

    def test_mytrade_message_shape(self):
        trade = {"id": "t1", "order": "o1", "symbol": "BTC/USD", "side": "buy",
                 "price": 118000.0, "amount": 0.001, "cost": 118.0,
                 "timestamp": 1753000000000, "info": {}}
        msg = ws_stream.make_account_mytrade_msg(trade)
        self.assertEqual(msg["type"], "account_mytrade")
        self.assertEqual(msg["trade"]["order"], "o1")
        self.assertNotIn("info", msg["trade"])

    def test_balance_message_filters_zero(self):
        bal = {"USD": {"free": 100.0, "used": 0.0, "total": 100.0},
               "ETH": {"free": 0.0, "used": 0.0, "total": 0.0},
               "free": {"USD": 100.0},  # ccxt puts index dicts beside currencies
               "info": {"raw": True}}
        msg = ws_stream.make_account_balance_msg(bal)
        self.assertEqual(msg["type"], "account_balance")
        self.assertIn("USD", msg["balances"])
        self.assertNotIn("ETH", msg["balances"])   # zero balances filtered (daemon parity)
        self.assertNotIn("free", msg["balances"])  # non-currency index keys filtered
        self.assertNotIn("info", msg["balances"])

    def test_credentials_never_leave_process_boundary(self):
        # set_account_credentials must be a recognized stdin command constant,
        # and the module must not read account creds from argv or the env.
        self.assertIn("set_account_credentials", ws_stream.ACCOUNT_CMDS)
        src_path = os.path.join(os.path.dirname(__file__), "..", "scripts", "exchange", "ws_stream.py")
        with open(src_path) as f:
            src = f.read()
        self.assertNotIn("ACCOUNT_API_KEY", src)  # no env-var creds path
        self.assertNotIn("os.environ[\"API", src)

    def test_normalize_pem_matches_daemon(self):
        pem = "-----BEGIN EC PRIVATE KEY-----\\nabc\\n-----END EC PRIVATE KEY-----\\n"
        fixed = ws_stream._normalize_pem(pem)
        self.assertIn("\n", fixed)
        self.assertNotIn("\\n", fixed)
        # non-PEM secrets untouched
        self.assertEqual(ws_stream._normalize_pem("plainbase64=="), "plainbase64==")


if __name__ == "__main__":
    unittest.main()
