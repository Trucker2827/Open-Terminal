# test_place_order.py — regression test for the place_order TOOL path (not just
# the lower-level RiskManager). Stubs the two SDK lookups (_estimate_price,
# _current_position_notional) so the real place_order flow runs with no API keys
# and no network. Locks four behaviors on both venues:
#   1. within-cap order -> dry-run preview with a sized notional
#   2. order-cap block
#   3. position-cap block on BUY + sell exemption + under-cap allowed
#   4. fail-closed: a market order with no resolvable price is blocked
import unittest

from trading_mcp.config import Settings
from trading_mcp.safety import RiskManager, TradingBlocked
from trading_mcp.alpaca_tools import AlpacaService
from trading_mcp.coinbase_tools import CoinbaseService


def _alpaca(order_cap=500.0, pos_cap=2500.0, position=None, price=None):
    s = Settings(trading_mode="paper", dry_run=True,
                 max_order_notional_usd=order_cap, max_position_notional_usd=pos_cap,
                 enable_alpaca=True, alpaca_api_key=None, alpaca_secret_key=None)
    svc = AlpacaService(s, RiskManager(s))
    svc._estimate_price = lambda symbol, asset_class: price           # stub SDK price lookup
    svc._current_position_notional = lambda symbol: position          # stub SDK position lookup
    return svc


def _coinbase(order_cap=500.0, pos_cap=2500.0, position=None, price=None):
    s = Settings(trading_mode="paper", dry_run=True,
                 max_order_notional_usd=order_cap, max_position_notional_usd=pos_cap,
                 enable_coinbase=True, coinbase_api_key=None, coinbase_api_secret=None)
    svc = CoinbaseService(s, RiskManager(s))
    svc._estimate_price = lambda product_id: price
    svc._current_position_notional = lambda product_id, p: position
    return svc


class TestAlpacaPlaceOrder(unittest.TestCase):
    def test_within_cap_limit_preview(self):
        r = _alpaca().place_order("AAPL", "buy", 2, "limit", limit_price=100)
        self.assertTrue(r["ok"])
        self.assertTrue(r["dry_run"])
        self.assertEqual(r["would_submit"]["estimated_notional_usd"], 200)

    def test_order_cap_block(self):
        with self.assertRaises(TradingBlocked):
            _alpaca().place_order("AAPL", "buy", 10, "limit", limit_price=100)  # 1000 > 500

    def test_position_cap_blocks_buy(self):
        # order $300 (< $500 order cap) but pushes $2300 -> $2600 (> $2500 position cap)
        with self.assertRaises(TradingBlocked):
            _alpaca(position=2300.0).place_order("AAPL", "buy", 3, "limit", limit_price=100)

    def test_position_cap_exempts_sell(self):
        r = _alpaca(position=2300.0).place_order("AAPL", "sell", 3, "limit", limit_price=100)
        self.assertTrue(r["ok"])

    def test_position_cap_under_limit_allows(self):
        r = _alpaca(position=2000.0).place_order("AAPL", "buy", 3, "limit", limit_price=100)  # 2300 < 2500
        self.assertTrue(r["ok"])

    def test_market_no_price_fails_closed(self):
        with self.assertRaises(TradingBlocked):
            _alpaca(price=None).place_order("AAPL", "buy", 1, "market")

    def test_market_with_price_is_sized_and_capped(self):
        r = _alpaca(price=100.0).place_order("AAPL", "buy", 2, "market")  # 200 <= 500
        self.assertEqual(r["would_submit"]["estimated_notional_usd"], 200)
        with self.assertRaises(TradingBlocked):  # 1000 > 500: market order IS now capped
            _alpaca(price=100.0).place_order("AAPL", "buy", 10, "market")


class TestCoinbasePlaceOrder(unittest.TestCase):
    def test_within_cap_limit_preview(self):
        r = _coinbase().place_order("BTC-USD", "buy", 2, "limit", limit_price=100)
        self.assertTrue(r["ok"])
        self.assertEqual(r["would_submit"]["estimated_notional_usd"], 200)

    def test_order_cap_block(self):
        with self.assertRaises(TradingBlocked):
            _coinbase().place_order("BTC-USD", "buy", 1, "limit", limit_price=1000)  # 1000 > 500

    def test_market_no_price_fails_closed(self):
        with self.assertRaises(TradingBlocked):
            _coinbase(price=None).place_order("BTC-USD", "buy", 0.01, "market")

    def test_market_with_price_is_sized_and_capped(self):
        r = _coinbase(price=100.0).place_order("BTC-USD", "buy", 2, "market")  # 200 <= 500
        self.assertEqual(r["would_submit"]["estimated_notional_usd"], 200)
        with self.assertRaises(TradingBlocked):
            _coinbase(price=100.0).place_order("BTC-USD", "buy", 10, "market")  # 1000 > 500


class TestMcpToolSchemas(unittest.TestCase):
    """Guards the @tool_error signature-preservation regression: every tool must
    advertise its REAL parameters, not the wrapper's (*args, **kwargs)."""

    def _schemas(self):
        import asyncio
        import server
        return {t.name: (t.inputSchema or {}) for t in asyncio.run(server.mcp.list_tools())}

    def test_place_order_exposes_named_params(self):
        props = (self._schemas()["place_order"].get("properties") or {})
        for p in ("symbol", "side", "quantity", "type", "limit_price", "venue"):
            self.assertIn(p, props)
        self.assertNotIn("args", props)
        self.assertNotIn("kwargs", props)

    def test_no_tool_leaks_varargs_schema(self):
        for name, schema in self._schemas().items():
            props = schema.get("properties") or {}
            self.assertNotIn("args", props, f"{name} leaked *args into its schema")
            self.assertNotIn("kwargs", props, f"{name} leaked **kwargs into its schema")


if __name__ == "__main__":
    unittest.main()
