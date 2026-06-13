from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any


@dataclass(frozen=True)
class Level:
    price: float
    size: float


@dataclass(frozen=True)
class BinaryBook:
    ticker: str
    yes_bids: tuple[Level, ...]
    no_bids: tuple[Level, ...]

    @staticmethod
    def _best(levels: tuple[Level, ...]) -> Level | None:
        return max(levels, key=lambda level: level.price, default=None)

    @property
    def best_yes_bid(self) -> Level | None:
        return self._best(self.yes_bids)

    @property
    def best_no_bid(self) -> Level | None:
        return self._best(self.no_bids)

    @property
    def best_yes_ask(self) -> Level | None:
        best_no = self.best_no_bid
        if best_no is None:
            return None
        return Level(price=round(1.0 - best_no.price, 10), size=best_no.size)

    @property
    def best_no_ask(self) -> Level | None:
        best_yes = self.best_yes_bid
        if best_yes is None:
            return None
        return Level(price=round(1.0 - best_yes.price, 10), size=best_yes.size)

    @property
    def yes_spread(self) -> float | None:
        bid = self.best_yes_bid
        ask = self.best_yes_ask
        if bid is None or ask is None:
            return None
        return ask.price - bid.price


@dataclass(frozen=True)
class Market:
    ticker: str
    title: str
    subtitle: str
    status: str
    close_time: datetime | None
    raw: dict[str, Any]

    @classmethod
    def from_api(cls, payload: dict[str, Any]) -> "Market":
        return cls(
            ticker=str(payload.get("ticker", "")),
            title=str(payload.get("title", "")),
            subtitle=str(payload.get("subtitle", "")),
            status=str(payload.get("status", "")),
            close_time=_parse_time(
                payload.get("close_time")
                or payload.get("expiration_time")
                or payload.get("latest_expiration_time")
                or payload.get("expected_expiration_time")
            ),
            raw=payload,
        )

    @property
    def text(self) -> str:
        values = [
            self.ticker,
            self.title,
            self.subtitle,
            str(self.raw.get("rules_primary", "")),
            str(self.raw.get("rules_secondary", "")),
        ]
        return " ".join(value for value in values if value).strip()


@dataclass(frozen=True)
class EdgeSignal:
    ticker: str
    side: str
    fair_price: float
    ask_price: float
    size: float
    edge: float
    cost: float
    expected_value: float
    seconds_to_close: float | None
    reason: str
    fee_per_contract: float = 0.0
    net_edge: float | None = None
    net_expected_value: float | None = None
    slippage_per_contract: float = 0.0


def _parse_time(value: Any) -> datetime | None:
    if not value:
        return None
    if isinstance(value, datetime):
        return value.astimezone(timezone.utc)
    text = str(value).replace("Z", "+00:00")
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        return parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc)
