"""Read-only, certificate-driven Kalshi structural-arbitrage measurement.

Kalshi exposes one reciprocal binary book: a NO bid at ``p`` is the YES ask
at ``1-p``.  This module never treats YES and NO bids as two independent asks.

Nothing here places orders.  A bundle is evaluated only when an explicit
payoff certificate enumerates every allowed settlement state.  Market titles
are deliberately never parsed to infer exclusivity or exhaustiveness.
"""
from __future__ import annotations

import hashlib
import json
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from decimal import Decimal, ROUND_CEILING
from itertools import combinations
from pathlib import Path
from typing import Any, Mapping

from .kalshi import KalshiRestClient
from .models import BinaryBook, Level, Market


SCHEMA_VERSION = 1
CENT = Decimal("0.01")
ONE = Decimal("1")
ZERO = Decimal("0")
GENERAL_TAKER_RATE = Decimal("0.07")
BTC_THRESHOLD_CORRIDOR_FAMILY = "btc_threshold_corridor"
CORRIDOR_COMPARISONS = {"greater_than", "greater_than_or_equal"}
API_STRIKE_COMPARISONS = {
    "greater": "greater_than",
    "greater_than": "greater_than",
    "greater_equal": "greater_than_or_equal",
    "greater_than_or_equal": "greater_than_or_equal",
}
SETTLEMENT_TIME_FIELDS = {
    "close_time",
    "expiration_time",
    "expected_expiration_time",
    "latest_expiration_time",
}
SETTLEMENT_TERM_FIELDS = (
    "ticker",
    "event_ticker",
    "series_ticker",
    "rules_primary",
    "rules_secondary",
    "strike_type",
    "floor_strike",
    "cap_strike",
    "custom_strike",
    "functional_strike",
    "settlement_source_url",
    "settlement_sources",
    "close_time",
    "expiration_time",
    "expected_expiration_time",
    "latest_expiration_time",
    "early_close_condition",
    "can_close_early",
    "settlement_timer_seconds",
    "response_price_units",
    "notional_value",
)


@dataclass(frozen=True)
class ReadOnlyKalshiClient:
    """Narrow facade: the scanner cannot name an order mutation operation."""

    client: KalshiRestClient

    def get_market(self, ticker: str) -> Market:
        return self.client.get_market(ticker)

    def get_events(
        self,
        *,
        status: str = "open",
        limit: int = 200,
        cursor: str | None = None,
        with_nested_markets: bool = True,
    ) -> tuple[list[dict[str, Any]], str | None]:
        return self.client.get_events(
            status=status,
            limit=limit,
            cursor=cursor,
            with_nested_markets=with_nested_markets,
        )

    def get_series(self, ticker: str) -> dict[str, Any]:
        return self.client.get_series(ticker)

    def get_orderbooks(self, tickers: list[str]) -> dict[str, BinaryBook]:
        return self.client.get_orderbooks(tickers)


def _decimal(value: Any, field: str) -> Decimal:
    try:
        result = Decimal(str(value))
    except Exception as exc:  # noqa: BLE001 - produce a certificate error.
        raise ValueError(f"{field} must be numeric") from exc
    if not result.is_finite():
        raise ValueError(f"{field} must be finite")
    return result


@dataclass(frozen=True)
class CertifiedLeg:
    ticker: str
    side: str
    payouts: tuple[Decimal, ...]

    def payload(self) -> dict[str, object]:
        return {
            "ticker": self.ticker,
            "side": self.side,
            "payouts": [str(value) for value in self.payouts],
        }


