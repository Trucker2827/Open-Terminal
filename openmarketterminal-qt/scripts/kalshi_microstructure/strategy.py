from __future__ import annotations

import math
import re
from datetime import datetime, timezone

from .models import BinaryBook, EdgeSignal, Level, Market
from .fees import TAKER_FEE_RATE, fee_per_contract, net_expected_value

MONEY_RE = re.compile(r"\$?\s*([0-9]{1,3}(?:,[0-9]{3})*(?:\.[0-9]+)?|[0-9]+(?:\.[0-9]+)?)\s*(?:k|K)?")


def extract_numeric_threshold(text: str) -> float | None:
    candidates: list[float] = []
    for match in MONEY_RE.finditer(text):
        raw = match.group(0)
        number = float(match.group(1).replace(",", ""))
        if raw.strip().lower().endswith("k"):
            number *= 1000
        if number >= 100:
            candidates.append(number)
    if not candidates:
        return None
    return max(candidates)


def market_threshold(market: Market) -> float | None:
    strike_type, floor, cap = market_strike_bounds(market)
    if floor is not None:
        return floor
    if cap is not None:
        return cap

    if strike_type is not None:
        return None

    values = [
        market.title,
        market.subtitle,
        str(market.raw.get("yes_sub_title", "")),
        str(market.raw.get("no_sub_title", "")),
        str(market.raw.get("rules_primary", "")),
        str(market.raw.get("rules_secondary", "")),
    ]
    return extract_numeric_threshold(" ".join(values))


def market_strike_bounds(market: Market) -> tuple[str | None, float | None, float | None]:
    for key in ("floor_strike", "cap_strike", "strike"):
        value = market.raw.get(key)
        if value not in (None, ""):
            number = _float_or_none(value)
            if number is None:
                continue
            if key == "cap_strike":
                return None, None, number
            return None, number, None

    custom_strike = market.raw.get("custom_strike")
    if isinstance(custom_strike, dict):
        strike_type = str(custom_strike.get("strike_type", "")).lower()
        floor = _float_or_none(custom_strike.get("floor_strike"))
        cap = _float_or_none(custom_strike.get("cap_strike"))
        strike = _float_or_none(custom_strike.get("strike"))
        if strike is not None and floor is None and cap is None:
            floor = strike
        return strike_type or None, floor, cap
    elif custom_strike not in (None, ""):
        return None, _float_or_none(custom_strike), None

    return None, None, None


def _float_or_none(value: object) -> float | None:
    if value in (None, ""):
        return None
    return float(value)


def market_is_above_contract(text: str) -> bool:
    lowered = text.lower()
    above_terms = ("above", "higher than", "greater than", "over", "exceed", "at least", "price up")
    below_terms = ("below", "lower than", "less than", "under")
    above_pos = min((lowered.find(term) for term in above_terms if term in lowered), default=10**9)
    below_pos = min((lowered.find(term) for term in below_terms if term in lowered), default=10**9)
    return above_pos <= below_pos


def fair_probability_above(
    *,
    spot: float,
    threshold: float,
    seconds_to_close: float,
    annual_volatility: float,
) -> float:
    if seconds_to_close <= 0:
        return 1.0 if spot > threshold else 0.0
    seconds_per_year = 365.0 * 24.0 * 60.0 * 60.0
    sigma_t = annual_volatility * math.sqrt(seconds_to_close / seconds_per_year)
    if sigma_t <= 0:
        return 1.0 if spot > threshold else 0.0
    z = math.log(spot / threshold) / sigma_t
    return _normal_cdf(z)


def find_edge(
    *,
    market: Market,
    book: BinaryBook,
    spot: float,
    annual_volatility: float = 0.70,
    min_edge: float = 0.03,
    min_size: float = 1.0,
    fee_rate: float = TAKER_FEE_RATE,
    slippage_per_contract: float = 0.0,
    now: datetime | None = None,
) -> EdgeSignal | None:
    now = now or datetime.now(timezone.utc)
    threshold = market_threshold(market)
    if threshold is None:
        return None

    seconds_to_close = None
    if market.close_time is not None:
        seconds_to_close = max(0.0, (market.close_time - now).total_seconds())
    probability_above = fair_probability_above(
        spot=spot,
        threshold=threshold,
        seconds_to_close=seconds_to_close or 15 * 60,
        annual_volatility=annual_volatility,
    )
    fair_yes = probability_above if market_is_above_contract(market.text) else 1.0 - probability_above
    fair_no = 1.0 - fair_yes

    candidates = [
        _candidate(
            market,
            "YES",
            fair_yes,
            book.best_yes_ask,
            seconds_to_close,
            fee_rate,
            slippage_per_contract,
        ),
        _candidate(
            market,
            "NO",
            fair_no,
            book.best_no_ask,
            seconds_to_close,
            fee_rate,
            slippage_per_contract,
        ),
    ]
    valid = [
        signal
        for signal in candidates
        if signal is not None
        and (signal.net_edge if signal.net_edge is not None else signal.edge) >= min_edge
        and signal.size >= min_size
    ]
    if not valid:
        return None
    return max(valid, key=lambda signal: signal.expected_value)


def _candidate(
    market: Market,
    side: str,
    fair_price: float,
    ask: Level | None,
    seconds_to_close: float | None,
    fee_rate: float,
    slippage_per_contract: float,
) -> EdgeSignal | None:
    if ask is None:
        return None
    edge = fair_price - ask.price
    fee_each = fee_per_contract(ask.price, ask.size, fee_rate)
    net_edge = edge - fee_each - slippage_per_contract
    net_ev = net_expected_value(
        fair_price=fair_price,
        entry_price=ask.price,
        contracts=ask.size,
        fee_rate=fee_rate,
        slippage_per_contract=slippage_per_contract,
    )
    return EdgeSignal(
        ticker=market.ticker,
        side=side,
        fair_price=fair_price,
        ask_price=ask.price,
        size=ask.size,
        edge=edge,
        cost=ask.price * ask.size,
        expected_value=edge * ask.size,
        seconds_to_close=seconds_to_close,
        reason=(
            f"{side} ask {ask.price:.4f} vs estimated fair {fair_price:.4f}; "
            f"net edge {net_edge:.4f} after fees/slippage"
        ),
        fee_per_contract=fee_each,
        net_edge=net_edge,
        net_expected_value=net_ev,
        slippage_per_contract=slippage_per_contract,
    )


def _normal_cdf(value: float) -> float:
    return 0.5 * (1.0 + math.erf(value / math.sqrt(2.0)))
