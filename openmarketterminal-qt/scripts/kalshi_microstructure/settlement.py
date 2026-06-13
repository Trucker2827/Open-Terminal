from __future__ import annotations

from dataclasses import dataclass

from .models import Market
from .strategy import market_threshold


@dataclass(frozen=True)
class MarketSettlement:
    ticker: str
    status: str
    result_side: str | None
    expiration_value: float | None
    threshold: float | None

    @property
    def finalized(self) -> bool:
        return self.status == "finalized" and self.result_side in {"YES", "NO"}


def settlement_from_market(market: Market) -> MarketSettlement:
    return MarketSettlement(
        ticker=market.ticker,
        status=market.status,
        result_side=_result_side(market),
        expiration_value=_float_or_none(market.raw.get("expiration_value")),
        threshold=market_threshold(market),
    )


def payout_price(side: str, result_side: str) -> float:
    return 1.0 if side.upper() == result_side.upper() else 0.0


def _result_side(market: Market) -> str | None:
    result = str(market.raw.get("result") or "").strip().upper()
    if result in {"YES", "NO"}:
        return result
    return None


def _float_or_none(value: object) -> float | None:
    if value in (None, ""):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None
