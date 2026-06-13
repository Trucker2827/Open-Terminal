from __future__ import annotations

import json
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from .kalshi import KalshiRestClient
from .models import BinaryBook, Level, Market
from .reference import reference_spot
from .strategy import market_strike_bounds


@dataclass(frozen=True)
class RangeWatchTarget:
    series: str
    symbol: str


@dataclass(frozen=True)
class RangeObserverConfig:
    targets: tuple[RangeWatchTarget, ...]
    seconds: float = 60.0 * 60.0
    eval_seconds: float = 30.0
    limit: int = 100
    pages: int = 3
    near_count: int = 7
    max_close_seconds: float = 24.0 * 60.0 * 60.0
    out: Path = Path("logs/range-observer.jsonl")


@dataclass(frozen=True)
class RangeCandidate:
    market: Market
    seconds_to_close: float
    distance: float


def run_range_observer(client: KalshiRestClient, config: RangeObserverConfig) -> int:
    config.out.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + config.seconds
    rows = 0
    with config.out.open("a", encoding="utf-8") as handle:
        _write(handle, "range_observer_started", {"config": _config_payload(config)})
        rows += 1
        while time.monotonic() < deadline:
            started = time.monotonic()
            for target in config.targets:
                try:
                    rows += _observe_target(handle, client, target, config)
                except Exception as exc:
                    _write(
                        handle,
                        "range_observer_error",
                        {
                            "series": target.series,
                            "symbol": target.symbol,
                            "error_type": type(exc).__name__,
                            "message": str(exc),
                        },
                    )
                    rows += 1
            sleep_for = min(config.eval_seconds, max(0.0, deadline - time.monotonic()))
            sleep_for = max(0.0, sleep_for - max(0.0, time.monotonic() - started))
            if sleep_for:
                time.sleep(sleep_for)
        _write(handle, "range_observer_stopped", {"rows": rows})
        rows += 1
    return rows


def parse_range_targets(text: str) -> tuple[RangeWatchTarget, ...]:
    targets: list[RangeWatchTarget] = []
    for item in text.split(","):
        item = item.strip()
        if not item:
            continue
        if ":" not in item:
            raise ValueError(f"range watch target must be SERIES:SYMBOL, got {item!r}")
        series, symbol = [part.strip() for part in item.split(":", 1)]
        if not series or not symbol:
            raise ValueError(f"range watch target must be SERIES:SYMBOL, got {item!r}")
        targets.append(RangeWatchTarget(series=series, symbol=symbol))
    if not targets:
        raise ValueError("at least one range watch target is required")
    return tuple(targets)


def fetch_range_candidates(
    client: KalshiRestClient,
    *,
    series: str,
    spot: float,
    limit: int = 100,
    pages: int = 3,
    max_close_seconds: float = 24.0 * 60.0 * 60.0,
    now: datetime | None = None,
) -> list[RangeCandidate]:
    now = now or datetime.now(timezone.utc)
    markets = _fetch_markets(client, series=series, limit=limit, pages=pages)
    candidates: list[RangeCandidate] = []
    for market in markets:
        _, floor, cap = market_strike_bounds(market)
        if floor is None and cap is None:
            continue
        if market.close_time is None:
            continue
        seconds_to_close = (market.close_time - now).total_seconds()
        if seconds_to_close <= 0 or seconds_to_close > max_close_seconds:
            continue
        candidates.append(
            RangeCandidate(
                market=market,
                seconds_to_close=seconds_to_close,
                distance=range_distance(spot, floor, cap),
            )
        )
    if not candidates:
        return []
    soonest_close = min(candidate.market.close_time for candidate in candidates if candidate.market.close_time)
    candidates = [candidate for candidate in candidates if candidate.market.close_time == soonest_close]
    return sorted(candidates, key=lambda candidate: (candidate.distance, candidate.market.ticker))


def range_distance(spot: float, floor: float | None, cap: float | None) -> float:
    if floor is not None and cap is not None:
        if floor <= spot <= cap:
            return 0.0
        return min(abs(spot - floor), abs(spot - cap))
    if floor is not None:
        return abs(spot - floor)
    if cap is not None:
        return abs(spot - cap)
    return float("inf")


