from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Any

from .fees import TAKER_FEE_RATE, rounded_fee
from .settlement import MarketSettlement, payout_price


@dataclass(frozen=True)
class SkippedSignal:
    ticker: str
    side: str
    reason: str
    ts: str | None
    seconds_to_close: float | None
    ask_price: float
    fair_price: float | None
    edge: float | None
    net_edge: float | None
    available_cost: float | None
    asset: str | None = None


@dataclass(frozen=True)
class SkipOpportunity:
    ticker: str
    side: str
    reason: str
    ts: str | None
    seconds_to_close: float | None
    ask_price: float
    fair_price: float | None
    edge: float | None
    net_edge: float | None
    hypothetical_cost: float
    hypothetical_shares: float
    hypothetical_fee: float
    result_side: str | None
    finalized: bool
    won: bool | None
    hypothetical_pnl: float | None
    asset: str | None = None


@dataclass(frozen=True)
class SkipAnalysis:
    path: str
    skipped_signals: int
    opportunities: tuple[SkipOpportunity, ...]


def analyze_skipped_signals(
    path: Path,
    settlements: dict[str, MarketSettlement],
    *,
    max_hypothetical_cost: float = 2.0,
    fee_rate: float = TAKER_FEE_RATE,
    dedupe_seconds: float = 15.0,
) -> SkipAnalysis:
    raw = skipped_signals_from_log(path)
    representatives = dedupe_skipped_signals(raw, dedupe_seconds=dedupe_seconds)
    opportunities = tuple(
        _opportunity(
            signal,
            settlements.get(signal.ticker),
            max_hypothetical_cost=max_hypothetical_cost,
            fee_rate=fee_rate,
        )
        for signal in representatives
    )
    return SkipAnalysis(path=str(path), skipped_signals=len(raw), opportunities=opportunities)


def skipped_signals_from_log(path: Path) -> tuple[SkippedSignal, ...]:
    signals: list[SkippedSignal] = []
    for row in _rows(path):
        if row.get("event") != "signal_skipped":
            continue
        signal = row.get("signal") or {}
        if not signal.get("ticker") or not signal.get("side") or signal.get("ask_price") in (None, ""):
            continue
        signals.append(
            SkippedSignal(
                ticker=str(signal["ticker"]),
                side=str(signal["side"]).upper(),
                reason=str(row.get("reason") or "unknown"),
                ts=row.get("ts"),
                seconds_to_close=_float_or_none(signal.get("seconds_to_close")),
                ask_price=float(signal["ask_price"]),
                fair_price=_float_or_none(signal.get("fair_price")),
                edge=_float_or_none(signal.get("edge")),
                net_edge=_float_or_none(signal.get("net_edge")),
                available_cost=_float_or_none(signal.get("cost")),
                asset=_asset_symbol(row, str(signal["ticker"])),
            )
        )
    return tuple(signals)


