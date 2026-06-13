from __future__ import annotations

from dataclasses import dataclass

from .fees import TAKER_FEE_RATE, rounded_fee
from .models import EdgeSignal


@dataclass(frozen=True)
class SimulatedFill:
    shares: float
    cost: float
    fee: float
    requested_shares: float
    visible_shares: float
    fillable_shares: float
    unfilled_shares: float
    unfilled_cost: float

    @property
    def fill_ratio(self) -> float:
        if self.requested_shares <= 0:
            return 0.0
        return self.shares / self.requested_shares


def simulate_taker_buy(
    signal: EdgeSignal,
    *,
    max_cost: float,
    remaining_budget: float,
    available_cash: float,
    fee_rate: float = TAKER_FEE_RATE,
    liquidity_fraction: float = 1.0,
    queue_ahead_contracts: float = 0.0,
    min_fill_cost: float = 0.01,
) -> SimulatedFill | None:
    if signal.ask_price <= 0:
        return None
    budget = min(max_cost, remaining_budget, signal.cost, available_cash)
    if budget <= 0:
        return None

    requested_shares = budget / signal.ask_price
    visible_shares = max(0.0, signal.size - max(0.0, queue_ahead_contracts))
    fillable_shares = visible_shares * max(0.0, min(1.0, liquidity_fraction))
    shares = min(requested_shares, fillable_shares)
    if shares <= 0:
        return None

    cost = shares * signal.ask_price
    fee = rounded_fee(signal.ask_price, shares, fee_rate)
    if cost + fee > available_cash:
        cash_for_contracts = max(0.0, available_cash - fee)
        shares = min(shares, cash_for_contracts / signal.ask_price)
        cost = shares * signal.ask_price
        fee = rounded_fee(signal.ask_price, shares, fee_rate)

    if shares <= 0 or cost < min_fill_cost or cost + fee > available_cash:
        return None

    unfilled_shares = max(0.0, requested_shares - shares)
    return SimulatedFill(
        shares=shares,
        cost=cost,
        fee=fee,
        requested_shares=requested_shares,
        visible_shares=visible_shares,
        fillable_shares=fillable_shares,
        unfilled_shares=unfilled_shares,
        unfilled_cost=unfilled_shares * signal.ask_price,
    )
