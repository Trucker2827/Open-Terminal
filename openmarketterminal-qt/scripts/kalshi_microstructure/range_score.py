from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Any

from .fees import rounded_fee


@dataclass(frozen=True)
class ScoredRangeOpportunity:
    ts: str
    series: str
    symbol: str
    ticker: str
    price: float
    shares: float
    cost: float
    fee: float
    payout: float
    pnl: float
    won: bool
    entry_spot: float
    final_spot: float
    strike_floor: float
    strike_cap: float
    seconds_to_close: float
    final_spot_age_seconds: float


def score_range_observer_log(
    path: Path,
    *,
    max_entry_price: float = 0.98,
    max_cost: float = 5.0,
    fee_rate: float = 0.07,
    max_seconds_to_close: float = 30.0 * 60.0,
    min_seconds_to_close: float = 0.0,
    max_final_spot_age_seconds: float = 180.0,
    dedupe_per_market: bool = True,
) -> list[ScoredRangeOpportunity]:
    snapshots = [row for row in _rows(path) if row.get("event") == "range_snapshot"]
    final_spots = _final_spots_by_series_close(snapshots)
    scored: list[ScoredRangeOpportunity] = []
    seen: set[str] = set()

    for snap in snapshots:
        ts = str(snap.get("recorded_at") or "")
        series = str(snap.get("series") or "")
        symbol = str(snap.get("symbol") or "")
        entry_spot = _float_or_none(snap.get("spot"))
        if not ts or not series or entry_spot is None:
            continue
        for bucket in snap.get("buckets") or []:
            ticker = str(bucket.get("ticker") or "")
            if not ticker or (dedupe_per_market and ticker in seen):
                continue
            if bucket.get("relation") != "inside":
                continue
            seconds_to_close = _float_or_none(bucket.get("seconds_to_close"))
            if seconds_to_close is None:
                continue
            if seconds_to_close < min_seconds_to_close or seconds_to_close > max_seconds_to_close:
                continue
            floor = _float_or_none(bucket.get("strike_floor"))
            cap = _float_or_none(bucket.get("strike_cap"))
            if floor is None or cap is None:
                continue
            book = bucket.get("book") or {}
            ask = book.get("best_yes_ask") or {}
            price = _float_or_none(ask.get("price"))
            size = _float_or_none(ask.get("size"))
            if price is None or size is None or price <= 0 or size <= 0 or price > max_entry_price:
                continue
            close_time = str(bucket.get("close_time") or "")
            final = final_spots.get((series, close_time))
            if final is None:
                continue
            final_spot, final_recorded_at = final
            final_age = _seconds_between(close_time, final_recorded_at)
            if final_age is None or final_age < 0 or final_age > max_final_spot_age_seconds:
                continue
            shares = min(size, max_cost / price)
            cost = shares * price
            fee = rounded_fee(price, shares, fee_rate)
            won = floor <= final_spot <= cap
            payout = shares if won else 0.0
            scored.append(
                ScoredRangeOpportunity(
                    ts=ts,
                    series=series,
                    symbol=symbol,
                    ticker=ticker,
                    price=price,
                    shares=shares,
                    cost=cost,
                    fee=fee,
                    payout=payout,
                    pnl=payout - cost - fee,
                    won=won,
                    entry_spot=entry_spot,
                    final_spot=final_spot,
                    strike_floor=floor,
                    strike_cap=cap,
                    seconds_to_close=seconds_to_close,
                    final_spot_age_seconds=final_age,
                )
            )
            seen.add(ticker)
    return scored


def scored_range_payload(scored: list[ScoredRangeOpportunity]) -> dict[str, Any]:
    wins = sum(1 for item in scored if item.won)
    pnl = sum(item.pnl for item in scored)
    cost = sum(item.cost + item.fee for item in scored)
    by_series: dict[str, dict[str, Any]] = {}
    for item in scored:
        row = by_series.setdefault(item.series, {"opportunities": 0, "wins": 0, "pnl": 0.0, "cost": 0.0})
        row["opportunities"] += 1
        row["wins"] += int(item.won)
        row["pnl"] += item.pnl
        row["cost"] += item.cost + item.fee
    for row in by_series.values():
        row["losses"] = row["opportunities"] - row["wins"]
        row["win_rate"] = row["wins"] / row["opportunities"] if row["opportunities"] else None
    return {
        "settled_proxy_opportunities": len(scored),
        "wins": wins,
        "losses": len(scored) - wins,
        "win_rate": (wins / len(scored)) if scored else None,
        "hypothetical_cost": cost,
        "hypothetical_pnl": pnl,
        "by_series": by_series,
        "opportunities": [asdict(item) for item in scored],
    }


def _final_spots_by_series_close(rows: list[dict[str, Any]]) -> dict[tuple[str, str], tuple[float, str]]:
    final: dict[tuple[str, str], tuple[float, str]] = {}
    for row in rows:
        series = str(row.get("series") or "")
        recorded_at = str(row.get("recorded_at") or "")
        spot = _float_or_none(row.get("spot"))
        if not series or not recorded_at or spot is None:
            continue
        close_times = {str(bucket.get("close_time") or "") for bucket in row.get("buckets") or []}
        for close_time in close_times:
            if not close_time:
                continue
            age = _seconds_between(close_time, recorded_at)
            if age is None or age < 0:
                continue
            key = (series, close_time)
            previous = final.get(key)
            if previous is None or recorded_at > previous[1]:
                final[key] = (spot, recorded_at)
    return final


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


def _seconds_between(later: str, earlier: str) -> float | None:
    try:
        later_dt = datetime.fromisoformat(later)
        earlier_dt = datetime.fromisoformat(earlier)
    except ValueError:
        return None
    return (later_dt - earlier_dt).total_seconds()
