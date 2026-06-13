from __future__ import annotations

import asyncio
import json
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from .auth import KalshiCredentials
from .book_cache import KalshiBookCache
from .cf_liquidity import CFLiquidityConfig, cf_liquidity_payload
from .cfbenchmarks import (
    CF_INDEX_BY_SYMBOL,
    CFValueCache,
    maintain_kalshi_cf_cache,
    settlement_state_payload,
    tick_payload,
)
from .kalshi import KalshiRestClient
from .market_hours import daily_crypto_restart_window
from .models import BinaryBook, EdgeSignal, Market
from .bot import live_fill_payload, submit_live_signal
from .fees import rounded_fee
from .preflight import preflight_limit_order, preflight_payload
from .strategy import market_threshold
from .ws import KalshiWebSocketClient


@dataclass(frozen=True)
class OpportunityWatchTarget:
    series: str
    symbol: str


@dataclass(frozen=True)
class OpportunityWatchConfig:
    targets: tuple[OpportunityWatchTarget, ...]
    run_seconds: float = 60.0 * 60.0
    watch_seconds: float = 120.0
    eval_seconds: float = 1.0
    out: Path = Path("logs/cf-opportunity-watcher.jsonl")
    print_all: bool = False
    liquidity: CFLiquidityConfig = field(default_factory=CFLiquidityConfig)
    live: "OpportunityLiveConfig" = field(default_factory=lambda: OpportunityLiveConfig())


@dataclass(frozen=True)
class OpportunityLiveConfig:
    enabled: bool = False
    max_order_cost: float = 5.0
    max_run_cost: float = 20.0
    max_orders: int = 1
    min_fill_cost: float = 0.01
    fee_buffer: float = 0.0
    require_empty_portfolio: bool = True
    allow_multiple_per_market: bool = False
    block_daily_restart_window: bool = True
    tiered_sizing: bool = False
    micro_order_cost: float = 2.0
    weak_order_cost: float = 5.0
    strong_order_cost: float = 20.0
    very_strong_order_cost: float = 50.0
    weak_max_entry_price: float = 1.0
    strong_max_entry_price: float = 0.95
    very_strong_max_entry_price: float = 0.90


@dataclass
class OpportunityLiveState:
    spent: float = 0.0
    submitted_orders: int = 0
    filled_orders: int = 0
    stopped_reason: str | None = None
    traded_markets: set[str] = field(default_factory=set)


async def run_opportunity_watcher(
    client: KalshiRestClient,
    credentials: KalshiCredentials,
    env: str,
    config: OpportunityWatchConfig,
) -> None:
    config.out.parent.mkdir(parents=True, exist_ok=True)
    lock = asyncio.Lock()
    execution_lock = asyncio.Lock()
    live_state = OpportunityLiveState()
    deadline = time.monotonic() + config.run_seconds
    tasks = [
        asyncio.create_task(
            _watch_target(client, credentials, env, config, target, deadline, lock, execution_lock, live_state)
        )
        for target in config.targets
    ]
    try:
        await asyncio.gather(*tasks)
    finally:
        for task in tasks:
            task.cancel()
        for task in tasks:
            try:
                await task
            except asyncio.CancelledError:
                pass


def opportunity_payload(
    *,
    market: Market,
    book: BinaryBook | None,
    cf_cache: CFValueCache | None,
    config: OpportunityWatchConfig,
    target: OpportunityWatchTarget,
    now: datetime | None = None,
) -> dict[str, Any]:
    now = (now or datetime.now(timezone.utc)).astimezone(timezone.utc)
    seconds_to_close = _seconds_to_close(market, now)
    zone = _watch_zone(seconds_to_close, config.watch_seconds)
    threshold = market_threshold(market)

    cf_payload: dict[str, Any] | None = None
    liquidity: dict[str, Any] = {"status": "blocked", "executable": False, "reject_reason": "outside_watch_window"}
    if cf_cache is not None and cf_cache.latest is not None and threshold is not None and market.close_time is not None:
        state = cf_cache.settlement_state(threshold=threshold, window_end=market.close_time, now=now)
        state_payload = settlement_state_payload(state)
        cf_payload = {
            "index": cf_cache.index,
            "tick": tick_payload(cf_cache.latest),
            "settlement_state": state_payload,
        }
        if zone != "outside":
            liquidity = cf_liquidity_payload(state_payload, book, config.liquidity)
        early_side = _projected_side(state_payload)
    else:
        early_side = None
        if zone != "outside":
            liquidity = {"status": "blocked", "executable": False, "reject_reason": "missing_cf_or_threshold"}

    return {
        "ts": now.isoformat(),
        "event": "cf_opportunity",
        "series": target.series,
        "symbol": target.symbol,
        "market": {
            "ticker": market.ticker,
            "close_time": market.close_time.isoformat() if market.close_time else None,
            "threshold": threshold,
        },
        "seconds_to_close": seconds_to_close,
        "zone": zone,
        "early_side": early_side,
        "book": _book_payload(book),
        "cf": cf_payload,
        "liquidity": liquidity,
    }


