from __future__ import annotations

import asyncio
import json
import math
import time
from dataclasses import dataclass
from collections import deque
from typing import Any

import websockets


COINBASE_EXCHANGE_WS_URL = "wss://ws-feed.exchange.coinbase.com"


@dataclass
class SpotCache:
    symbol: str = "BTC-USD"
    max_ticks: int = 2000
    price: float | None = None
    exchange_ts: str | None = None
    received_monotonic: float | None = None
    ticks: deque[tuple[float, float]] | None = None

    def __post_init__(self) -> None:
        if self.ticks is None:
            self.ticks = deque(maxlen=self.max_ticks)

    @property
    def age_seconds(self) -> float | None:
        if self.received_monotonic is None:
            return None
        return time.monotonic() - self.received_monotonic

    def update(self, price: float, exchange_ts: str | None = None) -> None:
        self.price = price
        self.exchange_ts = exchange_ts
        self.received_monotonic = time.monotonic()
        self.ticks.append((self.received_monotonic, price))

    @property
    def tick_count(self) -> int:
        return len(self.ticks)

    def realized_volatility(
        self,
        *,
        lookback_seconds: float = 60.0,
        floor: float = 0.20,
        cap: float = 2.50,
    ) -> float | None:
        return realized_volatility_from_ticks(
            list(self.ticks),
            lookback_seconds=lookback_seconds,
            floor=floor,
            cap=cap,
        )

    def time_weighted_average(self, *, lookback_seconds: float) -> PriceWindow | None:
        return time_weighted_average_from_ticks(list(self.ticks), lookback_seconds=lookback_seconds)


@dataclass(frozen=True)
class PriceWindow:
    price: float
    span_seconds: float
    tick_count: int


async def stream_coinbase_ticker(symbol: str = "BTC-USD"):
    async with websockets.connect(COINBASE_EXCHANGE_WS_URL) as websocket:
        await websocket.send(
            json.dumps(
                {
                    "type": "subscribe",
                    "product_ids": [symbol],
                    "channels": ["ticker"],
                }
            )
        )
        async for raw in websocket:
            message = json.loads(raw)
            price = parse_coinbase_ticker_price(message, symbol)
            if price is not None:
                yield {
                    "symbol": symbol,
                    "price": price,
                    "exchange_ts": message.get("time"),
                    "message": message,
                }


async def maintain_spot_cache(cache: SpotCache) -> None:
    while True:
        try:
            async for tick in stream_coinbase_ticker(cache.symbol):
                cache.update(float(tick["price"]), exchange_ts=tick.get("exchange_ts"))
        except asyncio.CancelledError:
            raise
        except Exception:
            await asyncio.sleep(1.0)


async def sample_live_spot(symbol: str = "BTC-USD", timeout: float = 10.0) -> float:
    async with asyncio.timeout(timeout):
        async for tick in stream_coinbase_ticker(symbol):
            return float(tick["price"])
    raise TimeoutError(f"No {symbol} spot tick received")


def parse_coinbase_ticker_price(message: dict[str, Any], symbol: str) -> float | None:
    if message.get("type") != "ticker":
        return None
    if message.get("product_id") != symbol:
        return None
    price = message.get("price")
    if price is None:
        return None
    return float(price)


def realized_volatility_from_ticks(
    ticks: list[tuple[float, float]],
    *,
    lookback_seconds: float = 60.0,
    floor: float = 0.20,
    cap: float = 2.50,
) -> float | None:
    if len(ticks) < 3:
        return None
    latest_ts = ticks[-1][0]
    window = [(ts, price) for ts, price in ticks if latest_ts - ts <= lookback_seconds and price > 0]
    if len(window) < 3:
        return None

    returns: list[float] = []
    for (_, prev), (_, curr) in zip(window, window[1:]):
        if prev > 0 and curr > 0 and prev != curr:
            returns.append(math.log(curr / prev))
    if len(returns) < 2:
        return None

    mean = sum(returns) / len(returns)
    variance = sum((value - mean) ** 2 for value in returns) / (len(returns) - 1)
    if variance <= 0:
        return None

    elapsed = max(1e-9, window[-1][0] - window[0][0])
    samples_per_year = len(returns) * 365.0 * 24.0 * 60.0 * 60.0 / elapsed
    annualized = math.sqrt(variance * samples_per_year)
    return min(cap, max(floor, annualized))


def time_weighted_average_from_ticks(
    ticks: list[tuple[float, float]],
    *,
    lookback_seconds: float,
) -> PriceWindow | None:
    if lookback_seconds <= 0 or not ticks:
        return None
    now = ticks[-1][0]
    start = max(ticks[0][0], now - lookback_seconds)
    before = [(ts, price) for ts, price in ticks if ts < start and price > 0]
    window = [(ts, price) for ts, price in ticks if start <= ts <= now and price > 0]
    if not window:
        return None
    points: list[tuple[float, float]] = []
    if before:
        points.append((start, before[-1][1]))
    elif window[0][0] > start:
        points.append(window[0])
    points.extend(window)
    points = sorted({(ts, price) for ts, price in points})
    if len(points) < 2:
        return PriceWindow(price=points[0][1], span_seconds=0.0, tick_count=len(window))

    total = 0.0
    duration = 0.0
    for (ts, price), (next_ts, _next_price) in zip(points, points[1:]):
        dt = max(0.0, next_ts - ts)
        total += price * dt
        duration += dt
    tail_dt = max(0.0, now - points[-1][0])
    total += points[-1][1] * tail_dt
    duration += tail_dt
    if duration <= 0:
        return PriceWindow(price=points[-1][1], span_seconds=0.0, tick_count=len(window))
    return PriceWindow(price=total / duration, span_seconds=duration, tick_count=len(window))
