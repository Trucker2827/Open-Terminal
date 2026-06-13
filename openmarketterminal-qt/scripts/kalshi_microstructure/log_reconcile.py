from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from .fees import TAKER_FEE_RATE, rounded_fee
from .settlement import MarketSettlement, payout_price


@dataclass(frozen=True)
class SettledPaperOrder:
    ticker: str
    side: str
    shares: float
    cost: float
    fee: float
    payout: float
    pnl: float
    ts: str | None


@dataclass(frozen=True)
class BotLogAudit:
    path: str
    started: int
    stopped: int
    scans: int
    paper_orders: int
    live_preflights: int
    live_preflight_blocks: int
    live_submissions: int
    live_results: int
    live_filled_orders: int
    live_unfilled_orders: int
    live_fill_count: float
    live_fill_cost: float
    live_fees: float
    resting_order_warnings: int
    errors: int
    final_spent_this_run: float | None
    final_cash: float | None
    tickers: tuple[str, ...]


def settle_bot_log(
    path: Path,
    settlement: MarketSettlement,
    *,
    fee_rate: float = TAKER_FEE_RATE,
) -> list[SettledPaperOrder]:
    if settlement.result_side is None:
        raise ValueError(f"Market {settlement.ticker} has no result side.")

    settled: list[SettledPaperOrder] = []
    for row in _rows(path):
        if row.get("event") != "paper_order":
            continue
        signal = row.get("signal") or {}
        if signal.get("ticker") != settlement.ticker:
            continue
        side = str(signal["side"]).upper()
        shares = float(row["shares"])
        cost = float(row["cost"])
        price = float(signal["ask_price"])
        fee = float(row.get("fee") if row.get("fee") is not None else rounded_fee(price, shares, fee_rate))
        payout = shares * payout_price(side, settlement.result_side)
        settled.append(
            SettledPaperOrder(
                ticker=settlement.ticker,
                side=side,
                shares=shares,
                cost=cost,
                fee=fee,
                payout=payout,
                pnl=payout - cost - fee,
                ts=row.get("ts"),
            )
        )
    return settled


def settled_payload(orders: list[SettledPaperOrder]) -> dict[str, Any]:
    return {
        "orders": len(orders),
        "realized_pnl": sum(order.pnl for order in orders),
        "wins": sum(1 for order in orders if order.pnl > 0),
        "losses": sum(1 for order in orders if order.pnl < 0),
        "flat": sum(1 for order in orders if order.pnl == 0),
        "order_details": [asdict(order) for order in orders],
    }


def audit_bot_log(path: Path) -> BotLogAudit:
    started = stopped = scans = paper_orders = 0
    live_preflights = live_preflight_blocks = live_submissions = live_results = 0
    live_filled_orders = live_unfilled_orders = 0
    live_fill_count = live_fill_cost = live_fees = 0.0
    resting_order_warnings = errors = 0
    final_spent_this_run: float | None = None
    final_cash: float | None = None
    tickers: set[str] = set()
    result_order_ids: set[str] = set()
    submitted_fills: list[dict[str, float | str | None]] = []

    rows = list(_rows(path))
    for row in rows:
        if row.get("event") != "live_order_result":
            continue
        fill = row.get("fill") or {}
        order_id = fill.get("order_id")
        if order_id:
            result_order_ids.add(str(order_id))

    for row in rows:
        event = row.get("event")
        signal = row.get("signal") or {}
        if signal.get("ticker"):
            tickers.add(str(signal["ticker"]))
        if row.get("ticker"):
            tickers.add(str(row["ticker"]))

        if event == "bot_started":
            started += 1
        elif event == "bot_stopped":
            stopped += 1
            final_spent_this_run = _float_or_none(row.get("spent_this_run"))
            final_cash = _float_or_none(row.get("cash"))
        elif event in {"scan", "ws_scan"}:
            scans += 1
        elif event == "paper_order":
            paper_orders += 1
        elif event == "live_preflight":
            live_preflights += 1
            preflight = row.get("preflight") or {}
            warnings = preflight.get("warnings") or []
            if not preflight.get("approved"):
                live_preflight_blocks += 1
            if "resting_orders_exist" in warnings:
                resting_order_warnings += 1
        elif event == "live_order_submitted":
            live_submissions += 1
            submitted_fill = _fill_from_submission(row)
            if submitted_fill and submitted_fill.get("order_id") not in result_order_ids:
                submitted_fills.append(submitted_fill)
        elif event == "live_order_result":
            live_results += 1
            fill = row.get("fill") or {}
            fill_count = float(fill.get("fill_count") or 0.0)
            fill_cost = float(fill.get("fill_cost") or 0.0)
            fees = float(fill.get("fees") or 0.0)
            live_fill_count += fill_count
            live_fill_cost += fill_cost
            live_fees += fees
            if fill_count > 0:
                live_filled_orders += 1
            else:
                live_unfilled_orders += 1
        elif event == "signal_skipped":
            if row.get("reason") == "live_preflight" and not row.get("preflight"):
                live_preflight_blocks += 1
        elif event in {"error", "websocket_error"}:
            errors += 1

    for fill in submitted_fills:
        live_results += 1
        fill_count = float(fill.get("fill_count") or 0.0)
        fill_cost = float(fill.get("fill_cost") or 0.0)
        fees = float(fill.get("fees") or 0.0)
        live_fill_count += fill_count
        live_fill_cost += fill_cost
        live_fees += fees
        if fill_count > 0:
            live_filled_orders += 1
        else:
            live_unfilled_orders += 1

    return BotLogAudit(
        path=str(path),
        started=started,
        stopped=stopped,
        scans=scans,
        paper_orders=paper_orders,
        live_preflights=live_preflights,
        live_preflight_blocks=live_preflight_blocks,
        live_submissions=live_submissions,
        live_results=live_results,
        live_filled_orders=live_filled_orders,
        live_unfilled_orders=live_unfilled_orders,
        live_fill_count=live_fill_count,
        live_fill_cost=live_fill_cost,
        live_fees=live_fees,
        resting_order_warnings=resting_order_warnings,
        errors=errors,
        final_spent_this_run=final_spent_this_run,
        final_cash=final_cash,
        tickers=tuple(sorted(tickers)),
    )


def audit_payload(audit: BotLogAudit) -> dict[str, Any]:
    return asdict(audit)


def _rows(path: Path):
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            if line.strip():
                yield json.loads(line)


def _float_or_none(value: Any) -> float | None:
    if value in (None, ""):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _fill_from_submission(row: dict[str, Any]) -> dict[str, float | str | None] | None:
    response = row.get("response") or {}
    order = response.get("order") if isinstance(response, dict) else None
    if not isinstance(order, dict) or "fill_count_fp" not in order:
        return None
    return {
        "order_id": order.get("order_id"),
        "fill_count": _float_or_zero(order.get("fill_count_fp")),
        "fill_cost": _float_or_zero(order.get("taker_fill_cost_dollars"))
        + _float_or_zero(order.get("maker_fill_cost_dollars")),
        "fees": _float_or_zero(order.get("taker_fees_dollars"))
        + _float_or_zero(order.get("maker_fees_dollars")),
    }


def _float_or_zero(value: Any) -> float:
    parsed = _float_or_none(value)
    return parsed if parsed is not None else 0.0