async def _watch_target(
    client: KalshiRestClient,
    credentials: KalshiCredentials,
    env: str,
    config: OpportunityWatchConfig,
    target: OpportunityWatchTarget,
    deadline: float,
    lock: asyncio.Lock,
    execution_lock: asyncio.Lock,
    live_state: OpportunityLiveState,
) -> None:
    cf_index = CF_INDEX_BY_SYMBOL.get(target.symbol)
    cf_cache = CFValueCache(cf_index) if cf_index is not None else None
    cf_task = (
        asyncio.create_task(maintain_kalshi_cf_cache(cf_cache, credentials, env=env))
        if cf_cache is not None
        else None
    )
    try:
        while time.monotonic() < deadline:
            market = await asyncio.to_thread(_find_active_market, client, target.series)
            if market is None or market.close_time is None:
                await asyncio.sleep(2.0)
                continue
            seconds_to_close = _seconds_to_close(market)
            if seconds_to_close is not None and seconds_to_close > config.watch_seconds:
                await asyncio.sleep(min(5.0, max(1.0, seconds_to_close - config.watch_seconds)))
                continue
            if seconds_to_close is not None and seconds_to_close <= 0:
                await asyncio.sleep(1.0)
                continue
            await _stream_market_window(
                client,
                credentials,
                env,
                config,
                target,
                market,
                cf_cache,
                deadline,
                lock,
                execution_lock,
                live_state,
            )
    finally:
        if cf_task is not None:
            cf_task.cancel()
            try:
                await cf_task
            except asyncio.CancelledError:
                pass


async def _stream_market_window(
    client: KalshiRestClient,
    credentials: KalshiCredentials,
    env: str,
    config: OpportunityWatchConfig,
    target: OpportunityWatchTarget,
    market: Market,
    cf_cache: CFValueCache | None,
    deadline: float,
    lock: asyncio.Lock,
    execution_lock: asyncio.Lock,
    live_state: OpportunityLiveState,
) -> None:
    cache = KalshiBookCache(market.ticker)
    ws_client = KalshiWebSocketClient(credentials=credentials, env=env)
    next_eval = 0.0
    async for message in ws_client.stream(market_tickers=[market.ticker], channels=["orderbook_delta"]):
        if time.monotonic() >= deadline:
            break
        cache.apply(message)
        if message.get("type") not in {"orderbook_snapshot", "orderbook_delta"}:
            continue
        now_mono = time.monotonic()
        if now_mono < next_eval:
            continue
        now = datetime.now(timezone.utc)
        seconds_to_close = _seconds_to_close(market, now)
        if seconds_to_close is not None and seconds_to_close <= 0:
            break
        payload = opportunity_payload(
            market=market,
            book=cache.to_book(),
            cf_cache=cf_cache,
            config=config,
            target=target,
            now=now,
        )
        if payload["zone"] != "outside":
            await _write_payload(config.out, payload, lock)
            if config.print_all or payload.get("liquidity", {}).get("executable"):
                print(json.dumps(_console_payload(payload), sort_keys=True), flush=True)
            if payload.get("liquidity", {}).get("executable"):
                await _maybe_execute_live_opportunity(
                    client,
                    config,
                    payload,
                    lock,
                    execution_lock,
                    live_state,
                )
        next_eval = now_mono + config.eval_seconds