@dataclass(frozen=True)
class PayoffCertificate:
    bundle_id: str
    description: str
    outcomes: tuple[str, ...]
    legs: tuple[CertifiedLeg, ...]

    @classmethod
    def from_payload(cls, payload: Mapping[str, Any]) -> "PayoffCertificate":
        if payload.get("schema_version") != SCHEMA_VERSION:
            raise ValueError(f"schema_version must be {SCHEMA_VERSION}")
        bundle_id = str(payload.get("bundle_id") or "").strip()
        description = str(payload.get("description") or "").strip()
        outcomes_raw = payload.get("outcomes")
        legs_raw = payload.get("legs")
        if not bundle_id or not description:
            raise ValueError("bundle_id and description are required")
        if not isinstance(outcomes_raw, list) or not outcomes_raw:
            raise ValueError("outcomes must be a non-empty list")
        outcomes = tuple(str(value).strip() for value in outcomes_raw)
        if any(not value for value in outcomes) or len(set(outcomes)) != len(outcomes):
            raise ValueError("outcomes must be non-empty and unique")
        if not isinstance(legs_raw, list) or len(legs_raw) < 2:
            raise ValueError("a structural bundle requires at least two legs")

        legs: list[CertifiedLeg] = []
        identities: set[tuple[str, str]] = set()
        for index, raw in enumerate(legs_raw):
            if not isinstance(raw, Mapping):
                raise ValueError(f"legs[{index}] must be an object")
            ticker = str(raw.get("ticker") or "").strip()
            side = str(raw.get("side") or "").strip().lower()
            payouts_raw = raw.get("payouts")
            if not ticker or side not in {"yes", "no"}:
                raise ValueError(f"legs[{index}] needs a ticker and yes/no side")
            if (ticker, side) in identities:
                raise ValueError(f"duplicate certified leg: {ticker} {side}")
            if not isinstance(payouts_raw, list) or len(payouts_raw) != len(outcomes):
                raise ValueError(f"legs[{index}].payouts must match outcomes")
            payouts = tuple(
                _decimal(value, f"legs[{index}].payouts") for value in payouts_raw
            )
            if any(value not in {ZERO, ONE} for value in payouts):
                raise ValueError("version 1 certificates allow only binary 0/1 payouts")
            identities.add((ticker, side))
            legs.append(CertifiedLeg(ticker=ticker, side=side, payouts=payouts))

        certificate = cls(
            bundle_id=bundle_id,
            description=description,
            outcomes=outcomes,
            legs=tuple(legs),
        )
        by_identity = {(leg.ticker, leg.side): leg for leg in certificate.legs}
        for ticker in certificate.tickers:
            yes = by_identity.get((ticker, "yes"))
            no = by_identity.get((ticker, "no"))
            if yes is not None and no is not None and any(
                yes_value + no_value != ONE
                for yes_value, no_value in zip(yes.payouts, no.payouts)
            ):
                raise ValueError(f"same-market YES/NO payouts must be complementary: {ticker}")
        if certificate.guaranteed_payout <= ZERO:
            raise ValueError("payoff matrix does not prove a positive payout in every outcome")
        return certificate

    @property
    def tickers(self) -> tuple[str, ...]:
        return tuple(dict.fromkeys(leg.ticker for leg in self.legs))

    @property
    def guaranteed_payout(self) -> Decimal:
        totals = [sum((leg.payouts[index] for leg in self.legs), ZERO)
                  for index in range(len(self.outcomes))]
        return min(totals)

    def payload(self) -> dict[str, object]:
        return {
            "schema_version": SCHEMA_VERSION,
            "bundle_id": self.bundle_id,
            "description": self.description,
            "outcomes": list(self.outcomes),
            "legs": [leg.payload() for leg in self.legs],
        }

    @property
    def digest(self) -> str:
        encoded = json.dumps(self.payload(), sort_keys=True, separators=(",", ":")).encode()
        return hashlib.sha256(encoded).hexdigest()


def settlement_terms_payload(payload: Mapping[str, Any]) -> dict[str, Any]:
    """Return the non-quote fields bound by a corridor review.

    Hashing the complete market response would make bids, status, volume and
    other volatile data invalidate a review.  This explicit allow-list binds
    the settlement and strike fields that give the pair its payoff shape.
    Adding a newly relevant API field therefore requires a schema review,
    rather than silently broadening an old certificate.
    """
    return {field: payload.get(field) for field in SETTLEMENT_TERM_FIELDS}


def settlement_terms_sha256(payload: Mapping[str, Any]) -> str:
    return _canonical_digest(settlement_terms_payload(payload))


@dataclass(frozen=True)
class CorridorMember:
    ticker: str
    strike: Decimal
    terms_sha256: str

    def payload(self) -> dict[str, str]:
        return {
            "ticker": self.ticker,
            "strike": str(self.strike),
            "terms_sha256": self.terms_sha256,
        }


