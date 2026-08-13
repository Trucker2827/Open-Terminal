from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from .models import BinaryBook, Level


@dataclass
class KalshiBookCache:
    ticker: str
    yes: dict[float, float] = field(default_factory=dict)
    no: dict[float, float] = field(default_factory=dict)
    seq: int | None = None
    valid: bool = False
    gap_reason: str = "waiting for orderbook snapshot"

    def apply(self, message: dict[str, Any]) -> None:
        msg_type = message.get("type")
        msg = message.get("msg") or {}
        if not isinstance(msg, dict):
            return
        if msg.get("market_ticker") not in {None, self.ticker}:
            return

        if msg_type == "orderbook_snapshot":
            seq = _message_seq(message)
            if seq is None:
                self._invalidate("orderbook snapshot missing sequence")
                return
            self.yes = _levels_to_dict(
                msg.get("yes_dollars_fp") or msg.get("yes_dollars") or msg.get("yes") or []
            )
            self.no = _levels_to_dict(
                msg.get("no_dollars_fp") or msg.get("no_dollars") or msg.get("no") or []
            )
            self.seq = seq
            self.valid = True
            self.gap_reason = ""
            return

        if msg_type == "orderbook_delta":
            if not self.valid or self.seq is None:
                return
            seq = _message_seq(message)
            if seq is None:
                self._invalidate("orderbook delta missing sequence")
                return
            expected = self.seq + 1
            if seq != expected:
                self._invalidate(f"orderbook sequence gap: expected {expected}, got {seq}")
                return
            side = str(msg.get("side", "")).lower()
            if side not in {"yes", "no"}:
                return
            price = float(msg["price_dollars"])
            delta = float(msg["delta_fp"])
            ladder = self.yes if side == "yes" else self.no
            new_size = round(ladder.get(price, 0.0) + delta, 10)
            if new_size <= 0:
                ladder.pop(price, None)
            else:
                ladder[price] = new_size
            self.seq = seq

    def _invalidate(self, reason: str) -> None:
        # A dropped delta makes every later level suspect.  Empty the ladders
        # and wait for a fresh snapshot rather than silently trading stale data.
        self.yes.clear()
        self.no.clear()
        self.seq = None
        self.valid = False
        self.gap_reason = reason

    def to_book(self) -> BinaryBook:
        return BinaryBook(
            ticker=self.ticker,
            yes_bids=_dict_to_levels(self.yes),
            no_bids=_dict_to_levels(self.no),
        )


def _levels_to_dict(raw: list[list[str]]) -> dict[float, float]:
    return {float(price): float(size) for price, size in raw if float(size) > 0}


def _dict_to_levels(raw: dict[float, float]) -> tuple[Level, ...]:
    return tuple(
        Level(price=price, size=size)
        for price, size in sorted(raw.items(), key=lambda item: item[0], reverse=True)
        if size > 0
    )


def _message_seq(message: dict[str, Any]) -> int | None:
    value = message.get("seq")
    return int(value) if value is not None else None
