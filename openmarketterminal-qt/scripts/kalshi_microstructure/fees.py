from __future__ import annotations

import math


TAKER_FEE_RATE = 0.07
MAKER_FEE_RATE = 0.0175


def rounded_fee(price: float, contracts: float, rate: float = TAKER_FEE_RATE) -> float:
    if price <= 0 or contracts <= 0 or rate <= 0:
        return 0.0
    raw = rate * contracts * price * (1.0 - price)
    return math.ceil(raw * 100.0 - 1e-12) / 100.0


def fee_per_contract(price: float, contracts: float, rate: float = TAKER_FEE_RATE) -> float:
    if contracts <= 0:
        return 0.0
    return rounded_fee(price, contracts, rate) / contracts


def net_expected_value(
    *,
    fair_price: float,
    entry_price: float,
    contracts: float,
    fee_rate: float = TAKER_FEE_RATE,
    slippage_per_contract: float = 0.0,
) -> float:
    gross = (fair_price - entry_price - slippage_per_contract) * contracts
    return gross - rounded_fee(entry_price, contracts, fee_rate)