@dataclass(frozen=True)
class BtcThresholdCorridorCertificate:
    """Reviewed BTC threshold ladder; never inferred from market titles."""

    event_ticker: str
    underlier: str
    comparison: str
    api_strike_type: str
    settlement_time_field: str
    settlement_time: str
    reviewed_at: str
    members: tuple[CorridorMember, ...]

    @classmethod
    def from_payload(
        cls, payload: Mapping[str, Any]
    ) -> "BtcThresholdCorridorCertificate":
        if payload.get("schema_version") != SCHEMA_VERSION:
            raise ValueError(f"schema_version must be {SCHEMA_VERSION}")
        if payload.get("family") != BTC_THRESHOLD_CORRIDOR_FAMILY:
            raise ValueError(f"family must be {BTC_THRESHOLD_CORRIDOR_FAMILY}")
        if payload.get("rules_reviewed") is not True:
            raise ValueError("rules_reviewed must be explicitly true")
        if payload.get("ordinary_binary_payouts_only") is not True:
            raise ValueError("ordinary_binary_payouts_only must be explicitly true")

        event_ticker = str(payload.get("event_ticker") or "").strip()
        underlier = str(payload.get("underlier") or "").strip().upper()
        comparison = str(payload.get("comparison") or "").strip().lower()
        api_strike_type = str(payload.get("api_strike_type") or "").strip().lower()
        settlement_time_field = str(payload.get("settlement_time_field") or "").strip()
        settlement_time = str(payload.get("settlement_time") or "").strip()
        reviewed_at = str(payload.get("reviewed_at") or "").strip()
        if underlier != "BTC":
            raise ValueError("underlier must be BTC")
        if not event_ticker or not settlement_time or not reviewed_at or not api_strike_type:
            raise ValueError(
                "event_ticker, api_strike_type, settlement_time, and reviewed_at are required"
            )
        if comparison not in CORRIDOR_COMPARISONS:
            raise ValueError(f"unsupported comparison: {comparison or 'missing'}")
        if API_STRIKE_COMPARISONS.get(api_strike_type) != comparison:
            raise ValueError("api_strike_type does not match the certified comparison")
        if settlement_time_field not in SETTLEMENT_TIME_FIELDS:
            raise ValueError("settlement_time_field is not a supported exact API field")

        members_raw = payload.get("markets")
        if not isinstance(members_raw, list) or len(members_raw) < 2:
            raise ValueError("a corridor family requires at least two reviewed markets")
        members: list[CorridorMember] = []
        tickers: set[str] = set()
        strikes: set[Decimal] = set()
        for index, raw in enumerate(members_raw):
            if not isinstance(raw, Mapping):
                raise ValueError(f"markets[{index}] must be an object")
            ticker = str(raw.get("ticker") or "").strip()
            strike = _decimal(raw.get("strike"), f"markets[{index}].strike")
            terms_sha256 = str(raw.get("terms_sha256") or "").strip().lower()
            if not ticker or strike <= ZERO:
                raise ValueError(f"markets[{index}] needs a ticker and positive strike")
            if len(terms_sha256) != 64 or any(c not in "0123456789abcdef" for c in terms_sha256):
                raise ValueError(f"markets[{index}].terms_sha256 must be lowercase SHA-256")
            if ticker in tickers or strike in strikes:
                raise ValueError("corridor market tickers and strikes must be unique")
            tickers.add(ticker)
            strikes.add(strike)
            members.append(CorridorMember(ticker, strike, terms_sha256))

        return cls(
            event_ticker=event_ticker,
            underlier=underlier,
            comparison=comparison,
            api_strike_type=api_strike_type,
            settlement_time_field=settlement_time_field,
            settlement_time=settlement_time,
            reviewed_at=reviewed_at,
            members=tuple(sorted(members, key=lambda member: member.strike)),
        )

    @property
    def tickers(self) -> tuple[str, ...]:
        return tuple(member.ticker for member in self.members)

    @property
    def pairs(self) -> tuple[tuple[CorridorMember, CorridorMember], ...]:
        return tuple(combinations(self.members, 2))

    def pair_certificate(
        self, lower: CorridorMember, higher: CorridorMember
    ) -> PayoffCertificate:
        if lower.strike >= higher.strike:
            raise ValueError("corridor lower strike must be below higher strike")
        if self.comparison == "greater_than_or_equal":
            outcomes = (
                "below_lower",
                "at_or_above_lower_below_higher",
                "at_or_above_higher",
            )
        else:
            outcomes = (
                "at_or_below_lower",
                "above_lower_at_or_below_higher",
                "above_higher",
            )
        return PayoffCertificate.from_payload({
            "schema_version": SCHEMA_VERSION,
            "bundle_id": f"{BTC_THRESHOLD_CORRIDOR_FAMILY}:{lower.ticker}:{higher.ticker}",
            "description": (
                f"Certified lower YES {lower.strike} plus higher NO {higher.strike}"
            ),
            "outcomes": list(outcomes),
            "legs": [
                {"ticker": lower.ticker, "side": "yes", "payouts": [0, 1, 1]},
                {"ticker": higher.ticker, "side": "no", "payouts": [1, 1, 0]},
            ],
        })

    def payload(self) -> dict[str, object]:
        return {
            "schema_version": SCHEMA_VERSION,
            "family": BTC_THRESHOLD_CORRIDOR_FAMILY,
            "rules_reviewed": True,
            "ordinary_binary_payouts_only": True,
            "event_ticker": self.event_ticker,
            "underlier": self.underlier,
            "comparison": self.comparison,
            "api_strike_type": self.api_strike_type,
            "settlement_time_field": self.settlement_time_field,
            "settlement_time": self.settlement_time,
            "reviewed_at": self.reviewed_at,
            "markets": [member.payload() for member in self.members],
        }

    @property
    def digest(self) -> str:
        return _canonical_digest(self.payload())


def load_corridor_certificate(path: Path) -> BtcThresholdCorridorCertificate:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError("corridor certificate root must be an object")
    return BtcThresholdCorridorCertificate.from_payload(payload)


@dataclass(frozen=True)
class FeeSchedule:
    fee_type: str
    multiplier: Decimal

    @classmethod
    def from_payload(cls, payload: Mapping[str, Any]) -> "FeeSchedule":
        fee_type = str(payload.get("fee_type") or "").strip().lower()
        multiplier = _decimal(payload.get("fee_multiplier", 1), "fee_multiplier")
        if multiplier < ZERO:
            raise ValueError("fee_multiplier cannot be negative")
        if fee_type not in {"none", "quadratic", "quadratic_with_maker_fees"}:
            raise ValueError(f"unsupported fee_type: {fee_type or 'missing'}")
        return cls(fee_type=fee_type, multiplier=multiplier)

    def taker_fee(self, price: Decimal, contracts: Decimal) -> Decimal:
        if self.fee_type == "none" or price <= ZERO or contracts <= ZERO:
            return ZERO
        raw = GENERAL_TAKER_RATE * self.multiplier * contracts * price * (ONE - price)
        return raw.quantize(CENT, rounding=ROUND_CEILING)

    def payload(self) -> dict[str, str]:
        return {"fee_type": self.fee_type, "fee_multiplier": str(self.multiplier)}


