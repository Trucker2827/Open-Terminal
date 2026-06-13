from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from .book_cache import KalshiBookCache
from .execution import simulate_taker_buy
from .fees import TAKER_FEE_RATE, rounded_fee
from .models import BinaryBook, EdgeSignal, Market
from .settlement import MarketSettlement, payout_price
from .strategy import find_edge


@dataclass(frozen=True)
class BacktestConfig:
    path: Path
    annual_volatility: float = 0.70
    min_edge: float = 0.03
    min_size: float = 1.0
    max_order_cost: float = 2.0
    max_trades: int = 10
    hold_seconds: float = 5.0
    cooldown_seconds: float = 10.0
    fee_rate: float = TAKER_FEE_RATE
    slippage_per_contract: float = 0.0
    use_recorded_volatility: bool = False
    liquidity_fraction: float = 1.0
    queue_ahead_contracts: float = 0.0
    min_fill_cost: float = 0.01


@dataclass
class ReplayPosition:
    ticker: str
    side: str
    shares: float
    entry_price: float
    entry_cost: float
    entry_fee: float
    entry_time: datetime
    entry_signal: EdgeSignal


@dataclass
class ReplayTrade:
    ticker: str
    side: str
    shares: float
    entry_price: float
    exit_price: float
    entry_fee: float
    exit_fee: float
    entry_time: str
    exit_time: str
    pnl: float
    reason: str


@dataclass
class BacktestResult:
    trades: list[ReplayTrade]
    open_positions: int
    realized_pnl: float
    messages: int
    scans: int


@dataclass(frozen=True)
class CFDecisionConfig:
    path: Path
    max_order_cost: float = 5.0
    max_trades: int = 1
    max_entry_price: float = 0.98
    max_seconds_remaining: float = 60.0
    min_observed_count: int = 55
    max_remaining_count: int | None = 5
    min_required_gap: float = 0.0
    min_projected_gap: float = 0.0
    require_locked_side: bool = False
    allowed_side: str | None = None
    cooldown_seconds: float = 5.0
    fee_rate: float = TAKER_FEE_RATE
    liquidity_fraction: float = 1.0
    queue_ahead_contracts: float = 0.0
    min_fill_cost: float = 0.01
    one_trade_per_market: bool = True
    max_threshold_latest_ratio: float = 1.25


@dataclass
class CFDecisionPosition:
    ticker: str
    side: str
    shares: float
    entry_price: float
    entry_cost: float
    entry_fee: float
    entry_time: datetime
    observed_count: int
    remaining_count: int
    seconds_remaining: float
    projected_average: float
    threshold: float
    latest_value: float | None
    required_remaining_average_for_yes: float | None
    reason: str


@dataclass
class CFDecisionTrade:
    ticker: str
    side: str
    final_side: str
    shares: float
    entry_price: float
    entry_cost: float
    entry_fee: float
    entry_time: str
    exit_time: str
    pnl: float
    reason: str
    observed_count: int
    remaining_count: int
    seconds_remaining: float
    projected_average: float
    threshold: float
    latest_value: float | None
    required_remaining_average_for_yes: float | None


@dataclass
class CFDecisionResult:
    trades: list[CFDecisionTrade]
    open_positions: int
    realized_pnl: float
    messages: int
    scans: int


