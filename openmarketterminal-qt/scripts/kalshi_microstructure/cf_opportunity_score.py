from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from .fees import rounded_fee
from .settlement import MarketSettlement, payout_price


@dataclass(frozen=True)
class ScoredCFOpportunity:
    ts: str
    ticker: str
    symbol: str | None
    side: str
    price: float
    shares: float
    cost: float
    fee: float
    payout: float
    pnl: float
    won: bool
    result_side: str
    tier: str | None
    seconds_to_close: float | None


def score_cf_opportunity_log(
    path: Path,
    settlements: dict[str, MarketSettlement],
    *,
    max_entry_price: float = 0.98,
    max_cost: float = 2.0,
    fee_rate: float = 0.07,
    dedupe_per_market: bool = True,
) -> list[ScoredCFOpportunity]:
    scored: list[ScoredCFOpportunity] = []
    seen: set[str] = set()
    for row in _rows(path):
        if row.get("event") != "cf_opportunity":
            continue
        liquidity = row.get("liquidity") or {}
        ask = liquidity.get("ask") or {}
        side = str(liquidity.get("decision_side") or "").upper()
        ticker = str((row.get("market") or {}).get("ticker") or "")
        price = _float_or_none(ask.get("price"))
        size = _float_or_none(ask.get("size"))
        if not ticker or side not in {"YES", "NO"} or price is None or size is None:
            continue
        if price <= 0 or price > max_entry_price or size <= 0:
            continue
        if dedupe_per_market and ticker in seen:
            continue
        settlement = settlements.get(ticker)
        if settlement is None or not settlement.finalized or settlement.result_side is None:
            continue
        shares = min(size, max_cost / price)
        cost = shares * price
        fee = rounded_fee(price, shares, fee_rate)
        payout = shares * payout_price(side, settlement.result_side)
        scored.append(
            ScoredCFOpportunity(
                ts=str(row.get("ts") or ""),
                ticker=ticker,
                symbol=row.get("symbol"),
                side=side,
                price=price,
                shares=shares,
                cost=cost,
                fee=fee,
                payout=payout,
                pnl=payout - cost - fee,
                won=side == settlement.result_side,
                result_side=settlement.result_side,
                tier=(row.get("candidate") or {}).get("tier"),
                seconds_to_close=_float_or_none(row.get("seconds_to_close")),
            )
        )
        seen.add(ticker)
    return scored


def scored_cf_payload(scored: list[ScoredCFOpportunity]) -> dict[str, Any]:
    wins = sum(1 for item in scored if item.won)
    pnl = sum(item.pnl for item in scored)
    cost = sum(item.cost + item.fee for item in scored)
    return {
        "settled_opportunities": len(scored),
        "wins": wins,
        "losses": len(scored) - wins,
        "win_rate": (wins / len(scored)) if scored else None,
        "hypothetical_cost": cost,
        "hypothetical_pnl": pnl,
        "opportunities": [asdict(item) for item in scored],
    }


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