@dataclass(frozen=True)
class LegExecution:
    ticker: str
    side: str
    contracts: Decimal
    cost: Decimal
    fee: Decimal
    fills: tuple[tuple[Decimal, Decimal], ...]

    def payload(self) -> dict[str, object]:
        return {
            "ticker": self.ticker,
            "side": self.side,
            "contracts": str(self.contracts),
            "cost": str(self.cost),
            "fee": str(self.fee),
            "fills": [{"price": str(price), "contracts": str(size)}
                      for price, size in self.fills],
        }


@dataclass(frozen=True)
class StructuralEvaluation:
    state: str
    reason: str
    bundle_id: str
    certificate_sha256: str
    quantity: Decimal
    guaranteed_payout: Decimal
    acquisition_cost: Decimal = ZERO
    fees: Decimal = ZERO
    execution_buffer: Decimal = ZERO
    net_profit: Decimal = ZERO
    net_edge_per_bundle: Decimal = ZERO
    legs: tuple[LegExecution, ...] = ()

    def payload(self) -> dict[str, object]:
        return {
            "state": self.state,
            "reason": self.reason,
            "bundle_id": self.bundle_id,
            "certificate_sha256": self.certificate_sha256,
            "quantity": str(self.quantity),
            "guaranteed_payout": str(self.guaranteed_payout),
            "acquisition_cost": str(self.acquisition_cost),
            "fees": str(self.fees),
            "execution_buffer": str(self.execution_buffer),
            "net_profit": str(self.net_profit),
            "net_edge_per_bundle": str(self.net_edge_per_bundle),
            "legs": [leg.payload() for leg in self.legs],
        }


def load_certificate(path: Path) -> PayoffCertificate:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError("certificate root must be an object")
    return PayoffCertificate.from_payload(payload)


def _canonical_digest(payload: Mapping[str, Any]) -> str:
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def _market_api_strike_type(payload: Mapping[str, Any]) -> str:
    direct = str(payload.get("strike_type") or "").strip().lower()
    custom = payload.get("custom_strike")
    if not direct and isinstance(custom, Mapping):
        direct = str(custom.get("strike_type") or "").strip().lower()
    return direct


def _market_floor_strike(payload: Mapping[str, Any]) -> Decimal | None:
    value = payload.get("floor_strike")
    custom = payload.get("custom_strike")
    if value in (None, "") and isinstance(custom, Mapping):
        value = custom.get("floor_strike", custom.get("strike"))
    if value in (None, ""):
        value = payload.get("strike")
    if value in (None, ""):
        return None
    return _decimal(value, "market floor strike")


def validate_corridor_markets(
    certificate: BtcThresholdCorridorCertificate,
    markets: Mapping[str, Market],
) -> str:
    """Return an empty string only when live metadata matches the review."""
    for member in certificate.members:
        market = markets.get(member.ticker)
        if market is None:
            return f"missing certified market: {member.ticker}"
        if market.ticker != member.ticker or str(market.raw.get("ticker") or "") != member.ticker:
            return f"market ticker changed: {member.ticker}"
        if market.status.lower() not in {"active", "open"}:
            return f"market is not open: {member.ticker} ({market.status})"
        raw = market.raw
        if str(raw.get("event_ticker") or "") != certificate.event_ticker:
            return f"event_ticker changed: {member.ticker}"
        if _market_api_strike_type(raw) != certificate.api_strike_type:
            return f"API strike type changed: {member.ticker}"
        if _market_floor_strike(raw) != member.strike:
            return f"certified strike changed: {member.ticker}"
        if str(raw.get(certificate.settlement_time_field) or "") != certificate.settlement_time:
            return f"settlement time changed: {member.ticker}"
        if settlement_terms_sha256(raw) != member.terms_sha256:
            return f"reviewed settlement terms changed: {member.ticker}"
    return ""


def evaluate_corridor_family(
    certificate: BtcThresholdCorridorCertificate,
    markets: Mapping[str, Market],
    books: Mapping[str, BinaryBook],
    fees: Mapping[str, FeeSchedule],
    *,
    quantity: Decimal = ONE,
    execution_buffer_per_contract: Decimal = ZERO,
    min_net_edge_per_bundle: Decimal = ZERO,
) -> dict[str, object]:
    validation_error = validate_corridor_markets(certificate, markets)
    if validation_error:
        return {
            "family": BTC_THRESHOLD_CORRIDOR_FAMILY,
            "state": "unavailable",
            "reason": validation_error,
            "certificate_sha256": certificate.digest,
            "pairs_evaluated": 0,
            "opportunities": 0,
            "pairs": [],
        }

    rows: list[dict[str, object]] = []
    for lower, higher in certificate.pairs:
        pair_certificate = certificate.pair_certificate(lower, higher)
        evaluation = evaluate_bundle(
            pair_certificate,
            books,
            fees,
            quantity=quantity,
            execution_buffer_per_contract=execution_buffer_per_contract,
            min_net_edge_per_bundle=min_net_edge_per_bundle,
        )
        rows.append({
            "family": BTC_THRESHOLD_CORRIDOR_FAMILY,
            "event_ticker": certificate.event_ticker,
            "lower_ticker": lower.ticker,
            "lower_strike": str(lower.strike),
            "higher_ticker": higher.ticker,
            "higher_strike": str(higher.strike),
            "comparison": certificate.comparison,
            "evaluation": evaluation.payload(),
        })
    opportunities = sum(
        row["evaluation"]["state"] == "opportunity"  # type: ignore[index]
        for row in rows
    )
    available = [
        row for row in rows if row["evaluation"]["state"] != "unavailable"  # type: ignore[index]
    ]
    state = "opportunity" if opportunities else ("not_profitable" if available else "unavailable")
    return {
        "family": BTC_THRESHOLD_CORRIDOR_FAMILY,
        "state": state,
        "reason": (
            "one or more certified corridors clear the threshold"
            if opportunities
            else "no certified corridor clears the threshold"
            if available
            else "all certified corridors are unavailable"
        ),
        "certificate_sha256": certificate.digest,
        "pairs_evaluated": len(rows),
        "opportunities": opportunities,
        "pairs": rows,
    }


