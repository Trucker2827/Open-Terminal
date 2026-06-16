from __future__ import annotations

import argparse
from typing import Literal
from mcp.server.fastmcp import FastMCP

from trading_mcp.config import get_settings
from trading_mcp.safety import RiskManager
from trading_mcp.utils import setup_logging, tool_error
from trading_mcp.alpaca_tools import AlpacaService
from trading_mcp.coinbase_tools import CoinbaseService

settings = get_settings()
setup_logging(settings.log_level)
risk = RiskManager(settings)
alpaca = AlpacaService(settings, risk)
coinbase = CoinbaseService(settings, risk)

mcp = FastMCP("secure-trading-mcp")


@mcp.resource("trading://safety-policy")
def safety_policy() -> str:
    return f"""
Trading MCP safety policy:
- mode={settings.trading_mode}
- dry_run={settings.dry_run}
- require_confirmation={settings.require_confirmation}
- max_order_notional_usd={settings.max_order_notional_usd}
- max_position_notional_usd={settings.max_position_notional_usd}
- max_daily_order_count={settings.max_daily_order_count}
- options_enabled={settings.allow_options_trading}
- crypto_withdrawals_enabled={settings.allow_crypto_withdrawals}
""".strip()


@mcp.prompt()
def trade_review_prompt(symbol: str, side: str, quantity: float, venue: str) -> str:
    return f"Review the proposed {venue} trade: {side} {quantity} {symbol}. Check cash, positions, market data, max notional, and whether this is paper/dry-run before any placement."


@mcp.tool()
@tool_error
def get_account_balance(venue: Literal["alpaca", "coinbase"] = "alpaca") -> dict:
    """Get account balance/account summary from Alpaca or Coinbase."""
    return alpaca.get_account_balance() if venue == "alpaca" else coinbase.get_account_balance()


@mcp.tool()
@tool_error
def get_portfolio(venue: Literal["alpaca", "coinbase"] = "alpaca") -> dict:
    """Get positions/holdings from Alpaca or Coinbase."""
    return alpaca.get_portfolio() if venue == "alpaca" else coinbase.get_portfolio()


@mcp.tool()
@tool_error
def get_market_data(ticker: str, timeframe: str = "1Day", venue: Literal["alpaca", "coinbase"] = "alpaca", asset_class: str = "stock") -> dict:
    """Get latest/high-level market data. For Coinbase use product ids like BTC-USD."""
    return alpaca.get_market_data(ticker, timeframe, asset_class) if venue == "alpaca" else coinbase.get_market_data(ticker)


@mcp.tool()
@tool_error
def get_historical_data(ticker: str, timeframe: str = "1Day", venue: Literal["alpaca"] = "alpaca", asset_class: str = "stock", days: int = 30) -> dict:
    """Get historical bars from Alpaca for stocks, ETFs, options, or crypto."""
    return alpaca.get_historical_data(ticker, timeframe, asset_class, days)


@mcp.tool()
@tool_error
def place_order(
    symbol: str,
    side: Literal["buy", "sell"],
    quantity: float,
    type: Literal["market", "limit"] = "market",
    limit_price: float | None = None,
    venue: Literal["alpaca", "coinbase"] = "alpaca",
    asset_class: Literal["stock", "etf", "option", "crypto"] = "stock",
    time_in_force: str = "day",
    confirmation_token: str | None = None,
) -> dict:
    """Place or dry-run a guarded order. Live orders require env opt-in and confirmation_token."""
    if venue == "alpaca":
        return alpaca.place_order(symbol, side, quantity, type, limit_price, time_in_force, asset_class, confirmation_token)
    return coinbase.place_order(symbol, side, quantity, type, limit_price, confirmation_token)


@mcp.tool()
@tool_error
def cancel_order(order_id: str, venue: Literal["alpaca", "coinbase"] = "alpaca") -> dict:
    """Cancel an order. In DRY_RUN mode, reports what would be canceled."""
    return alpaca.cancel_order(order_id) if venue == "alpaca" else coinbase.cancel_order(order_id)


@mcp.tool()
@tool_error
def get_open_orders(venue: Literal["alpaca", "coinbase"] = "alpaca", product_id: str | None = None) -> dict:
    """Get currently open orders."""
    return alpaca.get_open_orders() if venue == "alpaca" else coinbase.get_open_orders(product_id)


@mcp.tool()
@tool_error
def analyze_positions(venue: Literal["alpaca"] = "alpaca") -> dict:
    """Analyze Alpaca positions, including absolute portfolio weights and unrealized P/L fields."""
    return alpaca.analyze_positions()


@mcp.tool()
@tool_error
def backtest_strategy(ticker: str, asset_class: str = "stock", days: int = 120, short_window: int = 5, long_window: int = 20) -> dict:
    """Simple moving-average crossover backtest using Alpaca historical daily bars."""
    return alpaca.backtest_strategy(ticker, asset_class, days, short_window, long_window)


@mcp.tool()
@tool_error
def coinbase_send_crypto(asset: str, amount: float, destination: str, network: str | None = None, confirmation_token: str | None = None) -> dict:
    """Guarded crypto withdrawal/transfer placeholder. Disabled unless explicitly enabled and backend implemented."""
    return coinbase.send_crypto(asset, amount, destination, network, confirmation_token)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--transport", default="stdio", choices=["stdio", "sse", "streamable-http"])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()

    # Official MCP SDK versions differ slightly by transport support. stdio is the safest default.
    if args.transport == "stdio":
        mcp.run(transport="stdio")
    else:
        mcp.run(transport=args.transport, host=args.host, port=args.port)


if __name__ == "__main__":
    main()
