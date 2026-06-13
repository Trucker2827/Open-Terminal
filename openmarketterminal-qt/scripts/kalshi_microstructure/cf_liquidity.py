from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .book_cache import KalshiBookCache
from .models import BinaryBook, Level


@dataclass(frozen=True)
class CFLiquidityConfig:
    max_entry_price: float = 0.98
    max_seconds_remaining: float = 60.0
    min_observed_count: int = 55
    max_remaining_count: int | None = 5
    min_required_gap: float = 0.0
    min_projected_gap: float = 0.0
    require_locked_side: bool = False
    allowed_side: str | None = None
    min_size: float = 1.0
    max_threshold_latest_ratio: float = 1.25


@dataclass
class CFLiquiditySummary:
    cf_final_states: int = 0
    yes_states: int = 0
    no_states: int = 0
    ask_seen: int = 0
    executable: int = 0
    reject_reasons: dict[str, int] = field(default_factory=dict)
    examples: list[dict[str, Any]] = field(default_factory=list)

    def add(self, payload: dict[str, Any], *, max_examples: int = 5) -> None:
        if payload.get("evaluated"):
            self.cf_final_states += 1
            side = payload.get("decision_side")
            if side == "YES":
                self.yes_states += 1
            elif side == "NO":
                self.no_states += 1
        if payload.get("ask") is not None:
            self.ask_seen += 1
        if payload.get("executable"):
            self.executable += 1
        reason = str(payload.get("reject_reason") or "")
        if reason:
            self.reject_reasons[reason] = self.reject_reasons.get(reason, 0) + 1
        if len(self.examples) < max_examples and (payload.get("executable") or payload.get("evaluated")):
            self.examples.append(payload)

    def payload(self) -> dict[str, Any]:
        return {
            "cf_final_states": self.cf_final_states,
            "yes_states": self.yes_states,
            "no_states": self.no_states,
            "ask_seen": self.ask_seen,
            "executable": self.executable,
            "reject_reasons": dict(sorted(self.reject_reasons.items())),
            "examples": self.examples,
        }


def cf_liquidity_payload(
    state: dict[str, Any] | None,
    book: BinaryBook | None,
    config: CFLiquidityConfig | None = None,
) -> dict[str, Any]:
    config = config or CFLiquidityConfig()
    if state is None:
        return _blocked("no_state")
    if book is None:
        return _blocked("no_book")

    observed_count = int(state.get("observed_count") or 0)
    remaining_count = int(state.get("remaining_count") or 0)
    seconds_remaining = _optional_float(state.get("seconds_remaining"))
    threshold = _optional_float(state.get("threshold"))
    projected = _optional_float(state.get("projected_average"))
    latest = _optional_float(state.get("latest_value"))
    required_yes = _optional_float(state.get("required_remaining_average_for_yes"))
    locked_side = _clean_side(state.get("locked_side"))

    common = {
        "observed_count": observed_count,
        "remaining_count": remaining_count,
        "seconds_remaining": seconds_remaining,
        "threshold": threshold,
        "projected_average": projected,
        "latest_value": latest,
        "required_remaining_average_for_yes": required_yes,
        "locked_side": locked_side,
    }

    if observed_count < config.min_observed_count:
        return _blocked("too_early", **common)
    if seconds_remaining is None or seconds_remaining > config.max_seconds_remaining:
        return _blocked("too_much_time_remaining", **common)
    if config.max_remaining_count is not None and remaining_count > config.max_remaining_count:
        return _blocked("too_many_remaining_ticks", **common)
    if threshold is None or projected is None:
        return _blocked("missing_projection", **common)
    if latest is not None and not _threshold_matches_latest(threshold, latest, config.max_threshold_latest_ratio):
        return _blocked("threshold_mismatch", **common)
    if abs(projected - threshold) < config.min_projected_gap:
        return _blocked("projected_gap_too_small", **common)

    side = locked_side
    reason = "cf_locked_side"
    if side is None and not config.require_locked_side:
        side = "YES" if projected > threshold else "NO"
        reason = "cf_projected_final_window"
    if side is None:
        return _blocked("no_locked_side", evaluated=True, **common)
    if config.allowed_side and side != config.allowed_side.upper():
        return _blocked("side_not_allowed", evaluated=True, decision_side=side, decision_reason=reason, **common)

    if reason != "cf_locked_side" and config.min_required_gap > 0:
        if required_yes is None or latest is None:
            return _blocked("missing_required_gap_inputs", evaluated=True, decision_side=side, decision_reason=reason, **common)
        gap = latest - required_yes if side == "YES" else required_yes - latest
        if gap < config.min_required_gap:
            return _blocked(
                "required_gap_too_small",
                evaluated=True,
                decision_side=side,
                decision_reason=reason,
                required_gap=gap,
                **common,
            )

    ask = book.best_yes_ask if side == "YES" else book.best_no_ask
    ask_payload = _level_payload(ask)
    if ask is None:
        return _blocked("no_ask", evaluated=True, decision_side=side, decision_reason=reason, **common)
    if ask.price > config.max_entry_price:
        return _blocked(
            "ask_too_expensive",
            evaluated=True,
            decision_side=side,
            decision_reason=reason,
            ask=ask_payload,
            **common,
        )
    if ask.size < config.min_size:
        return _blocked(
            "ask_too_small",
            evaluated=True,
            decision_side=side,
            decision_reason=reason,
            ask=ask_payload,
            **common,
        )
    return {
        "status": "candidate",
        "evaluated": True,
        "executable": True,
        "decision_side": side,
        "decision_reason": reason,
        "ask": ask_payload,
        "entry_price": ask.price,
        "entry_size": ask.size,
        **common,
    }


def summarize_cf_liquidity_recording(
    path: Path,
    config: CFLiquidityConfig | None = None,
) -> CFLiquiditySummary:
    config = config or CFLiquidityConfig()
    cache: KalshiBookCache | None = None
    summary = CFLiquiditySummary()
    for row in _rows(path):
        event = row.get("event")
        if event == "session_started":
            ticker = str((row.get("market") or {}).get("ticker") or "")
            cache = KalshiBookCache(ticker) if ticker else None
            continue
        if event == "kalshi" and cache is not None:
            cache.apply(row.get("message") or {})
            continue
        if event != "cf" or cache is None:
            continue
        payload = cf_liquidity_payload(row.get("settlement_state"), cache.to_book(), config)
        summary.add(payload)
    return summary


def aggregate_cf_liquidity_summaries(summaries: list[CFLiquiditySummary]) -> dict[str, Any]:
    aggregate = CFLiquiditySummary()
    for summary in summaries:
        aggregate.cf_final_states += summary.cf_final_states
        aggregate.yes_states += summary.yes_states
        aggregate.no_states += summary.no_states
        aggregate.ask_seen += summary.ask_seen
        aggregate.executable += summary.executable
        for reason, count in summary.reject_reasons.items():
            aggregate.reject_reasons[reason] = aggregate.reject_reasons.get(reason, 0) + count
        for example in summary.examples:
            if len(aggregate.examples) < 10:
                aggregate.examples.append(example)
    return aggregate.payload()


def _blocked(reason: str, **values: Any) -> dict[str, Any]:
    return {
        "status": "blocked",
        "evaluated": bool(values.pop("evaluated", False)),
        "executable": False,
        "reject_reason": reason,
        **values,
    }


def _level_payload(level: Level | None) -> dict[str, float] | None:
    if level is None:
        return None
    return {"price": level.price, "size": level.size}


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