def discovery_row(event: Mapping[str, Any], *, observed_at: str) -> dict[str, object]:
    """Preserve an event as a candidate, never as an executable certificate.

    Kalshi's ``mutually_exclusive`` flag is useful discovery metadata, but it
    does not prove exhaustive binary settlement. Individual rules may permit
    scalar/non-standard payouts. Accordingly every row remains blocked on
    manual rules review even when the exchange marks the event exclusive.
    """
    raw_markets = event.get("markets")
    markets = [market for market in raw_markets if isinstance(market, Mapping)] \
        if isinstance(raw_markets, list) else []
    active = [
        market for market in markets
        if str(market.get("status") or "").lower() in {"active", "open"}
    ]
    tickers = [str(market.get("ticker") or "") for market in active]
    tickers = [ticker for ticker in tickers if ticker]
    exclusive = event.get("mutually_exclusive") is True
    reasons = [
        "settlement rules have not been manually reviewed",
        "scalar, cancellation, DNP, last-fair-price, and other non-standard payouts are not enumerated",
    ]
    if not exclusive:
        reasons.append("event is not machine-declared mutually exclusive")
    if len(active) < 2:
        reasons.append("fewer than two open child markets")
    return {
        "schema_version": SCHEMA_VERSION,
        "event": "kalshi_structural_arb_candidate",
        "observed_at": observed_at,
        "event_ticker": str(event.get("event_ticker") or ""),
        "series_ticker": str(event.get("series_ticker") or ""),
        "title": str(event.get("title") or ""),
        "machine_claims": {
            "mutually_exclusive": exclusive,
            "collateral_return_type": event.get("collateral_return_type"),
        },
        "open_market_tickers": tickers,
        "open_market_count": len(tickers),
        "candidate": exclusive and len(tickers) >= 2,
        "certified": False,
        "review_status": "manual_rules_review_required",
        "certification_blockers": reasons,
        "event_sha256": _canonical_digest(event),
        "raw_event": dict(event),
    }


def discover_candidates(
    client: Any,
    *,
    out: Path,
    max_pages: int = 0,
    page_size: int = 20,
) -> dict[str, int]:
    """Record open structural candidates without manufacturing certificates."""
    if max_pages < 0:
        raise ValueError("max_pages cannot be negative")
    if not 1 <= page_size <= 200:
        raise ValueError("page_size must be between 1 and 200")
    out.parent.mkdir(parents=True, exist_ok=True)
    cursor: str | None = None
    pages = events = candidates = 0
    seen_cursors: set[str] = set()
    with out.open("a", encoding="utf-8") as handle:
        while True:
            batch, next_cursor = client.get_events(
                status="open", limit=page_size, cursor=cursor, with_nested_markets=True
            )
            observed_at = datetime.now(timezone.utc).isoformat()
            pages += 1
            for event in batch:
                row = discovery_row(event, observed_at=observed_at)
                handle.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")
                events += 1
                candidates += int(bool(row["candidate"]))
            handle.flush()
            if not next_cursor or (max_pages and pages >= max_pages):
                break
            if next_cursor in seen_cursors:
                raise RuntimeError("Kalshi event pagination repeated a cursor")
            seen_cursors.add(next_cursor)
            cursor = next_cursor
    return {"pages": pages, "events": events, "candidates": candidates, "certified": 0}


def _ask_levels(book: BinaryBook, side: str) -> tuple[tuple[Decimal, Decimal], ...]:
    # Kalshi returns bids only.  The opposite-side bid is this side's ask.
    opposite: tuple[Level, ...] = book.no_bids if side == "yes" else book.yes_bids
    aggregated: dict[Decimal, Decimal] = {}
    for level in opposite:
        bid = _decimal(level.price, "book price")
        size = _decimal(level.size, "book size")
        ask = ONE - bid
        if ZERO < ask < ONE and size > ZERO:
            aggregated[ask] = aggregated.get(ask, ZERO) + size
    return tuple(sorted(aggregated.items()))


