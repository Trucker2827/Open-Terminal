from __future__ import annotations

import uuid
from dataclasses import dataclass


@dataclass(frozen=True)
class LimitOrder:
    ticker: str
    side: str
    action: str
    price: float
    count: float
    time_in_force: str = "good_till_canceled"
    post_only: bool = True
    reduce_only: bool = False
    client_order_id: str | None = None

    @property
    def estimated_buy_cost(self) -> float:
        if self.action != "buy":
            return 0.0
        return self.price * self.count

    def validate(self, max_buy_cost: float) -> None:
        if self.side not in {"yes", "no"}:
            raise ValueError("--side must be yes or no")
        if self.action not in {"buy", "sell"}:
            raise ValueError("--action must be buy or sell")
        if not 0.0 < self.price < 1.0:
            raise ValueError("--price must be between 0 and 1")
        if self.count <= 0:
            raise ValueError("--count must be positive")
        if self.estimated_buy_cost > max_buy_cost:
            raise ValueError(
                f"estimated buy cost ${self.estimated_buy_cost:.2f} exceeds "
                f"--max-buy-cost ${max_buy_cost:.2f}"
            )

    def to_api(self) -> dict[str, object]:
        price_key = "yes_price_dollars" if self.side == "yes" else "no_price_dollars"
        return {
            "ticker": self.ticker,
            "side": self.side,
            "action": self.action,
            "client_order_id": self.client_order_id or f"km-{uuid.uuid4()}",
            "count_fp": f"{self.count:.2f}",
            price_key: f"{self.price:.4f}",
            "time_in_force": self.time_in_force,
            "post_only": self.post_only,
            "reduce_only": self.reduce_only,
            "cancel_order_on_pause": True,
        }