def dedupe_skipped_signals(
    signals: tuple[SkippedSignal, ...],
    *,
    dedupe_seconds: float = 15.0,
) -> tuple[SkippedSignal, ...]:
    if dedupe_seconds <= 0:
        return signals
    buckets: dict[tuple[str, str, str, int], SkippedSignal] = {}
    for signal in signals:
        bucket = (
            signal.ticker,
            signal.side,
            signal.reason,
            int((_timestamp(signal.ts) or 0.0) // dedupe_seconds),
        )
        current = buckets.get(bucket)
        if current is None or _quality(signal) > _quality(current):
            buckets[bucket] = signal
    return tuple(sorted(buckets.values(), key=lambda item: item.ts or ""))


def skip_analysis_payload(analysis: SkipAnalysis, *, details: bool = False, top: int = 20) -> dict[str, Any]:
    opportunities = analysis.opportunities
    settled = [row for row in opportunities if row.finalized and row.hypothetical_pnl is not None]
    payload: dict[str, Any] = {
        "path": analysis.path,
        "skipped_signals_raw": analysis.skipped_signals,
        "opportunities_deduped": len(opportunities),
        "settled_opportunities": len(settled),
        "hypothetical_pnl": sum(row.hypothetical_pnl or 0.0 for row in settled),
        "hypothetical_wins": sum(1 for row in settled if row.won is True),
        "hypothetical_losses": sum(1 for row in settled if row.won is False),
        "by_reason": _group_payload(opportunities, key=lambda row: row.reason),
        "by_asset": _group_payload(opportunities, key=lambda row: row.asset or _series_from_ticker(row.ticker)),
    }
    sorted_rows = sorted(
        opportunities,
        key=lambda row: (
            row.hypothetical_pnl if row.hypothetical_pnl is not None else -999999.0,
            row.net_edge if row.net_edge is not None else row.edge or 0.0,
        ),
        reverse=True,
    )
    payload["top_opportunities"] = [asdict(row) for row in sorted_rows[: max(0, top)]]
    if details:
        payload["opportunities"] = [asdict(row) for row in opportunities]
    return payload


def focused_shadow_payload(
    analysis: SkipAnalysis,
    *,
    asset: str | None = "ETH-USD",
    side: str | None = None,
    max_entry_price: float | None = 0.50,
    max_seconds_to_close: float | None = 180.0,
    min_net_edge: float | None = None,
    min_settled_for_promotion: int = 10,
    min_win_rate_for_promotion: float = 0.65,
    min_pnl_for_promotion: float = 0.0,
    top: int = 5,
) -> dict[str, Any]:
    opportunities = tuple(
        row
        for row in analysis.opportunities
        if _matches_shadow_filter(
            row,
            asset=asset,
            side=side,
            max_entry_price=max_entry_price,
            max_seconds_to_close=max_seconds_to_close,
            min_net_edge=min_net_edge,
        )
    )
    settled = [row for row in opportunities if row.finalized and row.hypothetical_pnl is not None]
    wins = sum(1 for row in settled if row.won is True)
    losses = sum(1 for row in settled if row.won is False)
    pnl = sum(row.hypothetical_pnl or 0.0 for row in settled)
    promotion = _shadow_promotion_status(
        settled=len(settled),
        wins=wins,
        losses=losses,
        pnl=pnl,
        min_settled=min_settled_for_promotion,
        min_win_rate=min_win_rate_for_promotion,
        min_pnl=min_pnl_for_promotion,
    )
    sorted_rows = sorted(
        opportunities,
        key=lambda row: (
            row.hypothetical_pnl if row.hypothetical_pnl is not None else -999999.0,
            row.net_edge if row.net_edge is not None else row.edge or 0.0,
        ),
        reverse=True,
    )
    return {
        "asset": asset,
        "side": side.upper() if side else None,
        "max_entry_price": max_entry_price,
        "max_seconds_to_close": max_seconds_to_close,
        "min_net_edge": min_net_edge,
        "live_enabled": False,
        "promotion": promotion,
        "opportunities": len(opportunities),
        "settled": len(settled),
        "hypothetical_pnl": pnl,
        "wins": wins,
        "losses": losses,
        "by_side": _group_payload(opportunities, key=lambda row: row.side),
        "top_opportunities": [asdict(row) for row in sorted_rows[: max(0, top)]],
    }


def _shadow_promotion_status(
    *,
    settled: int,
    wins: int,
    losses: int,
    pnl: float,
    min_settled: int,
    min_win_rate: float,
    min_pnl: float,
) -> dict[str, Any]:
    total = wins + losses
    win_rate = wins / total if total else None
    if settled < min_settled:
        status = "collect_more_data"
    elif pnl <= min_pnl:
        status = "reject_negative_pnl"
    elif win_rate is None or win_rate < min_win_rate:
        status = "reject_low_win_rate"
    else:
        status = "candidate_passed_shadow"
    return {
        "status": status,
        "settled": settled,
        "min_settled": min_settled,
        "win_rate": win_rate,
        "min_win_rate": min_win_rate,
        "pnl": pnl,
        "min_pnl": min_pnl,
    }


def _matches_shadow_filter(
    row: SkipOpportunity,
    *,
    asset: str | None,
    side: str | None,
    max_entry_price: float | None,
    max_seconds_to_close: float | None,
    min_net_edge: float | None,
) -> bool:
    if asset is not None and row.asset != asset:
        return False
    if side is not None and row.side != side.upper():
        return False
    if max_entry_price is not None and row.ask_price > max_entry_price:
        return False
    if max_seconds_to_close is not None:
        if row.seconds_to_close is None or row.seconds_to_close > max_seconds_to_close:
            return False
    if min_net_edge is not None:
        edge = row.net_edge if row.net_edge is not None else row.edge
        if edge is None or edge < min_net_edge:
            return False
    return True


def _opportunity(
    signal: SkippedSignal,
    settlement: MarketSettlement | None,
    *,
    max_hypothetical_cost: float,
    fee_rate: float,
) -> SkipOpportunity:
    available_cost = signal.available_cost if signal.available_cost is not None else max_hypothetical_cost
    cost = max(0.0, min(max_hypothetical_cost, available_cost))
    shares = cost / signal.ask_price if signal.ask_price > 0 else 0.0
    fee = rounded_fee(signal.ask_price, shares, fee_rate) if shares > 0 else 0.0
    result_side = settlement.result_side if settlement else None
    finalized = bool(settlement and settlement.finalized and settlement.result_side)
    won = None
    pnl = None
    if finalized and result_side is not None:
        payout = shares * payout_price(signal.side, result_side)
        pnl = payout - cost - fee
        won = payout > 0
    return SkipOpportunity(
        ticker=signal.ticker,
        side=signal.side,
        reason=signal.reason,
        ts=signal.ts,
        seconds_to_close=signal.seconds_to_close,
        ask_price=signal.ask_price,
        fair_price=signal.fair_price,
        edge=signal.edge,
        net_edge=signal.net_edge,
        hypothetical_cost=cost,
        hypothetical_shares=shares,
        hypothetical_fee=fee,
        result_side=result_side,
        finalized=finalized,
        won=won,
        hypothetical_pnl=pnl,
        asset=signal.asset,
    )


def _group_payload(opportunities: tuple[SkipOpportunity, ...], *, key) -> dict[str, dict[str, Any]]:
    groups: dict[str, list[SkipOpportunity]] = {}
    for row in opportunities:
        groups.setdefault(str(key(row)), []).append(row)
    payload: dict[str, dict[str, Any]] = {}
    for name, rows in sorted(groups.items()):
        settled = [row for row in rows if row.finalized and row.hypothetical_pnl is not None]
        payload[name] = {
            "opportunities": len(rows),
            "settled": len(settled),
            "hypothetical_pnl": sum(row.hypothetical_pnl or 0.0 for row in settled),
            "wins": sum(1 for row in settled if row.won is True),
            "losses": sum(1 for row in settled if row.won is False),
            "avg_net_edge": _avg(row.net_edge for row in rows),
        }
    return payload


def _avg(values) -> float | None:
    parsed = [float(value) for value in values if value is not None]
    if not parsed:
        return None
    return sum(parsed) / len(parsed)


def _quality(signal: SkippedSignal) -> tuple[float, float]:
    net_edge = signal.net_edge if signal.net_edge is not None else signal.edge or 0.0
    available_cost = signal.available_cost if signal.available_cost is not None else 0.0
    return net_edge, available_cost


def _asset_symbol(row: dict[str, Any], ticker: str) -> str | None:
    asset = row.get("asset")
    if isinstance(asset, dict) and asset.get("symbol"):
        return str(asset["symbol"])
    return _symbol_from_ticker(ticker)


def _symbol_from_ticker(ticker: str) -> str | None:
    series = _series_from_ticker(ticker)
    mapping = {
        "KXBTC15M": "BTC-USD",
        "KXETH15M": "ETH-USD",
        "KXSOL15M": "SOL-USD",
        "KXDOGE15M": "DOGE-USD",
        "KXXRP15M": "XRP-USD",
    }
    return mapping.get(series)


def _series_from_ticker(ticker: str) -> str:
    return ticker.split("-", 1)[0]


def _timestamp(value: str | None) -> float | None:
    if not value:
        return None
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None


def _float_or_none(value: Any) -> float | None:
    if value in (None, ""):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _rows(path: Path):
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            if line.strip():
                yield json.loads(line)