def _sweep_leg(
    leg: CertifiedLeg,
    book: BinaryBook,
    schedule: FeeSchedule,
    quantity: Decimal,
) -> LegExecution | None:
    remaining = quantity
    cost = ZERO
    fee = ZERO
    fills: list[tuple[Decimal, Decimal]] = []
    for price, available in _ask_levels(book, leg.side):
        filled = min(remaining, available)
        if filled <= ZERO:
            continue
        fills.append((price, filled))
        cost += price * filled
        # Rounding each price-level fill separately is intentionally
        # conservative relative to aggregating the order before rounding.
        fee += schedule.taker_fee(price, filled)
        remaining -= filled
        if remaining <= ZERO:
            break
    if remaining > ZERO:
        return None
    return LegExecution(
        ticker=leg.ticker,
        side=leg.side,
        contracts=quantity,
        cost=cost,
        fee=fee,
        fills=tuple(fills),
    )


def evaluate_bundle(
    certificate: PayoffCertificate,
    books: Mapping[str, BinaryBook],
    fees: Mapping[str, FeeSchedule],
    *,
    quantity: Decimal = ONE,
    execution_buffer_per_contract: Decimal = ZERO,
    min_net_edge_per_bundle: Decimal = ZERO,
) -> StructuralEvaluation:
    quantity = _decimal(quantity, "quantity")
    buffer_rate = _decimal(execution_buffer_per_contract, "execution_buffer_per_contract")
    min_edge = _decimal(min_net_edge_per_bundle, "min_net_edge_per_bundle")
    base = dict(
        bundle_id=certificate.bundle_id,
        certificate_sha256=certificate.digest,
        quantity=quantity,
        guaranteed_payout=certificate.guaranteed_payout * quantity,
    )
    if quantity <= ZERO or buffer_rate < ZERO or min_edge < ZERO:
        return StructuralEvaluation(state="unavailable", reason="invalid scan parameters", **base)

    executions: list[LegExecution] = []
    for leg in certificate.legs:
        book = books.get(leg.ticker)
        schedule = fees.get(leg.ticker)
        if book is None:
            return StructuralEvaluation(
                state="unavailable", reason=f"missing batch book: {leg.ticker}", **base
            )
        if schedule is None:
            return StructuralEvaluation(
                state="unavailable", reason=f"missing fee schedule: {leg.ticker}", **base
            )
        execution = _sweep_leg(leg, book, schedule, quantity)
        if execution is None:
            return StructuralEvaluation(
                state="unavailable",
                reason=f"insufficient {leg.side.upper()} ask depth: {leg.ticker}",
                **base,
            )
        executions.append(execution)

    cost = sum((execution.cost for execution in executions), ZERO)
    fee = sum((execution.fee for execution in executions), ZERO)
    buffer = buffer_rate * quantity * Decimal(len(executions))
    guaranteed = certificate.guaranteed_payout * quantity
    net = guaranteed - cost - fee - buffer
    edge = net / quantity
    state = "opportunity" if edge >= min_edge and net > ZERO else "not_profitable"
    reason = "certified net edge clears threshold" if state == "opportunity" else "net edge does not clear threshold"
    return StructuralEvaluation(
        state=state,
        reason=reason,
        acquisition_cost=cost,
        fees=fee,
        execution_buffer=buffer,
        net_profit=net,
        net_edge_per_bundle=edge,
        legs=tuple(executions),
        **base,
    )


def _book_payload(book: BinaryBook) -> dict[str, object]:
    def levels(values: tuple[Level, ...]) -> list[list[str]]:
        return [[str(level.price), str(level.size)] for level in values]
    return {"ticker": book.ticker, "yes_bids": levels(book.yes_bids), "no_bids": levels(book.no_bids)}


def _book_from_payload(payload: Mapping[str, Any]) -> BinaryBook:
    def levels(key: str) -> tuple[Level, ...]:
        raw = payload.get(key)
        if not isinstance(raw, list):
            raise ValueError(f"evidence book missing {key}")
        return tuple(Level(price=float(row[0]), size=float(row[1])) for row in raw)
    ticker = str(payload.get("ticker") or "")
    if not ticker:
        raise ValueError("evidence book missing ticker")
    return BinaryBook(ticker=ticker, yes_bids=levels("yes_bids"), no_bids=levels("no_bids"))