def replay_recording(config: BacktestConfig) -> BacktestResult:
    market: Market | None = None
    cache: KalshiBookCache | None = None
    spot: float | None = None
    annual_volatility = config.annual_volatility
    open_positions: list[ReplayPosition] = []
    trades: list[ReplayTrade] = []
    last_entry_at: datetime | None = None
    messages = 0
    scans = 0

    for row in _rows(config.path):
        messages += 1
        now = _row_time(row)
        event = row.get("event")

        if event == "session_started":
            market = _market_from_record(row["market"])
            cache = KalshiBookCache(market.ticker)
            continue

        if event == "spot":
            spot = float(row["spot"])
            if config.use_recorded_volatility and row.get("annual_volatility") is not None:
                annual_volatility = float(row["annual_volatility"])
            continue

        if event != "kalshi" or market is None or cache is None:
            continue

        message = row.get("message") or {}
        cache.apply(message)
        if message.get("type") not in {"orderbook_snapshot", "orderbook_delta"}:
            continue

        book = cache.to_book()
        closed, open_positions = _close_due_positions(open_positions, book, now, config)
        trades.extend(closed)

        if spot is None or len(trades) + len(open_positions) >= config.max_trades:
            continue
        if last_entry_at and (now - last_entry_at).total_seconds() < config.cooldown_seconds:
            continue

        signal = find_edge(
            market=market,
            book=book,
            spot=spot,
            annual_volatility=annual_volatility,
            min_edge=config.min_edge,
            min_size=config.min_size,
            fee_rate=config.fee_rate,
            slippage_per_contract=config.slippage_per_contract,
            now=now,
        )
        scans += 1
        if signal is None:
            continue

        fill = simulate_taker_buy(
            signal,
            max_cost=config.max_order_cost,
            remaining_budget=config.max_order_cost,
            available_cash=config.max_order_cost * config.max_trades,
            fee_rate=config.fee_rate,
            liquidity_fraction=config.liquidity_fraction,
            queue_ahead_contracts=config.queue_ahead_contracts,
            min_fill_cost=config.min_fill_cost,
        )
        if fill is None:
            continue
        gross_edge = (signal.fair_price - signal.ask_price - config.slippage_per_contract) * fill.shares
        if gross_edge - fill.fee <= 0:
            continue
        open_positions.append(
            ReplayPosition(
                ticker=signal.ticker,
                side=signal.side,
                shares=fill.shares,
                entry_price=signal.ask_price,
                entry_cost=fill.cost,
                entry_fee=fill.fee,
                entry_time=now,
                entry_signal=signal,
            )
        )
        last_entry_at = now

    if cache is not None:
        book = cache.to_book()
        final_time = datetime.now(timezone.utc)
        closed, open_positions = _close_all(open_positions, book, final_time, "end_of_replay", config)
        trades.extend(closed)

    return BacktestResult(
        trades=trades,
        open_positions=len(open_positions),
        realized_pnl=sum(trade.pnl for trade in trades),
        messages=messages,
        scans=scans,
    )


def replay_recording_to_settlement(
    config: BacktestConfig,
    settlement: MarketSettlement,
) -> BacktestResult:
    if not settlement.finalized or settlement.result_side is None:
        raise ValueError(f"Market {settlement.ticker} is not finalized.")

    market: Market | None = None
    cache: KalshiBookCache | None = None
    spot: float | None = None
    annual_volatility = config.annual_volatility
    positions: list[ReplayPosition] = []
    last_entry_at: datetime | None = None
    messages = 0
    scans = 0

    for row in _rows(config.path):
        messages += 1
        now = _row_time(row)
        event = row.get("event")

        if event == "session_started":
            market = _market_from_record(row["market"])
            cache = KalshiBookCache(market.ticker)
            continue

        if event == "spot":
            spot = float(row["spot"])
            if config.use_recorded_volatility and row.get("annual_volatility") is not None:
                annual_volatility = float(row["annual_volatility"])
            continue

        if event != "kalshi" or market is None or cache is None:
            continue

        message = row.get("message") or {}
        cache.apply(message)
        if message.get("type") not in {"orderbook_snapshot", "orderbook_delta"}:
            continue

        if spot is None or len(positions) >= config.max_trades:
            continue
        if last_entry_at and (now - last_entry_at).total_seconds() < config.cooldown_seconds:
            continue

        signal = find_edge(
            market=market,
            book=cache.to_book(),
            spot=spot,
            annual_volatility=annual_volatility,
            min_edge=config.min_edge,
            min_size=config.min_size,
            fee_rate=config.fee_rate,
            slippage_per_contract=config.slippage_per_contract,
            now=now,
        )
        scans += 1
        if signal is None:
            continue

        fill = simulate_taker_buy(
            signal,
            max_cost=config.max_order_cost,
            remaining_budget=config.max_order_cost,
            available_cash=config.max_order_cost * config.max_trades,
            fee_rate=config.fee_rate,
            liquidity_fraction=config.liquidity_fraction,
            queue_ahead_contracts=config.queue_ahead_contracts,
            min_fill_cost=config.min_fill_cost,
        )
        if fill is None:
            continue
        gross_edge = (signal.fair_price - signal.ask_price - config.slippage_per_contract) * fill.shares
        if gross_edge - fill.fee <= 0:
            continue
        positions.append(
            ReplayPosition(
                ticker=signal.ticker,
                side=signal.side,
                shares=fill.shares,
                entry_price=signal.ask_price,
                entry_cost=fill.cost,
                entry_fee=fill.fee,
                entry_time=now,
                entry_signal=signal,
            )
        )
        last_entry_at = now

    final_time = datetime.now(timezone.utc)
    trades = [
        _settle_position(position, settlement.result_side, final_time)
        for position in positions
    ]
    return BacktestResult(
        trades=trades,
        open_positions=0,
        realized_pnl=sum(trade.pnl for trade in trades),
        messages=messages,
        scans=scans,
    )