def range_relation(
    spot: float,
    strike_type: str | None,
    floor: float | None,
    cap: float | None,
) -> str:
    if floor is not None and cap is not None:
        if floor <= spot <= cap:
            return "inside"
        if spot < floor:
            return f"below_floor:{floor - spot:g}"
        return f"above_cap:{spot - cap:g}"
    if floor is not None:
        relation = "above_floor" if spot >= floor else "below_floor"
        return f"{relation}:{abs(spot - floor):g}"
    if cap is not None:
        relation = "below_cap" if spot <= cap else "above_cap"
        return f"{relation}:{abs(spot - cap):g}"
    return strike_type or "unknown"


def strike_text(market: Market) -> str:
    strike_type, floor, cap = market_strike_bounds(market)
    if strike_type == "between" and floor is not None and cap is not None:
        return f"strike=between:{floor:g}-{cap:g}"
    if strike_type in {"greater", "above"} and floor is not None:
        return f"strike=>={floor:g}"
    if strike_type in {"less", "below"} and cap is not None:
        return f"strike=<={cap:g}"
    if floor is not None:
        return f"strike~{floor:g}"
    if cap is not None:
        return f"strike~{cap:g}"
    return ""


def _observe_target(
    handle: Any,
    client: KalshiRestClient,
    target: RangeWatchTarget,
    config: RangeObserverConfig,
) -> int:
    spot = reference_spot(target.symbol)
    candidates = fetch_range_candidates(
        client,
        series=target.series,
        spot=spot,
        limit=config.limit,
        pages=config.pages,
        max_close_seconds=config.max_close_seconds,
    )
    buckets = []
    for candidate in candidates[: max(1, config.near_count)]:
        book = client.get_orderbook(candidate.market.ticker)
        buckets.append(range_bucket_payload(candidate, spot=spot, book=book))
    _write(
        handle,
        "range_snapshot",
        {
            "series": target.series,
            "symbol": target.symbol,
            "spot": spot,
            "bucket_count": len(buckets),
            "buckets": buckets,
        },
    )
    return 1


def range_bucket_payload(candidate: RangeCandidate, *, spot: float, book: BinaryBook) -> dict[str, Any]:
    market = candidate.market
    strike_type, floor, cap = market_strike_bounds(market)
    return {
        "ticker": market.ticker,
        "title": market.title,
        "status": market.status,
        "close_time": market.close_time.isoformat() if market.close_time else None,
        "seconds_to_close": candidate.seconds_to_close,
        "strike_type": strike_type,
        "strike_floor": floor,
        "strike_cap": cap,
        "distance": candidate.distance,
        "relation": range_relation(spot, strike_type, floor, cap),
        "book": book_payload(book),
    }


def book_payload(book: BinaryBook) -> dict[str, Any]:
    return {
        "best_yes_bid": _level_payload(book.best_yes_bid),
        "best_yes_ask": _level_payload(book.best_yes_ask),
        "best_no_bid": _level_payload(book.best_no_bid),
        "best_no_ask": _level_payload(book.best_no_ask),
        "yes_spread": book.yes_spread,
    }


def _level_payload(level: Level | None) -> dict[str, float] | None:
    if level is None:
        return None
    return {"price": level.price, "size": level.size}


def _fetch_markets(
    client: KalshiRestClient,
    *,
    series: str,
    limit: int,
    pages: int,
) -> list[Market]:
    markets = []
    cursor = None
    for _ in range(max(1, pages)):
        page, cursor = client.get_markets(series_ticker=series, limit=limit, cursor=cursor)
        markets.extend(page)
        if not cursor:
            break
    return markets


def _write(handle: Any, event: str, payload: dict[str, Any]) -> None:
    row = {"recorded_at": datetime.now(timezone.utc).isoformat(), "event": event, **payload}
    handle.write(json.dumps(row, sort_keys=True) + "\n")
    handle.flush()


def _config_payload(config: RangeObserverConfig) -> dict[str, Any]:
    return {
        "targets": [target.__dict__ for target in config.targets],
        "seconds": config.seconds,
        "eval_seconds": config.eval_seconds,
        "limit": config.limit,
        "pages": config.pages,
        "near_count": config.near_count,
        "max_close_seconds": config.max_close_seconds,
        "out": str(config.out),
    }