def collect_snapshot(
    client: Any,
    certificate: PayoffCertificate,
    *,
    quantity: Decimal = ONE,
    execution_buffer_per_contract: Decimal = ZERO,
    min_net_edge_per_bundle: Decimal = ZERO,
) -> dict[str, object]:
    requested_at = datetime.now(timezone.utc)
    markets: dict[str, Market] = {}
    schedules: dict[str, FeeSchedule] = {}
    series_payloads: dict[str, dict[str, Any]] = {}
    unavailable_reason = ""

    try:
        for ticker in certificate.tickers:
            market = client.get_market(ticker)
            markets[ticker] = market
            if market.status.lower() not in {"active", "open"}:
                unavailable_reason = f"market is not open: {ticker} ({market.status})"
                break
            series_ticker = str(market.raw.get("series_ticker") or "")
            if not series_ticker:
                unavailable_reason = f"market has no series_ticker: {ticker}"
                break
            if series_ticker not in series_payloads:
                series_payloads[series_ticker] = client.get_series(series_ticker)
            try:
                schedules[ticker] = FeeSchedule.from_payload(series_payloads[series_ticker])
            except ValueError as exc:
                unavailable_reason = f"{ticker}: {exc}"
                break

        books = client.get_orderbooks(list(certificate.tickers)) if not unavailable_reason else {}
    except Exception as exc:  # noqa: BLE001 - persist telemetry failure as evidence.
        unavailable_reason = f"collection failed: {type(exc).__name__}: {exc}"
        books = {}
    received_at = datetime.now(timezone.utc)
    if unavailable_reason:
        evaluation = StructuralEvaluation(
            state="unavailable",
            reason=unavailable_reason,
            bundle_id=certificate.bundle_id,
            certificate_sha256=certificate.digest,
            quantity=_decimal(quantity, "quantity"),
            guaranteed_payout=certificate.guaranteed_payout * _decimal(quantity, "quantity"),
        )
    else:
        evaluation = evaluate_bundle(
            certificate,
            books,
            schedules,
            quantity=quantity,
            execution_buffer_per_contract=execution_buffer_per_contract,
            min_net_edge_per_bundle=min_net_edge_per_bundle,
        )

    return {
        "schema_version": SCHEMA_VERSION,
        "event": "kalshi_structural_arb_scan",
        "requested_at": requested_at.isoformat(),
        "received_at": received_at.isoformat(),
        "request_duration_ms": int((received_at - requested_at).total_seconds() * 1000),
        "certificate": certificate.payload(),
        "certificate_sha256": certificate.digest,
        "quantity": str(quantity),
        "execution_buffer_per_contract": str(execution_buffer_per_contract),
        "min_net_edge_per_bundle": str(min_net_edge_per_bundle),
        "collection_error": unavailable_reason or None,
        "markets": {ticker: market.raw for ticker, market in markets.items()},
        "series": series_payloads,
        "fees": {ticker: schedule.payload() for ticker, schedule in schedules.items()},
        "books": {ticker: _book_payload(book) for ticker, book in books.items()},
        "evaluation": evaluation.payload(),
    }


def _unavailable_corridor_result(
    certificate: BtcThresholdCorridorCertificate, reason: str
) -> dict[str, object]:
    return {
        "family": BTC_THRESHOLD_CORRIDOR_FAMILY,
        "state": "unavailable",
        "reason": reason,
        "certificate_sha256": certificate.digest,
        "pairs_evaluated": 0,
        "opportunities": 0,
        "pairs": [],
    }


def collect_corridor_snapshot(
    client: Any,
    certificate: BtcThresholdCorridorCertificate,
    *,
    quantity: Decimal = ONE,
    execution_buffer_per_contract: Decimal = ZERO,
    min_net_edge_per_bundle: Decimal = ZERO,
) -> dict[str, object]:
    """Collect one synchronized snapshot for the certified corridor family."""
    requested_at = datetime.now(timezone.utc)
    markets: dict[str, Market] = {}
    schedules: dict[str, FeeSchedule] = {}
    series_payloads: dict[str, dict[str, Any]] = {}
    books: dict[str, BinaryBook] = {}
    unavailable_reason = ""
    try:
        for ticker in certificate.tickers:
            market = client.get_market(ticker)
            markets[ticker] = market
        unavailable_reason = validate_corridor_markets(certificate, markets)
        if not unavailable_reason:
            for ticker, market in markets.items():
                series_ticker = str(market.raw.get("series_ticker") or "")
                if not series_ticker:
                    unavailable_reason = f"market has no series_ticker: {ticker}"
                    break
                if series_ticker not in series_payloads:
                    series_payloads[series_ticker] = client.get_series(series_ticker)
                try:
                    schedules[ticker] = FeeSchedule.from_payload(series_payloads[series_ticker])
                except ValueError as exc:
                    unavailable_reason = f"{ticker}: {exc}"
                    break
        if not unavailable_reason:
            books = client.get_orderbooks(list(certificate.tickers))
    except Exception as exc:  # noqa: BLE001 - persist telemetry failure as evidence.
        unavailable_reason = f"collection failed: {type(exc).__name__}: {exc}"
    received_at = datetime.now(timezone.utc)
    evaluation = (
        _unavailable_corridor_result(certificate, unavailable_reason)
        if unavailable_reason
        else evaluate_corridor_family(
            certificate,
            markets,
            books,
            schedules,
            quantity=quantity,
            execution_buffer_per_contract=execution_buffer_per_contract,
            min_net_edge_per_bundle=min_net_edge_per_bundle,
        )
    )
    return {
        "schema_version": SCHEMA_VERSION,
        "event": "kalshi_btc_threshold_corridor_scan",
        "family": BTC_THRESHOLD_CORRIDOR_FAMILY,
        "requested_at": requested_at.isoformat(),
        "received_at": received_at.isoformat(),
        "request_duration_ms": int((received_at - requested_at).total_seconds() * 1000),
        "certificate": certificate.payload(),
        "certificate_sha256": certificate.digest,
        "quantity": str(quantity),
        "execution_buffer_per_contract": str(execution_buffer_per_contract),
        "min_net_edge_per_bundle": str(min_net_edge_per_bundle),
        "collection_error": unavailable_reason or None,
        "markets": {ticker: market.raw for ticker, market in markets.items()},
        "series": series_payloads,
        "fees": {ticker: schedule.payload() for ticker, schedule in schedules.items()},
        "books": {ticker: _book_payload(book) for ticker, book in books.items()},
        "evaluation": evaluation,
    }