def replay_cf_final_window(config: CFDecisionConfig) -> CFDecisionResult:
    market: Market | None = None
    cache: KalshiBookCache | None = None
    latest_state: dict[str, Any] | None = None
    final_side: str | None = None
    positions: list[CFDecisionPosition] = []
    last_entry_at: datetime | None = None
    traded_tickers: set[str] = set()
    messages = 0
    scans = 0

    for row in _rows(config.path):
        messages += 1
        now = _row_time(row)
        event = row.get("event")

        if event == "session_started":
            market = _market_from_record(row["market"])
            cache = KalshiBookCache(market.ticker)
            continue

        if event == "cf":
            state = row.get("settlement_state")
            if isinstance(state, dict):
                latest_state = state
                final_side = _final_side_from_cf_state(state) or final_side
            if market is not None and cache is not None:
                position = _cf_decision_entry(
                    market=market,
                    book=cache.to_book(),
                    state=latest_state,
                    now=now,
                    config=config,
                    current_trades=len(positions),
                    last_entry_at=last_entry_at,
                    traded_tickers=traded_tickers,
                )
                scans += 1
                if position is not None:
                    positions.append(position)
                    last_entry_at = now
                    traded_tickers.add(position.ticker)
            continue

        if event != "kalshi" or market is None or cache is None:
            continue

        message = row.get("message") or {}
        cache.apply(message)
        if message.get("type") not in {"orderbook_snapshot", "orderbook_delta"}:
            continue

        position = _cf_decision_entry(
            market=market,
            book=cache.to_book(),
            state=latest_state,
            now=now,
            config=config,
            current_trades=len(positions),
            last_entry_at=last_entry_at,
            traded_tickers=traded_tickers,
        )
        scans += 1
        if position is not None:
            positions.append(position)
            last_entry_at = now
            traded_tickers.add(position.ticker)

    if final_side is None:
        return CFDecisionResult(
            trades=[],
            open_positions=len(positions),
            realized_pnl=0.0,
            messages=messages,
            scans=scans,
        )

    exit_time = datetime.now(timezone.utc)
    trades = [_settle_cf_decision_position(position, final_side, exit_time) for position in positions]
    return CFDecisionResult(
        trades=trades,
        open_positions=0,
        realized_pnl=sum(trade.pnl for trade in trades),
        messages=messages,
        scans=scans,
    )


def result_payload(result: BacktestResult) -> dict[str, Any]:
    return {
        "messages": result.messages,
        "scans": result.scans,
        "trades": len(result.trades),
        "open_positions": result.open_positions,
        "realized_pnl": result.realized_pnl,
        "wins": sum(1 for trade in result.trades if trade.pnl > 0),
        "losses": sum(1 for trade in result.trades if trade.pnl < 0),
        "flat": sum(1 for trade in result.trades if trade.pnl == 0),
        "trade_details": [asdict(trade) for trade in result.trades],
    }


def cf_decision_payload(result: CFDecisionResult) -> dict[str, Any]:
    return {
        "messages": result.messages,
        "scans": result.scans,
        "trades": len(result.trades),
        "open_positions": result.open_positions,
        "realized_pnl": result.realized_pnl,
        "wins": sum(1 for trade in result.trades if trade.pnl > 0),
        "losses": sum(1 for trade in result.trades if trade.pnl < 0),
        "flat": sum(1 for trade in result.trades if trade.pnl == 0),
        "trade_details": [asdict(trade) for trade in result.trades],
    }


def _close_due_positions(
    positions: list[ReplayPosition],
    book: BinaryBook,
    now: datetime,
    config: BacktestConfig,
) -> tuple[list[ReplayTrade], list[ReplayPosition]]:
    closed: list[ReplayTrade] = []
    kept: list[ReplayPosition] = []
    for position in positions:
        if (now - position.entry_time).total_seconds() >= config.hold_seconds:
            closed.append(_close_position(position, book, now, "hold_elapsed", config))
        else:
            kept.append(position)
    return closed, kept


