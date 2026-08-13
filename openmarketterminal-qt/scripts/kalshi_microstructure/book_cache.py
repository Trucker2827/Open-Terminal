from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from .models import BinaryBook, Level


class ReconnectRequired(RuntimeError):
    """The current WebSocket cannot produce a trustworthy book again."""


@dataclass
class KalshiBookCache:
    """One market's ladder; sequence continuity belongs to its subscription."""

    ticker: str
    # True only when this cache sees every sequenced message for its sid (the
    # existing callers subscribe exactly one market per connection).  A
    # multi-market subscription must use KalshiSubscriptionBookCache instead.
    validate_sequence: bool = False
    yes: dict[float, float] = field(default_factory=dict)
    no: dict[float, float] = field(default_factory=dict)
    sid: int | None = None
    seq: int | None = None
    ts_ms: int | None = None
    valid: bool = False
    gap_reason: str = "waiting for orderbook snapshot"

    def apply(self, message: dict[str, Any]) -> None:
        msg_type = message.get("type")
        if self.validate_sequence:
            if not self._advance_subscription_sequence(message):
                return
        msg = message.get("msg") or {}
        if not isinstance(msg, dict):
            self._fail_reconnect("malformed orderbook message body")
        if msg.get("market_ticker") not in {None, self.ticker}:
            return

        # Control frames (including ``ok``) consume the sid's sequence but do
        # not mutate a book. ``subscribed`` has no sid/seq and returns above.
        if msg_type not in {"orderbook_snapshot", "orderbook_delta"}:
            return

        if msg_type == "orderbook_snapshot":
            seq = _message_seq(message)
            if seq is None:
                self._fail_reconnect("orderbook snapshot missing sequence")
            try:
                self.yes = _levels_to_dict(
                    msg.get("yes_dollars_fp") or msg.get("yes_dollars") or msg.get("yes") or []
                )
                self.no = _levels_to_dict(
                    msg.get("no_dollars_fp") or msg.get("no_dollars") or msg.get("no") or []
                )
                self.ts_ms = _message_ts_ms(msg)
            except (KeyError, TypeError, ValueError, OverflowError) as exc:
                self._fail_reconnect(f"malformed orderbook snapshot: {exc}")
            self.seq = seq
            self.valid = True
            self.gap_reason = ""
            return

        if msg_type == "orderbook_delta":
            if not self.valid or self.seq is None:
                self._fail_reconnect(f"orderbook delta preceded snapshot: {self.ticker}")
            seq = _message_seq(message)
            if seq is None:
                self._fail_reconnect("orderbook delta missing sequence")
            side = str(msg.get("side", "")).lower()
            if side not in {"yes", "no"}:
                self._fail_reconnect(f"malformed orderbook delta: unrecognised side {side!r}")
            try:
                price = float(msg["price_dollars"])
                delta = float(msg["delta_fp"])
                ts_ms = _message_ts_ms(msg)
            except (KeyError, TypeError, ValueError, OverflowError) as exc:
                self._fail_reconnect(f"malformed orderbook delta: {exc}")
            ladder = self.yes if side == "yes" else self.no
            new_size = round(ladder.get(price, 0.0) + delta, 10)
            if new_size <= 0:
                ladder.pop(price, None)
            else:
                ladder[price] = new_size
            self.seq = seq
            self.ts_ms = ts_ms

    def _advance_subscription_sequence(self, message: dict[str, Any]) -> bool:
        sid_raw = message.get("sid")
        seq_raw = message.get("seq")
        if sid_raw is None and seq_raw is None:
            return False
        if sid_raw is None or seq_raw is None:
            self._fail_reconnect("sequenced frame missing sid or seq")
        try:
            sid = int(sid_raw)
            seq = int(seq_raw)
        except (TypeError, ValueError, OverflowError) as exc:
            self._fail_reconnect(f"malformed subscription sequence: {exc}")
        if self.sid is None:
            self.sid = sid
        elif sid != self.sid:
            self._fail_reconnect(f"subscription id changed: expected {self.sid}, got {sid}")
        if self.seq is not None and seq != self.seq + 1:
            self._fail_reconnect(f"subscription sequence gap: expected {self.seq + 1}, got {seq}")
        self.seq = seq
        return True

    def _fail_reconnect(self, reason: str) -> None:
        self._invalidate(reason)
        raise ReconnectRequired(reason)

    def _invalidate(self, reason: str) -> None:
        # A dropped delta makes every later level suspect. Empty the ladders;
        # the transport must replace this socket because re-subscribe on the
        # same Kalshi connection does not replay snapshots.
        self.yes.clear()
        self.no.clear()
        self.ts_ms = None
        self.valid = False
        self.gap_reason = reason

    def to_book(self) -> BinaryBook:
        return BinaryBook(
            ticker=self.ticker,
            yes_bids=_dict_to_levels(self.yes),
            no_bids=_dict_to_levels(self.no),
        )


