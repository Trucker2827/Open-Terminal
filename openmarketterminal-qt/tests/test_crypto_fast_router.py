#!/usr/bin/env python3
import json
import pathlib
import sys
import types
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "exchange"))

if "ccxt" not in sys.modules:
    fake_ccxt = types.ModuleType("ccxt")
    fake_ccxt.exchanges = []
    for name in ("AuthenticationError", "InsufficientFunds", "InvalidOrder",
                 "OrderNotFound", "RateLimitExceeded", "NetworkError",
                 "ExchangeNotAvailable", "ExchangeError"):
        setattr(fake_ccxt, name, type(name, (Exception,), {}))
    sys.modules["ccxt"] = fake_ccxt
if "exchange_client" not in sys.modules:
    fake_client = types.ModuleType("exchange_client")
    fake_client.load_cached_markets = lambda _exchange: None
    fake_client.save_markets_cache = lambda _exchange, _markets: None
    fake_client.get_default_type = lambda _exchange: "spot"
    sys.modules["exchange_client"] = fake_client

import exchange_daemon


class FakeWs:
    connected = True

    def __init__(self):
        self.sent = []

    def send(self, payload):
        self.sent.append(json.loads(payload))

    def recv(self):
        request = self.sent[-1]
        return json.dumps({
            "method": "add_order",
            "req_id": request["req_id"],
            "success": True,
            "result": {
                "order_id": "ORDER-1",
                "cl_ord_id": request["params"].get("cl_ord_id"),
            },
            "time_in": "2026-07-23T12:00:00.000Z",
            "time_out": "2026-07-23T12:00:00.004Z",
        })

    def close(self):
        self.connected = False


class FastRouterTest(unittest.TestCase):
    def test_kraken_native_payload_carries_staleness_and_idempotency(self):
        router = exchange_daemon._KrakenNativeOrderRouter("key", "c2VjcmV0")
        router.ws = FakeWs()
        router.token = "token"
        result = router.place({
            "symbol": "BTC/USD",
            "side": "buy",
            "type": "limit",
            "amount": 0.01,
            "price": 65000,
            "time_in_force": "IOC",
            "client_order_id": "scalp-unique-123456789",
            "deadline_ms": 750,
        })
        payload = router.ws.sent[0]
        self.assertEqual(payload["method"], "add_order")
        params = payload["params"]
        self.assertEqual(params["time_in_force"], "ioc")
        self.assertTrue(params["deadline"].endswith("Z"))
        self.assertEqual(params["cl_ord_id"], "scalp-unique-123456789"[:18])
        self.assertEqual(result["adapter"], "kraken_spot_websocket_v2")
        self.assertEqual(result["deadline_ms"], 750)
        self.assertGreaterEqual(result["order_ack_latency_ms"], 0)

    def test_deadline_is_bounded(self):
        router = exchange_daemon._KrakenNativeOrderRouter("key", "c2VjcmV0")
        router.ws = FakeWs()
        router.token = "token"
        low = router.place({
            "symbol": "ETH/USD", "side": "sell", "type": "market",
            "amount": 0.1, "deadline_ms": 10,
        })
        self.assertEqual(low["deadline_ms"], 500)


if __name__ == "__main__":
    unittest.main()