def _close_all(
    positions: list[ReplayPosition],
    book: BinaryBook,
    now: datetime,
    reason: str,
    config: BacktestConfig,
) -> tuple[list[ReplayTrade], list[ReplayPosition]]:
    return [_close_position(position, book, now, reason, config) for position in positions], []


def _close_position(
    position: ReplayPosition,
    book: BinaryBook,
    now: datetime,
    reason: str,
    config: BacktestConfig,
) -> ReplayTrade:
    exit_level = book.best_yes_bid if position.side == "YES" else book.best_no_bid
    exit_price = exit_level.price if exit_level is not None else 0.0
    exit_fee = rounded_fee(exit_price, position.shares, config.fee_rate)
    pnl = position.shares * exit_price - position.entry_cost - position.entry_fee - exit_fee
    return ReplayTrade(
        ticker=position.ticker,
        side=position.side,
        shares=position.shares,
        entry_price=position.entry_price,
        exit_price=exit_price,
        entry_fee=position.entry_fee,
        exit_fee=exit_fee,
        entry_time=position.entry_time.isoformat(),
        exit_time=now.isoformat(),
        pnl=pnl,
        reason=reason,
    )


def _settle_position(position: ReplayPosition, result_side: str, now: datetime) -> ReplayTrade:
    exit_price = payout_price(position.side, result_side)
    pnl = position.shares * exit_price - position.entry_cost - position.entry_fee
    return ReplayTrade(
        ticker=position.ticker,
        side=position.side,
        shares=position.shares,
        entry_price=position.entry_price,
        exit_price=exit_price,
        entry_fee=position.entry_fee,
        exit_fee=0.0,
        entry_time=position.entry_time.isoformat(),
        exit_time=now.isoformat(),
        pnl=pnl,
        reason=f"settlement_{result_side.lower()}",
    )


def _settle_cf_decision_position(position: CFDecisionPosition, final_side: str, now: datetime) -> CFDecisionTrade:
    payout = position.shares if position.side == final_side else 0.0
    pnl = payout - position.entry_cost - position.entry_fee
    return CFDecisionTrade(
        ticker=position.ticker,
        side=position.side,
        final_side=final_side,
        shares=position.shares,
        entry_price=position.entry_price,
        entry_cost=position.entry_cost,
        entry_fee=position.entry_fee,
        entry_time=position.entry_time.isoformat(),
        exit_time=now.isoformat(),
        pnl=pnl,
        reason=f"cf_settlement_{final_side.lower()}",
        observed_count=position.observed_count,
        remaining_count=position.remaining_count,
        seconds_remaining=position.seconds_remaining,
        projected_average=position.projected_average,
        threshold=position.threshold,
        latest_value=position.latest_value,
        required_remaining_average_for_yes=position.required_remaining_average_for_yes,
    )


def _cf_decision_entry(
    *,
    market: Market,
    book: BinaryBook,
    state: dict[str, Any] | None,
    now: datetime,
    config: CFDecisionConfig,
    current_trades: int,
    last_entry_at: datetime | None,
    traded_tickers: set[str],
) -> CFDecisionPosition | None:
    if state is None or current_trades >= config.max_trades:
        return None
    if config.one_trade_per_market and market.ticker in traded_tickers:
        return None
    if last_entry_at and (now - last_entry_at).total_seconds() < config.cooldown_seconds:
        return None

    decision = _cf_decision_side(state, config)
    if decision is None:
        return None
    side, reason = decision
    ask = book.best_yes_ask if side == "YES" else book.best_no_ask
    if ask is None or ask.price > config.max_entry_price:
        return None

    signal = EdgeSignal(
        ticker=market.ticker,
        side=side,
        fair_price=1.0,
        ask_price=ask.price,
        size=ask.size,
        edge=1.0 - ask.price,
        cost=ask.price * ask.size,
        expected_value=(1.0 - ask.price) * ask.size,
        seconds_to_close=_optional_float(state.get("seconds_remaining")),
        reason=reason,
        fee_per_contract=0.0,
        net_edge=1.0 - ask.price,
        net_expected_value=(1.0 - ask.price) * ask.size,
    )
    fill = simulate_taker_buy(
        signal,
        max_cost=config.max_order_cost,
        remaining_budget=config.max_order_cost,
        available_cash=config.max_order_cost * max(1, config.max_trades),
        fee_rate=config.fee_rate,
        liquidity_fraction=config.liquidity_fraction,
        queue_ahead_contracts=config.queue_ahead_contracts,
        min_fill_cost=config.min_fill_cost,
    )
    if fill is None:
        return None
    if (fill.shares * (1.0 - ask.price)) - fill.fee <= 0:
        return None

    projected = _optional_float(state.get("projected_average"))
    threshold = _optional_float(state.get("threshold"))
    if projected is None or threshold is None:
        return None

    return CFDecisionPosition(
        ticker=market.ticker,
        side=side,
        shares=fill.shares,
        entry_price=ask.price,
        entry_cost=fill.cost,
        entry_fee=fill.fee,
        entry_time=now,
        observed_count=int(state.get("observed_count") or 0),
        remaining_count=int(state.get("remaining_count") or 0),
        seconds_remaining=float(state.get("seconds_remaining") or 0.0),
        projected_average=projected,
        threshold=threshold,
        latest_value=_optional_float(state.get("latest_value")),
        required_remaining_average_for_yes=_optional_float(state.get("required_remaining_average_for_yes")),
        reason=reason,
    )


