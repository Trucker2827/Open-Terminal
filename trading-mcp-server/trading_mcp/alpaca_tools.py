from __future__ import annotations

from datetime import datetime, timedelta, timezone
from typing import Any, Literal
from uuid import UUID

from .config import Settings
from .safety import RiskManager
from .utils import audit, json_safe


class AlpacaService:
    def __init__(self, settings: Settings, risk: RiskManager):
        self.settings = settings
        self.risk = risk
        self._trading = None
        self._stock_data = None
        self._crypto_data = None
        self._option_data = None

    def _require_enabled(self) -> None:
        if not self.settings.enable_alpaca:
            raise RuntimeError("Alpaca disabled by ENABLE_ALPACA=false")
        if not self.settings.alpaca_api_key or not self.settings.alpaca_secret_key:
            raise RuntimeError("Alpaca credentials missing")

    @property
    def trading(self):
        self._require_enabled()
        if self._trading is None:
            from alpaca.trading.client import TradingClient
            self._trading = TradingClient(
                self.settings.alpaca_api_key,
                self.settings.alpaca_secret_key,
                paper=(self.settings.trading_mode == "paper" or self.settings.alpaca_paper),
            )
        return self._trading

    @property
    def stock_data(self):
        self._require_enabled()
        if self._stock_data is None:
            from alpaca.data.historical import StockHistoricalDataClient
            self._stock_data = StockHistoricalDataClient(self.settings.alpaca_api_key, self.settings.alpaca_secret_key)
        return self._stock_data

    @property
    def crypto_data(self):
        self._require_enabled()
        if self._crypto_data is None:
            from alpaca.data.historical import CryptoHistoricalDataClient
            self._crypto_data = CryptoHistoricalDataClient(self.settings.alpaca_api_key, self.settings.alpaca_secret_key)
        return self._crypto_data

    @property
    def option_data(self):
        self._require_enabled()
        if self._option_data is None:
            from alpaca.data.historical import OptionHistoricalDataClient
            self._option_data = OptionHistoricalDataClient(self.settings.alpaca_api_key, self.settings.alpaca_secret_key)
        return self._option_data

    def get_account_balance(self) -> dict[str, Any]:
        self.risk.check_read()
        account = self.trading.get_account()
        return {"ok": True, "venue": "alpaca", "account": json_safe(account)}

    def get_portfolio(self) -> dict[str, Any]:
        self.risk.check_read()
        positions = self.trading.get_all_positions()
        return {"ok": True, "venue": "alpaca", "positions": json_safe(positions)}

    def get_open_orders(self) -> dict[str, Any]:
        self.risk.check_read()
        from alpaca.trading.requests import GetOrdersRequest
        from alpaca.trading.enums import QueryOrderStatus
        orders = self.trading.get_orders(filter=GetOrdersRequest(status=QueryOrderStatus.OPEN))
        return {"ok": True, "venue": "alpaca", "orders": json_safe(orders)}

    def cancel_order(self, order_id: str) -> dict[str, Any]:
        self.risk.check_read()
        UUID(order_id)  # Validate shape early.
        if self.settings.dry_run:
            return {"ok": True, "dry_run": True, "message": "Alpaca cancel skipped because DRY_RUN=true", "order_id": order_id}
        result = self.trading.cancel_order_by_id(order_id)
        audit("alpaca.cancel_order", {"order_id": order_id, "result": json_safe(result)})
        return {"ok": True, "venue": "alpaca", "result": json_safe(result)}

    def place_order(
        self,
        symbol: str,
        side: Literal["buy", "sell"],
        quantity: float,
        type: Literal["market", "limit"] = "market",
        limit_price: float | None = None,
        time_in_force: str = "day",
        asset_class: Literal["stock", "etf", "option", "crypto"] = "stock",
        confirmation_token: str | None = None,
    ) -> dict[str, Any]:
        estimated_price = limit_price if limit_price is not None else self._estimate_price(symbol, asset_class)
        risk_report = self.risk.check_trade(
            venue="alpaca", symbol=symbol, side=side, quantity=quantity,
            estimated_price=estimated_price, confirmation_token=confirmation_token, asset_class=asset_class,
            current_position_notional=self._current_position_notional(symbol),
        )
        if self.settings.dry_run:
            return {"ok": True, "dry_run": True, "would_submit": risk_report}

        from alpaca.trading.enums import OrderSide, TimeInForce
        from alpaca.trading.requests import MarketOrderRequest, LimitOrderRequest
        order_side = OrderSide.BUY if side == "buy" else OrderSide.SELL
        tif = TimeInForce(time_in_force)
        if type == "market":
            req = MarketOrderRequest(symbol=symbol, qty=quantity, side=order_side, time_in_force=tif)
        elif type == "limit" and limit_price is not None:
            req = LimitOrderRequest(symbol=symbol, qty=quantity, side=order_side, time_in_force=tif, limit_price=limit_price)
        else:
            raise ValueError("limit orders require limit_price")
        order = self.trading.submit_order(order_data=req)
        audit("alpaca.place_order", {"risk": risk_report, "order": json_safe(order)})
        return {"ok": True, "venue": "alpaca", "order": json_safe(order), "risk": risk_report}

    def get_market_data(self, ticker: str, timeframe: str = "1Day", asset_class: str = "stock") -> dict[str, Any]:
        return self.get_historical_data(ticker, timeframe=timeframe, asset_class=asset_class, days=5)

    def get_historical_data(self, ticker: str, timeframe: str = "1Day", asset_class: str = "stock", days: int = 30) -> dict[str, Any]:
        self.risk.check_read()
        from alpaca.data.timeframe import TimeFrame
        end = datetime.now(timezone.utc)
        start = end - timedelta(days=days)
        tf = self._timeframe(timeframe)
        if asset_class == "crypto":
            from alpaca.data.requests import CryptoBarsRequest
            bars = self.crypto_data.get_crypto_bars(CryptoBarsRequest(symbol_or_symbols=[ticker], timeframe=tf, start=start, end=end))
        elif asset_class == "option":
            from alpaca.data.requests import OptionBarsRequest
            bars = self.option_data.get_option_bars(OptionBarsRequest(symbol_or_symbols=[ticker], timeframe=tf, start=start, end=end))
        else:
            from alpaca.data.requests import StockBarsRequest
            bars = self.stock_data.get_stock_bars(StockBarsRequest(symbol_or_symbols=[ticker], timeframe=tf, start=start, end=end))
        return {"ok": True, "venue": "alpaca", "ticker": ticker, "bars": json_safe(bars)}

    def analyze_positions(self) -> dict[str, Any]:
        portfolio = self.get_portfolio()["positions"]
        rows = []
        total_abs = 0.0
        for p in portfolio:
            mv = float(p.get("market_value") or 0)
            total_abs += abs(mv)
            rows.append({
                "symbol": p.get("symbol"),
                "qty": p.get("qty"),
                "market_value": mv,
                "unrealized_pl": p.get("unrealized_pl"),
                "unrealized_plpc": p.get("unrealized_plpc"),
            })
        for r in rows:
            r["portfolio_weight_abs"] = abs(r["market_value"]) / total_abs if total_abs else 0
        return {"ok": True, "venue": "alpaca", "total_abs_market_value": total_abs, "positions": rows}

    @staticmethod
    def backtest_strategy_from_bars(bars: list[dict[str, Any]], short_window: int = 5, long_window: int = 20,
                                    fee_bps: float = 0.0) -> dict[str, Any]:
        closes = [float(b["close"]) for b in bars if b.get("close") is not None]
        if len(closes) < long_window + 2:
            return {"ok": False, "error": "not enough bars"}
        fee = max(0.0, fee_bps) / 10000.0  # per-side transaction cost (e.g. 50 bps = 0.50%)
        cash, position = 1.0, 0.0
        trades = []
        for i in range(long_window, len(closes)):
            short_ma = sum(closes[i - short_window:i]) / short_window
            long_ma = sum(closes[i - long_window:i]) / long_window
            price = closes[i]
            if short_ma > long_ma and position == 0:
                position = (cash * (1.0 - fee)) / price  # pay fee on the buy notional
                cash = 0
                trades.append({"i": i, "action": "buy", "price": price})
            elif short_ma < long_ma and position > 0:
                cash = position * price * (1.0 - fee)     # pay fee on the sell notional
                position = 0
                trades.append({"i": i, "action": "sell", "price": price})
        final_value = cash + position * closes[-1]
        return {"ok": True, "starting_value": 1.0, "ending_value": final_value,
                "return_pct": (final_value - 1.0) * 100, "trades": trades, "fee_bps": fee_bps}

    def backtest_strategy(self, ticker: str, asset_class: str = "stock", days: int = 120, short_window: int = 5,
                          long_window: int = 20, fee_bps: float = 0.0) -> dict[str, Any]:
        data = self.get_historical_data(ticker, timeframe="1Day", asset_class=asset_class, days=days)
        raw = data.get("bars", {})
        # alpaca-py objects serialize differently by version; accept list-like or symbol-keyed bars.
        bars = raw.get("data", {}).get(ticker) if isinstance(raw, dict) else None
        if not bars and isinstance(raw, dict) and ticker in raw:
            bars = raw[ticker]
        if not isinstance(bars, list):
            return {"ok": False, "error": "could not parse bars from SDK response", "raw_shape": str(type(raw))}
        return self.backtest_strategy_from_bars(bars, short_window, long_window, fee_bps)

    @staticmethod
    def backtest_mean_reversion_from_bars(bars: list[dict[str, Any]], window: int = 20, k: float = 1.0,
                                          fee_bps: float = 0.0) -> dict[str, Any]:
        # Long-only mean reversion: BUY when close < MA - k*σ (oversold), EXIT when
        # close >= MA (reverted to the mean). Opposite logic to the crossover.
        import statistics
        closes = [float(b["close"]) for b in bars if b.get("close") is not None]
        if len(closes) < window + 2:
            return {"ok": False, "error": "not enough bars"}
        fee = max(0.0, fee_bps) / 10000.0
        cash, position = 1.0, 0.0
        trades = []
        for i in range(window, len(closes)):
            w = closes[i - window:i]
            ma = sum(w) / window
            sd = statistics.pstdev(w)
            price = closes[i]
            if position == 0 and price < ma - k * sd:
                position = (cash * (1.0 - fee)) / price
                cash = 0
                trades.append({"i": i, "action": "buy", "price": price})
            elif position > 0 and price >= ma:
                cash = position * price * (1.0 - fee)
                position = 0
                trades.append({"i": i, "action": "sell", "price": price})
        final_value = cash + position * closes[-1]
        return {"ok": True, "starting_value": 1.0, "ending_value": final_value,
                "return_pct": (final_value - 1.0) * 100, "trades": trades, "fee_bps": fee_bps, "window": window, "k": k}

    def backtest_mean_reversion(self, ticker: str, asset_class: str = "stock", days: int = 120, window: int = 20,
                                k: float = 1.0, fee_bps: float = 0.0) -> dict[str, Any]:
        data = self.get_historical_data(ticker, timeframe="1Day", asset_class=asset_class, days=days)
        raw = data.get("bars", {})
        bars = raw.get("data", {}).get(ticker) if isinstance(raw, dict) else None
        if not bars and isinstance(raw, dict) and ticker in raw:
            bars = raw[ticker]
        if not isinstance(bars, list):
            return {"ok": False, "error": "could not parse bars from SDK response", "raw_shape": str(type(raw))}
        return self.backtest_mean_reversion_from_bars(bars, window, k, fee_bps)

    def _estimate_price(self, symbol: str, asset_class: str) -> float | None:
        # Best-effort latest trade price for sizing a MARKET order. Fail-SAFE:
        # any error returns None, and the risk manager then BLOCKS the order
        # (fail-closed) rather than letting an unpriceable order through.
        try:
            if asset_class == "crypto":
                from alpaca.data.requests import CryptoLatestTradeRequest
                resp = self.crypto_data.get_crypto_latest_trade(CryptoLatestTradeRequest(symbol_or_symbols=symbol))
            elif asset_class == "option":
                from alpaca.data.requests import OptionLatestTradeRequest
                resp = self.option_data.get_option_latest_trade(OptionLatestTradeRequest(symbol_or_symbols=symbol))
            else:
                from alpaca.data.requests import StockLatestTradeRequest
                resp = self.stock_data.get_stock_latest_trade(StockLatestTradeRequest(symbol_or_symbols=symbol))
            trade = resp.get(symbol) if isinstance(resp, dict) else None
            price = getattr(trade, "price", None)
            return float(price) if price else None
        except Exception:
            return None

    def _current_position_notional(self, symbol: str) -> float | None:
        # Current market value of an open position in `symbol`, for the position
        # cap. None when there is no position or the lookup fails (cap then skipped
        # for this order; the per-order notional cap still applies).
        try:
            pos = self.trading.get_open_position(symbol)
            mv = getattr(pos, "market_value", None)
            return abs(float(mv)) if mv is not None else None
        except Exception:
            return None

    @staticmethod
    def _timeframe(value: str):
        from alpaca.data.timeframe import TimeFrame
        mapping = {"1Min": TimeFrame.Minute, "1Hour": TimeFrame.Hour, "1Day": TimeFrame.Day, "1Week": TimeFrame.Week}
        return mapping.get(value, TimeFrame.Day)