@dataclass
class KalshiSubscriptionBookCache:
    """Coherent books for one Kalshi subscription id (``sid``).

    Kalshi's ``seq`` is shared by every market in a subscription.  A gap can
    therefore have affected any member and invalidates the entire group.
    Recovery requires constructing a new subscription cache from fresh
    snapshots; deltas never repair an invalid group.
    """

    tickers: tuple[str, ...]
    books: dict[str, KalshiBookCache] = field(init=False)
    sid: int | None = None
    seq: int | None = None
    valid: bool = True
    gap_reason: str = ""

    def __post_init__(self) -> None:
        if not self.tickers or len(set(self.tickers)) != len(self.tickers):
            raise ValueError("subscription tickers must be non-empty and unique")
        self.books = {
            ticker: KalshiBookCache(ticker, validate_sequence=False)
            for ticker in self.tickers
        }

    def apply(self, message: dict[str, Any]) -> None:
        if not self.valid:
            raise ReconnectRequired(self.gap_reason or "subscription cache is invalid")
        sid_raw = message.get("sid")
        seq_raw = message.get("seq")
        # The initial ``subscribed`` response is explicitly outside the sid
        # sequence. All sid-tagged frames—including controls—advance it.
        if sid_raw is None and seq_raw is None:
            return
        if sid_raw is None or seq_raw is None:
            self._fail_reconnect("sequenced frame missing sid or seq")
        try:
            sid = int(sid_raw)
            seq = int(seq_raw)
        except (TypeError, ValueError, OverflowError) as exc:
            self._fail_reconnect(f"malformed subscription sequence: {exc}")
        if self.sid is None:
            self.sid = sid
        elif sid != self.sid:
            self._fail_reconnect(f"subscription id changed: expected {self.sid}, got {sid}")
        if self.seq is not None and seq != self.seq + 1:
            self._fail_reconnect(f"subscription sequence gap: expected {self.seq + 1}, got {seq}")
        self.seq = seq

        if message.get("type") not in {"orderbook_snapshot", "orderbook_delta"}:
            return
        msg = message.get("msg") or {}
        if not isinstance(msg, dict):
            self._fail_reconnect("malformed orderbook message body")
        ticker = str(msg.get("market_ticker") or "")
        market = self.books.get(ticker)
        if market is None:
            self._fail_reconnect(f"unexpected market in subscription: {ticker or '<missing>'}")
        if message.get("type") == "orderbook_delta" and not self.books[ticker].valid:
            self._fail_reconnect(f"orderbook delta preceded snapshot: {ticker}")
        try:
            market.apply(message)
        except ReconnectRequired as exc:
            self._fail_reconnect(str(exc))
        if not market.valid:
            self._fail_reconnect(market.gap_reason)

    @property
    def ready(self) -> bool:
        return self.valid and all(book.valid for book in self.books.values())

    def coherent_within(self, max_span_ms: int) -> bool:
        """Whether every member has a timestamp within one exchange-time band."""
        if not self.ready or max_span_ms < 0:
            return False
        timestamps = [book.ts_ms for book in self.books.values()]
        if any(value is None for value in timestamps):
            return False
        values = [value for value in timestamps if value is not None]
        return max(values) - min(values) <= max_span_ms

    def to_books(self) -> dict[str, BinaryBook]:
        return (
            {ticker: book.to_book() for ticker, book in self.books.items()}
            if self.ready else {}
        )

    def _invalidate_all(self, reason: str) -> None:
        for book in self.books.values():
            book._invalidate(reason)
        self.valid = False
        self.gap_reason = reason

    def _fail_reconnect(self, reason: str) -> None:
        self._invalidate_all(reason)
        raise ReconnectRequired(reason)


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


def _message_ts_ms(message: dict[str, Any]) -> int | None:
    value = message.get("ts_ms")
    return int(value) if value is not None else None
