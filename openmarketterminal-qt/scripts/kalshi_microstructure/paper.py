from __future__ import annotations

from dataclasses import dataclass, field

from .models import EdgeSignal


@dataclass
class PaperLedger:
    cash: float = 1000.0
    positions: dict[tuple[str, str], float] = field(default_factory=dict)

    def buy(self, signal: EdgeSignal, max_cost: float) -> float:
        cost = min(max_cost, self.cash, signal.cost)
        if cost <= 0 or signal.ask_price <= 0:
            return 0.0
        shares = cost / signal.ask_price
        self.cash -= cost
        key = (signal.ticker, signal.side)
        self.positions[key] = self.positions.get(key, 0.0) + shares
        return shares