async def _maybe_execute_live_opportunity(
    client: KalshiRestClient,
    config: OpportunityWatchConfig,
    payload: dict[str, Any],
    write_lock: asyncio.Lock,
    execution_lock: asyncio.Lock,
    live_state: OpportunityLiveState,
) -> None:
    async with execution_lock:
        candidate = live_candidate_from_payload(payload, config.live, live_state)
        if not candidate["approved"]:
            if config.live.enabled:
                await _write_payload(
                    config.out,
                    {"event": "cf_live_reject", "ts": _utc_now(), "candidate": candidate, "opportunity": payload},
                    write_lock,
                )
            return

        pre_audit = await asyncio.to_thread(_live_safety_snapshot, client)
        if config.live.require_empty_portfolio and pre_audit["portfolio_value"] > 0:
            candidate = {**candidate, "approved": False, "reason": "portfolio_value_active", "pre_audit": pre_audit}
            await _write_payload(
                config.out,
                {"event": "cf_live_reject", "ts": _utc_now(), "candidate": candidate, "opportunity": payload},
                write_lock,
            )
            return
        if pre_audit["resting_orders"] > 0:
            candidate = {**candidate, "approved": False, "reason": "resting_orders_exist", "pre_audit": pre_audit}
            await _write_payload(
                config.out,
                {"event": "cf_live_reject", "ts": _utc_now(), "candidate": candidate, "opportunity": payload},
                write_lock,
            )
            return

        signal = _candidate_signal(payload, candidate)
        preflight = await asyncio.to_thread(_fresh_preflight, client, signal, candidate, config.live)
        await _write_payload(
            config.out,
            {
                "event": "cf_live_preflight",
                "ts": _utc_now(),
                "candidate": candidate,
                "pre_audit": pre_audit,
                "preflight": preflight,
            },
            write_lock,
        )
        if not preflight.get("approved"):
            return

        response = await asyncio.to_thread(
            submit_live_signal,
            client,
            signal,
            candidate["shares"],
            price=candidate["price"],
        )
        live_state.submitted_orders += 1
        await _write_payload(
            config.out,
            {"event": "cf_live_order_submitted", "ts": _utc_now(), "candidate": candidate, "response": response},
            write_lock,
        )

        fill = live_fill_payload(response, signal, price=candidate["price"])
        if fill["fill_count"] > 0:
            live_state.filled_orders += 1
            live_state.spent += fill["fill_cost"] + fill["fees"]
            live_state.traded_markets.add(signal.ticker)
        post_audit = await asyncio.to_thread(_live_safety_snapshot, client)
        if post_audit["resting_orders"] > 0:
            live_state.stopped_reason = "post_trade_resting_orders"
        if config.live.require_empty_portfolio and post_audit["portfolio_value"] > 0:
            live_state.stopped_reason = "post_trade_open_exposure"
        await _write_payload(
            config.out,
            {
                "event": "cf_live_order_result",
                "ts": _utc_now(),
                "candidate": candidate,
                "fill": fill,
                "state": live_state_payload(live_state),
                "post_audit": post_audit,
            },
            write_lock,
        )