def _cf_decision_side(state: dict[str, Any], config: CFDecisionConfig) -> tuple[str, str] | None:
    observed_count = int(state.get("observed_count") or 0)
    remaining_count = int(state.get("remaining_count") or 0)
    seconds_remaining = _optional_float(state.get("seconds_remaining"))
    threshold = _optional_float(state.get("threshold"))
    projected = _optional_float(state.get("projected_average"))
    latest = _optional_float(state.get("latest_value"))
    required_yes = _optional_float(state.get("required_remaining_average_for_yes"))
    locked_side = _clean_side(state.get("locked_side"))

    if observed_count < config.min_observed_count:
        return None
    if seconds_remaining is None or seconds_remaining > config.max_seconds_remaining:
        return None
    if config.max_remaining_count is not None and remaining_count > config.max_remaining_count:
        return None
    if threshold is None or projected is None:
        return None
    if latest is not None and not _threshold_matches_latest(threshold, latest, config.max_threshold_latest_ratio):
        return None
    if abs(projected - threshold) < config.min_projected_gap:
        return None

    side = locked_side
    reason = "cf_locked_side"
    if side is None and not config.require_locked_side:
        side = "YES" if projected > threshold else "NO"
        reason = "cf_projected_final_window"
    if side is None:
        return None
    if config.allowed_side and side != config.allowed_side.upper():
        return None

    if reason != "cf_locked_side" and config.min_required_gap > 0:
        if required_yes is None or latest is None:
            return None
        gap = latest - required_yes if side == "YES" else required_yes - latest
        if gap < config.min_required_gap:
            return None

    return side, reason


def _final_side_from_cf_state(state: dict[str, Any]) -> str | None:
    locked_side = _clean_side(state.get("locked_side"))
    if locked_side:
        return locked_side
    observed_count = int(state.get("observed_count") or 0)
    target_count = int(state.get("target_count") or 60)
    if observed_count < target_count:
        return None
    threshold = _optional_float(state.get("threshold"))
    projected = _optional_float(state.get("projected_average"))
    if threshold is None or projected is None:
        return None
    return "YES" if projected > threshold else "NO"


def _clean_side(value: Any) -> str | None:
    if value is None:
        return None
    side = str(value).upper()
    if side not in {"YES", "NO"}:
        return None
    return side


def _optional_float(value: Any) -> float | None:
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _threshold_matches_latest(threshold: float, latest: float, max_ratio: float) -> bool:
    if threshold <= 0 or latest <= 0 or max_ratio <= 1:
        return False
    ratio = max(threshold / latest, latest / threshold)
    return ratio <= max_ratio


def _rows(path: Path):
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            if line.strip():
                yield json.loads(line)


def _row_time(row: dict[str, Any]) -> datetime:
    value = row.get("recorded_at")
    if value:
        return datetime.fromisoformat(str(value))
    return datetime.now(timezone.utc)


def _market_from_record(payload: dict[str, Any]) -> Market:
    raw = dict(payload.get("raw") or {})
    return Market.from_api(
        {
            **raw,
            "ticker": payload["ticker"],
            "title": payload.get("title") or raw.get("title") or "",
            "subtitle": payload.get("subtitle") or raw.get("subtitle") or "",
            "status": payload.get("status") or raw.get("status") or "open",
            "close_time": payload.get("close_time") or raw.get("close_time"),
        }
    )
