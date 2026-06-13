from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .fees import TAKER_FEE_RATE, rounded_fee
from .models import BinaryBook, Level


@dataclass(frozen=True)
class OrderPreflight:
    ticker: str
    side: str
    action: str
    price: float
    count: float
    notional_cost: float
    estimated_fee: float
    estimated_total_cost: float
    best_bid: float | None
    best_ask: float | None
    visible_at_or_better: float
    requested_vs_visible: float | None
    would_cross: bool
    resting_orders: int | None
    warnings: tuple[str, ...]


def preflight_limit_order(
    *,
    book: BinaryBook,
    ticker: str,
    side: str,
    action: str,
    price: float,
    count: float,
    fee_rate: float = TAKER_FEE_RATE,
    resting_orders: int | None = None,
    max_cost: float | None = None,
) -> OrderPreflight:
    side = side.upper()
    action = action.upper()
    notional_cost = price * count if action == "BUY" else 0.0
    fee = rounded_fee(price, count, fee_rate) if action == "BUY" else 0.0
    best_bid, best_ask = _best_bid_ask(book, side)
    visible = _visible_at_or_better(book, side, action, price)
    warnings: list[str] = []
    would_cross = _would_cross(action, price, best_bid, best_ask)
    if max_cost is not None and notional_cost + fee > max_cost:
        warnings.append("estimated_total_cost_exceeds_max_cost")
    if resting_orders:
        warnings.append("resting_orders_exist")
    if action == "BUY" and would_cross and visible < count:
        warnings.append("visible_depth_below_requested_count")
    if action == "BUY" and not would_cross:
        warnings.append("buy_order_would_rest")
    if action == "SELL" and not would_cross:
        warnings.append("sell_order_would_rest")

    return OrderPreflight(
        ticker=ticker,
        side=side,
        action=action,
        price=price,
        count=count,
        notional_cost=notional_cost,
        estimated_fee=fee,
        estimated_total_cost=notional_cost + fee,
        best_bid=best_bid,
        best_ask=best_ask,
        visible_at_or_better=visible,
        requested_vs_visible=(count / visible) if visible > 0 else None,
        would_cross=would_cross,
        resting_orders=resting_orders,
        warnings=tuple(warnings),
    )


def preflight_payload(preflight: OrderPreflight) -> dict[str, Any]:
    return {
        "ticker": preflight.ticker,
        "side": preflight.side,
        "action": preflight.action,
        "price": preflight.price,
        "count": preflight.count,
        "notional_cost": preflight.notional_cost,
        "estimated_fee": preflight.estimated_fee,
        "estimated_total_cost": preflight.estimated_total_cost,
        "best_bid": preflight.best_bid,
        "best_ask": preflight.best_ask,
        "visible_at_or_better": preflight.visible_at_or_better,
        "requested_vs_visible": preflight.requested_vs_visible,
        "would_cross": preflight.would_cross,
        "resting_orders": preflight.resting_orders,
        "warnings": list(preflight.warnings),
    }


def _best_bid_ask(book: BinaryBook, side: str) -> tuple[float | None, float | None]:
    if side == "YES":
        return _price(book.best_yes_bid), _price(book.best_yes_ask)
    return _price(book.best_no_bid), _price(book.best_no_ask)


def _visible_at_or_better(book: BinaryBook, side: str, action: str, price: float) -> float:
    if action == "BUY":
        asks = _ask_levels(book, side)
        return sum(level.size for level in asks if level.price <= price)
    bids = book.yes_bids if side == "YES" else book.no_bids
    return sum(level.size for level in bids if level.price >= price)


def _ask_levels(book: BinaryBook, side: str) -> tuple[Level, ...]:
    if side == "YES":
        return tuple(Level(price=round(1.0 - level.price, 10), size=level.size) for level in book.no_bids)
    return tuple(Level(price=round(1.0 - level.price, 10), size=level.size) for level in book.yes_bids)


def _would_cross(action: str, price: float, best_bid: float | None, best_ask: float | None) -> bool:
    if action == "BUY":
        return best_ask is not None and price >= best_ask
    return best_bid is not None and price <= best_bid


def _price(level: Level | None) -> float | None:
    return level.price if level is not None else None