def live_candidate_from_payload(
    payload: dict[str, Any],
    config: OpportunityLiveConfig,
    state: OpportunityLiveState | None = None,
) -> dict[str, Any]:
    state = state or OpportunityLiveState()
    if not config.enabled:
        return {"approved": False, "reason": "live_disabled"}
    maintenance = daily_crypto_restart_window()
    if config.block_daily_restart_window and maintenance.active:
        return {"approved": False, "reason": "daily_restart_window", "maintenance": maintenance.label}
    if state.stopped_reason:
        return {"approved": False, "reason": state.stopped_reason}
    if payload.get("zone") != "execution_10_0":
        return {"approved": False, "reason": "not_execution_zone"}
    liquidity = payload.get("liquidity") or {}
    if not liquidity.get("executable"):
        return {"approved": False, "reason": liquidity.get("reject_reason") or "not_executable"}
    market = payload.get("market") or {}
    ticker = market.get("ticker")
    if ticker in state.traded_markets and not config.allow_multiple_per_market:
        return {"approved": False, "reason": "market_already_traded", "ticker": ticker}
    if state.submitted_orders >= config.max_orders:
        return {"approved": False, "reason": "max_orders_reached"}
    remaining_budget = config.max_run_cost - state.spent
    if remaining_budget <= 0:
        return {"approved": False, "reason": "max_run_cost_reached"}
    ask = liquidity.get("ask") or {}
    side = liquidity.get("decision_side")
    price = _float_or_none(ask.get("price"))
    size = _float_or_none(ask.get("size"))
    if side not in {"YES", "NO"} or ticker is None or price is None or size is None:
        return {"approved": False, "reason": "missing_candidate_fields"}
    if price <= 0 or price >= 1 or size <= 0:
        return {"approved": False, "reason": "invalid_price_or_size"}
    tier = live_size_tier(price, config)
    order_budget = min(tier["order_cost"], remaining_budget)
    fee_buffer = max(0.0, config.fee_buffer)
    shares = min(size, order_budget / price)
    cost = shares * price
    estimated_fee = rounded_fee(price, shares)
    if cost < config.min_fill_cost:
        return {"approved": False, "reason": "below_min_fill_cost", "cost": cost}
    return {
        "approved": True,
        "ticker": ticker,
        "symbol": payload.get("symbol"),
        "side": side,
        "price": price,
        "shares": shares,
        "estimated_cost": cost,
        "estimated_fee": estimated_fee,
        "fee_buffer": fee_buffer,
        "max_total_cost": cost + estimated_fee + fee_buffer,
        "seconds_to_close": payload.get("seconds_to_close"),
        "remaining_budget": remaining_budget,
        "max_order_cost": tier["order_cost"],
        "tier": tier["tier"],
        "tier_reason": tier["reason"],
    }


def live_size_tier(price: float, config: OpportunityLiveConfig) -> dict[str, Any]:
    if not config.tiered_sizing:
        return {"tier": "flat", "order_cost": config.max_order_cost, "reason": "flat_max_order_cost"}
    if price <= config.very_strong_max_entry_price:
        return {
            "tier": "very_strong",
            "order_cost": config.very_strong_order_cost,
            "reason": f"ask_price <= {config.very_strong_max_entry_price:.4f}",
        }
    if price <= config.strong_max_entry_price:
        return {
            "tier": "strong",
            "order_cost": config.strong_order_cost,
            "reason": f"ask_price <= {config.strong_max_entry_price:.4f}",
        }
    if price <= config.weak_max_entry_price:
        return {
            "tier": "weak",
            "order_cost": config.weak_order_cost,
            "reason": f"ask_price <= {config.weak_max_entry_price:.4f}",
        }
    return {
        "tier": "micro",
        "order_cost": config.micro_order_cost,
        "reason": f"ask_price > {config.weak_max_entry_price:.4f}",
    }


def live_state_payload(state: OpportunityLiveState) -> dict[str, Any]:
    return {
        "spent": state.spent,
        "submitted_orders": state.submitted_orders,
        "filled_orders": state.filled_orders,
        "stopped_reason": state.stopped_reason,
        "traded_markets": sorted(state.traded_markets),
    }


def _candidate_signal(payload: dict[str, Any], candidate: dict[str, Any]) -> EdgeSignal:
    price = float(candidate["price"])
    shares = float(candidate["shares"])
    return EdgeSignal(
        ticker=str(candidate["ticker"]),
        side=str(candidate["side"]),
        fair_price=1.0,
        ask_price=price,
        size=shares,
        edge=1.0 - price,
        cost=price * shares,
        expected_value=(1.0 - price) * shares,
        seconds_to_close=_float_or_none(payload.get("seconds_to_close")),
        reason="cf_final_window_live",
        net_edge=1.0 - price,
        net_expected_value=(1.0 - price) * shares,
    )


def _fresh_preflight(
    client: KalshiRestClient,
    signal: EdgeSignal,
    candidate: dict[str, Any],
    config: OpportunityLiveConfig,
) -> dict[str, Any]:
    book = client.get_orderbook(signal.ticker)
    orders, _ = client.get_orders(status="resting", limit=100)
    preflight = preflight_payload(
        preflight_limit_order(
            book=book,
            ticker=signal.ticker,
            side=signal.side,
            action="buy",
            price=float(candidate["price"]),
            count=float(candidate["shares"]),
            resting_orders=len(orders),
            max_cost=min(
                float(candidate.get("max_total_cost") or candidate["max_order_cost"]),
                float(candidate.get("remaining_budget") or config.max_run_cost)
                + float(candidate.get("estimated_fee") or 0.0)
                + max(0.0, config.fee_buffer),
            ),
        )
    )
    preflight["approved"] = bool(preflight["would_cross"]) and not preflight["warnings"]
    return preflight


