from __future__ import annotations

import re
import time
from dataclasses import dataclass, field
from threading import Lock
from typing import Literal
from .config import Settings


class TradingBlocked(Exception):
    pass


@dataclass
class TokenBucket:
    per_minute: int
    _events: list[float] = field(default_factory=list)
    _lock: Lock = field(default_factory=Lock)

    def check(self) -> None:
        now = time.time()
        cutoff = now - 60.0
        with self._lock:
            self._events = [t for t in self._events if t >= cutoff]
            if len(self._events) >= self.per_minute:
                raise TradingBlocked(f"rate limit exceeded: {self.per_minute} calls/min")
            self._events.append(now)


@dataclass
class DailyCounter:
    count: int = 0
    day_key: str = ""
    _lock: Lock = field(default_factory=Lock)

    def increment_or_block(self, max_count: int) -> None:
        current = time.strftime("%Y-%m-%d", time.gmtime())
        with self._lock:
            if self.day_key != current:
                self.day_key = current
                self.count = 0
            if self.count >= max_count:
                raise TradingBlocked(f"daily order count limit exceeded: {max_count}")
            self.count += 1


OCC_RE = re.compile(r"^[A-Z]{1,6}\d{6}[CP]\d{8}$")


def is_option_symbol(symbol: str) -> bool:
    return bool(OCC_RE.match(symbol.replace(" ", "").upper()))


def option_contract_multiplier(symbol: str) -> int:
    return 100 if is_option_symbol(symbol) else 1


class RiskManager:
    def __init__(self, settings: Settings):
        self.settings = settings
        self.bucket = TokenBucket(settings.rate_limit_per_min)
        self.daily_counter = DailyCounter()

    def check_read(self) -> None:
        self.bucket.check()

    def check_trade(
        self,
        *,
        venue: Literal["alpaca", "coinbase"],
        symbol: str,
        side: str,
        quantity: float,
        estimated_price: float | None,
        confirmation_token: str | None,
        asset_class: str | None = None,
    ) -> dict:
        self.bucket.check()
        if quantity <= 0:
            raise TradingBlocked("quantity must be positive")

        normalized_side = side.lower()
        if normalized_side not in {"buy", "sell"}:
            raise TradingBlocked("side must be buy or sell")

        option = asset_class == "option" or is_option_symbol(symbol)
        if option and not self.settings.allow_options_trading:
            raise TradingBlocked("options trading disabled; set ALLOW_OPTIONS_TRADING=true to enable")

        multiplier = option_contract_multiplier(symbol) if option else 1
        notional = None
        if estimated_price is not None:
            notional = abs(quantity * estimated_price * multiplier)
            if notional > self.settings.max_order_notional_usd:
                raise TradingBlocked(
                    f"order notional ${notional:,.2f} exceeds MAX_ORDER_NOTIONAL_USD ${self.settings.max_order_notional_usd:,.2f}"
                )

        if self.settings.trading_mode == "live" and self.settings.require_confirmation:
            if confirmation_token != self.settings.confirmation_token:
                raise TradingBlocked("live trade requires the configured confirmation_token")

        self.daily_counter.increment_or_block(self.settings.max_daily_order_count)

        return {
            "venue": venue,
            "symbol": symbol,
            "side": normalized_side,
            "quantity": quantity,
            "estimated_price": estimated_price,
            "contract_multiplier": multiplier,
            "estimated_notional_usd": notional,
            "dry_run": self.settings.dry_run,
            "trading_mode": self.settings.trading_mode,
        }