def record_corridor_snapshots(
    client: Any,
    certificate: BtcThresholdCorridorCertificate,
    *,
    out: Path,
    seconds: float,
    poll_seconds: float,
    quantity: Decimal,
    execution_buffer_per_contract: Decimal,
    min_net_edge_per_bundle: Decimal,
) -> int:
    if seconds <= 0 or poll_seconds <= 0:
        raise ValueError("seconds and poll_seconds must be positive")
    out.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + seconds
    scans = 0
    with out.open("a", encoding="utf-8") as handle:
        while True:
            row = collect_corridor_snapshot(
                client,
                certificate,
                quantity=quantity,
                execution_buffer_per_contract=execution_buffer_per_contract,
                min_net_edge_per_bundle=min_net_edge_per_bundle,
            )
            handle.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")
            handle.flush()
            scans += 1
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(poll_seconds, remaining))
    return scans


def record_snapshots(
    client: Any,
    certificate: PayoffCertificate,
    *,
    out: Path,
    seconds: float,
    poll_seconds: float,
    quantity: Decimal,
    execution_buffer_per_contract: Decimal,
    min_net_edge_per_bundle: Decimal,
) -> int:
    if seconds <= 0 or poll_seconds <= 0:
        raise ValueError("seconds and poll_seconds must be positive")
    out.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + seconds
    scans = 0
    with out.open("a", encoding="utf-8") as handle:
        while True:
            row = collect_snapshot(
                client,
                certificate,
                quantity=quantity,
                execution_buffer_per_contract=execution_buffer_per_contract,
                min_net_edge_per_bundle=min_net_edge_per_bundle,
            )
            handle.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")
            handle.flush()
            scans += 1
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(poll_seconds, remaining))
    return scans


def replay_evidence(path: Path) -> dict[str, object]:
    scans = matches = mismatches = malformed = 0
    states: dict[str, int] = {}
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        scans += 1
        try:
            row = json.loads(line)
            if row.get("family") == BTC_THRESHOLD_CORRIDOR_FAMILY:
                certificate = BtcThresholdCorridorCertificate.from_payload(row["certificate"])
                if certificate.digest != row["certificate_sha256"]:
                    raise ValueError("certificate digest mismatch")
                expected = row["evaluation"]
                collection_error = row.get("collection_error")
                if collection_error:
                    actual = _unavailable_corridor_result(certificate, str(collection_error))
                else:
                    markets = {
                        ticker: Market.from_api(payload)
                        for ticker, payload in row["markets"].items()
                    }
                    books = {
                        ticker: _book_from_payload(payload)
                        for ticker, payload in row["books"].items()
                    }
                    schedules = {
                        ticker: FeeSchedule.from_payload(payload)
                        for ticker, payload in row["fees"].items()
                    }
                    actual = evaluate_corridor_family(
                        certificate,
                        markets,
                        books,
                        schedules,
                        quantity=_decimal(row["quantity"], "quantity"),
                        execution_buffer_per_contract=_decimal(
                            row["execution_buffer_per_contract"],
                            "execution_buffer_per_contract",
                        ),
                        min_net_edge_per_bundle=_decimal(
                            row["min_net_edge_per_bundle"], "min_net_edge_per_bundle"
                        ),
                    )
                state = str(expected.get("state") or "unknown")
                states[state] = states.get(state, 0) + 1
                if actual == expected:
                    matches += 1
                else:
                    mismatches += 1
                continue
            certificate = PayoffCertificate.from_payload(row["certificate"])
            if certificate.digest != row["certificate_sha256"]:
                raise ValueError("certificate digest mismatch")
            expected = row["evaluation"]
            collection_error = row.get("collection_error")
            if collection_error:
                actual = StructuralEvaluation(
                    state="unavailable",
                    reason=str(collection_error),
                    bundle_id=certificate.bundle_id,
                    certificate_sha256=certificate.digest,
                    quantity=_decimal(row["quantity"], "quantity"),
                    guaranteed_payout=(
                        certificate.guaranteed_payout * _decimal(row["quantity"], "quantity")
                    ),
                ).payload()
            else:
                books = {
                    ticker: _book_from_payload(payload) for ticker, payload in row["books"].items()
                }
                schedules = {
                    ticker: FeeSchedule.from_payload(payload) for ticker, payload in row["fees"].items()
                }
                actual = evaluate_bundle(
                    certificate,
                    books,
                    schedules,
                    quantity=_decimal(row["quantity"], "quantity"),
                    execution_buffer_per_contract=_decimal(
                        row["execution_buffer_per_contract"], "execution_buffer_per_contract"
                    ),
                    min_net_edge_per_bundle=_decimal(
                        row["min_net_edge_per_bundle"], "min_net_edge_per_bundle"
                    ),
                ).payload()
            state = str(expected.get("state") or "unknown")
            states[state] = states.get(state, 0) + 1
            if actual == expected:
                matches += 1
            else:
                mismatches += 1
        except Exception:  # noqa: BLE001 - count corrupt evidence and continue audit.
            malformed += 1
            states[f"malformed_line_{line_number}"] = 1
    return {
        "scans": scans,
        "matches": matches,
        "mismatches": mismatches,
        "malformed": malformed,
        "states": states,
        "valid": scans > 0 and matches == scans and mismatches == 0 and malformed == 0,
    }