def _live_safety_snapshot(client: KalshiRestClient) -> dict[str, Any]:
    account = client.get_balance()
    orders, _ = client.get_orders(status="resting", limit=100)
    return {
        "balance_dollars": _float_or_none(account.get("balance_dollars")),
        "portfolio_value": _float_or_none(account.get("portfolio_value")) or 0.0,
        "resting_orders": len(orders),
    }


async def _write_payload(path: Path, payload: dict[str, Any], lock: asyncio.Lock) -> None:
    async with lock:
        await asyncio.to_thread(_append_jsonl, path, payload)


def _append_jsonl(path: Path, payload: dict[str, Any]) -> None:
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(payload, sort_keys=True) + "\n")


def _console_payload(payload: dict[str, Any]) -> dict[str, Any]:
    liquidity = payload.get("liquidity") or {}
    ask = liquidity.get("ask") or {}
    return {
        "ts": payload.get("ts"),
        "asset": payload.get("symbol"),
        "ticker": (payload.get("market") or {}).get("ticker"),
        "zone": payload.get("zone"),
        "seconds_to_close": payload.get("seconds_to_close"),
        "side": liquidity.get("decision_side") or payload.get("early_side"),
        "executable": liquidity.get("executable"),
        "reject_reason": liquidity.get("reject_reason"),
        "ask_price": ask.get("price"),
        "ask_size": ask.get("size"),
    }


def _find_active_market(client: KalshiRestClient, series: str) -> Market | None:
    markets, _ = client.get_markets(series_ticker=series, status="open", limit=20)
    active = [market for market in markets if market.status in {"active", "open"}]
    if not active:
        return None
    return min(active, key=lambda market: market.close_time or datetime.max.replace(tzinfo=timezone.utc))


def _seconds_to_close(market: Market, now: datetime | None = None) -> float | None:
    if market.close_time is None:
        return None
    now = (now or datetime.now(timezone.utc)).astimezone(timezone.utc)
    return (market.close_time - now).total_seconds()


def _watch_zone(seconds_to_close: float | None, watch_seconds: float) -> str:
    if seconds_to_close is None or seconds_to_close > watch_seconds:
        return "outside"
    if seconds_to_close > 60:
        return "setup_120_60"
    if seconds_to_close > 10:
        return "cf_60_10"
    if seconds_to_close > 0:
        return "execution_10_0"
    return "closed"


def _projected_side(state: dict[str, Any]) -> str | None:
    projected = state.get("projected_average")
    threshold = state.get("threshold")
    if projected is None or threshold is None:
        return None
    return "YES" if float(projected) > float(threshold) else "NO"


def _book_payload(book: BinaryBook | None) -> dict[str, Any] | None:
    if book is None:
        return None
    return {
        "yes_bid": _level_payload(book.best_yes_bid),
        "yes_ask": _level_payload(book.best_yes_ask),
        "no_bid": _level_payload(book.best_no_bid),
        "no_ask": _level_payload(book.best_no_ask),
        "yes_spread": book.yes_spread,
    }


def _level_payload(level: Any) -> dict[str, float] | None:
    if level is None:
        return None
    return {"price": level.price, "size": level.size}


def _float_or_none(value: Any) -> float | None:
    if value in (None, ""):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def parse_watch_targets(value: str) -> tuple[OpportunityWatchTarget, ...]:
    targets: list[OpportunityWatchTarget] = []
    for item in value.split(","):
        text = item.strip()
        if not text:
            continue
        if ":" not in text:
            raise ValueError(f"Watch target must be SERIES:SYMBOL, got {text!r}")
        series, symbol = [part.strip() for part in text.split(":", 1)]
        targets.append(OpportunityWatchTarget(series=series, symbol=symbol))
    if not targets:
        raise ValueError("At least one watch target is required.")
    return tuple(targets)
