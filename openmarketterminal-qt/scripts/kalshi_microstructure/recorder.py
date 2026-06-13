from __future__ import annotations

import asyncio
import json
import time
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from .auth import KalshiCredentials
from .cfbenchmarks import (
    CF_INDEX_BY_SYMBOL,
    CFValueCache,
    maintain_kalshi_cf_cache,
    settlement_state_payload,
    tick_payload,
)
from .kalshi import KalshiRestClient
from .models import Market
from .spot_ws import SpotCache, maintain_spot_cache
from .strategy import market_strike_bounds, market_threshold
from .ws import KalshiWebSocketClient


@dataclass(frozen=True)
class SessionRecordConfig:
    ticker: str
    symbol: str = "BTC-USD"
    seconds: float = 60.0
    spot_seconds: float = 2.0
    vol_lookback_seconds: float = 60.0
    min_annual_volatility: float = 0.20
    max_annual_volatility: float = 2.50
    cf_enabled: bool = True
    cf_seconds: float = 1.0
    out: Path = Path("logs/session-recording.jsonl")


@dataclass(frozen=True)
class SeriesRecordConfig:
    series: str = "KXBTC15M"
    symbol: str = "BTC-USD"
    run_seconds: float = 60.0 * 60.0
    max_market_seconds: float = 15.0 * 60.0
    spot_seconds: float = 2.0
    vol_lookback_seconds: float = 60.0
    min_annual_volatility: float = 0.20
    max_annual_volatility: float = 2.50
    cf_enabled: bool = True
    cf_seconds: float = 1.0
    out_dir: Path = Path("logs/series-recordings")
    idle_seconds: float = 3.0


async def record_session(
    client: KalshiRestClient,
    credentials: KalshiCredentials,
    env: str,
    config: SessionRecordConfig,
) -> int:
    market = client.get_market(config.ticker)
    config.out.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + config.seconds
    count = 0

    with config.out.open("a", encoding="utf-8") as handle:
        _write(handle, "session_started", {"config": _config_payload(config), "market": _market_payload(market)})
        count += 1

        spot_cache = SpotCache(config.symbol)
        spot_task = asyncio.create_task(maintain_spot_cache(spot_cache))
        cf_index = CF_INDEX_BY_SYMBOL.get(config.symbol)
        cf_cache = CFValueCache(cf_index) if config.cf_enabled and cf_index is not None else None
        cf_task = (
            asyncio.create_task(maintain_kalshi_cf_cache(cf_cache, credentials, env=env))
            if cf_cache is not None
            else None
        )
        ws_client = KalshiWebSocketClient(credentials=credentials, env=env)
        next_spot = time.monotonic() + config.spot_seconds
        next_cf = time.monotonic() + config.cf_seconds
        try:
            async for message in ws_client.stream(
                market_tickers=[config.ticker],
                channels=["orderbook_delta"],
            ):
                now = time.monotonic()
                if now >= deadline:
                    break

                _write(handle, "kalshi", {"message": message})
                count += 1

                if now >= next_spot:
                    if spot_cache.price is not None:
                        _write(
                            handle,
                            "spot",
                            {
                                "symbol": config.symbol,
                                "spot": spot_cache.price,
                                "spot_age_seconds": spot_cache.age_seconds,
                                "exchange_ts": spot_cache.exchange_ts,
                                "source": "coinbase_ws",
                                "annual_volatility": spot_cache.realized_volatility(
                                    lookback_seconds=config.vol_lookback_seconds,
                                    floor=config.min_annual_volatility,
                                    cap=config.max_annual_volatility,
                                ),
                                "volatility_tick_count": spot_cache.tick_count,
                            },
                        )
                    else:
                        _write(handle, "spot_error", {"kind": "NoSpotTick", "message": "spot cache empty"})
                    count += 1
                    next_spot = now + config.spot_seconds
                if cf_cache is not None and now >= next_cf:
                    cf_payload = _cf_payload(cf_cache, market)
                    if cf_payload is not None:
                        _write(handle, "cf", cf_payload)
                    else:
                        _write(handle, "cf_error", {"index": cf_cache.index, "message": "cf cache empty"})
                    count += 1
                    next_cf = now + config.cf_seconds
        finally:
            spot_task.cancel()
            if cf_task is not None:
                cf_task.cancel()
            try:
                await spot_task
            except asyncio.CancelledError:
                pass
            if cf_task is not None:
                try:
                    await cf_task
                except asyncio.CancelledError:
                    pass

        _write(handle, "session_stopped", {"messages": count})
        count += 1
    return count


async def record_series(
    client: KalshiRestClient,
    credentials: KalshiCredentials,
    env: str,
    config: SeriesRecordConfig,
) -> list[Path]:
    config.out_dir.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + config.run_seconds
    recorded: list[Path] = []
    seen: set[str] = set()

    while time.monotonic() < deadline:
        market = _find_active_market(client, config.series)
        if market is None:
            await asyncio.sleep(min(config.idle_seconds, max(0.0, deadline - time.monotonic())))
            continue
        if market.ticker in seen:
            await asyncio.sleep(min(config.idle_seconds, max(0.0, deadline - time.monotonic())))
            continue

        seconds_left = max(0.0, deadline - time.monotonic())
        record_seconds = _record_seconds_for_market(market, seconds_left, config.max_market_seconds)
        if record_seconds <= 0:
            seen.add(market.ticker)
            continue

        out = _session_output_path(config.out_dir, market)
        session = SessionRecordConfig(
            ticker=market.ticker,
            symbol=config.symbol,
            seconds=record_seconds,
            spot_seconds=config.spot_seconds,
            vol_lookback_seconds=config.vol_lookback_seconds,
            min_annual_volatility=config.min_annual_volatility,
            max_annual_volatility=config.max_annual_volatility,
            cf_enabled=config.cf_enabled,
            cf_seconds=config.cf_seconds,
            out=out,
        )
        await record_session(client, credentials, env, session)
        recorded.append(out)
        seen.add(market.ticker)

    return recorded


def _write(handle: Any, event: str, payload: dict[str, Any]) -> None:
    row = {
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "event": event,
        **payload,
    }
    handle.write(json.dumps(row, sort_keys=True) + "\n")
    handle.flush()


def _market_payload(market: Market) -> dict[str, Any]:
    strike_type, strike_floor, strike_cap = market_strike_bounds(market)
    return {
        "ticker": market.ticker,
        "title": market.title,
        "subtitle": market.subtitle,
        "status": market.status,
        "close_time": market.close_time.isoformat() if market.close_time else None,
        "threshold": market_threshold(market),
        "strike_type": strike_type,
        "strike_floor": strike_floor,
        "strike_cap": strike_cap,
        "raw": market.raw,
    }


def _config_payload(config: SessionRecordConfig) -> dict[str, Any]:
    payload = asdict(config)
    payload["out"] = str(config.out)
    return payload


def _cf_payload(cache: CFValueCache, market: Market) -> dict[str, Any] | None:
    latest = cache.latest
    threshold = market_threshold(market)
    if latest is None or threshold is None or market.close_time is None:
        return None
    return {
        "index": cache.index,
        "tick": tick_payload(latest),
        "settlement_state": settlement_state_payload(
            cache.settlement_state(threshold=threshold, window_end=market.close_time)
        ),
    }


def _find_active_market(client: KalshiRestClient, series: str) -> Market | None:
    markets, _ = client.get_markets(series_ticker=series, status="open", limit=20)
    active = [market for market in markets if market.status in {"active", "open"}]
    if not active:
        return None
    return min(active, key=lambda market: market.close_time or datetime.max.replace(tzinfo=timezone.utc))


def _record_seconds_for_market(market: Market, seconds_left: float, max_market_seconds: float) -> float:
    market_seconds = max_market_seconds
    if market.close_time is not None:
        market_seconds = max(0.0, (market.close_time - datetime.now(timezone.utc)).total_seconds())
    return min(seconds_left, max_market_seconds, market_seconds)


def _session_output_path(out_dir: Path, market: Market) -> Path:
    close = market.close_time.strftime("%Y%m%dT%H%M%SZ") if market.close_time else "unknown-close"
    return out_dir / f"{close}-{market.ticker}.jsonl"
