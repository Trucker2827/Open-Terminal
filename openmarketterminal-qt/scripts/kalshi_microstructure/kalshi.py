from __future__ import annotations

import json
import time
from dataclasses import dataclass
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

from .auth import KalshiCredentials
from .models import BinaryBook, Level, Market


PROD_REST_BASE = "https://external-api.kalshi.com/trade-api/v2"
DEMO_REST_BASE = "https://external-api.demo.kalshi.co/trade-api/v2"


@dataclass(frozen=True)
class KalshiRestClient:
    env: str = "prod"
    timeout: float = 10.0
    credentials: KalshiCredentials | None = None
    max_get_retries: int = 2
    retry_backoff_seconds: float = 0.25

    @property
    def base_url(self) -> str:
        return DEMO_REST_BASE if self.env == "demo" else PROD_REST_BASE

    def request_json(
        self,
        method: str,
        path: str,
        params: dict[str, Any] | None = None,
        body: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        query = f"?{urlencode(params, doseq=True)}" if params else ""
        headers = {"accept": "application/json", "user-agent": "kalshi-microstructure/0.1"}
        if self.credentials is not None:
            sign_path = f"/trade-api/v2{path}"
            headers.update(self.credentials.headers(method, sign_path))
        data = None
        if body is not None:
            headers["content-type"] = "application/json"
            data = json.dumps(body).encode("utf-8")
        attempts = self.max_get_retries + 1 if method.upper() == "GET" else 1
        for attempt in range(attempts):
            request = Request(
                f"{self.base_url}{path}{query}",
                headers=headers,
                method=method,
                data=data,
            )
            try:
                with urlopen(request, timeout=self.timeout) as response:
                    return json.loads(response.read().decode("utf-8"))
            except HTTPError as exc:
                if attempt >= attempts - 1 or exc.code not in {429, 500, 502, 503, 504}:
                    raise
                time.sleep(self.retry_backoff_seconds * (2**attempt))
            except (TimeoutError, URLError) as exc:
                if attempt >= attempts - 1:
                    raise
                time.sleep(self.retry_backoff_seconds * (2**attempt))
        raise RuntimeError("unreachable request retry state")

    def get_json(self, path: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        return self.request_json("GET", path, params=params)

    def auth_check(self) -> bool:
        self.get_json("/portfolio/balance")
        return True

    def get_balance(self) -> dict[str, Any]:
        return self.get_json("/portfolio/balance")

    def create_order(self, order: dict[str, Any]) -> dict[str, Any]:
        return self.request_json("POST", "/portfolio/orders", body=order)

    def cancel_order(self, order_id: str) -> dict[str, Any]:
        return self.request_json("DELETE", f"/portfolio/orders/{order_id}")

    def get_orders(
        self,
        *,
        ticker: str | None = None,
        status: str | None = None,
        limit: int = 100,
        cursor: str | None = None,
    ) -> tuple[list[dict[str, Any]], str | None]:
        params: dict[str, Any] = {"limit": limit}
        if ticker:
            params["ticker"] = ticker
        if status:
            params["status"] = status
        if cursor:
            params["cursor"] = cursor
        payload = self.get_json("/portfolio/orders", params)
        return payload.get("orders", []), payload.get("cursor")

    def batch_cancel_orders(self, order_ids: list[str]) -> dict[str, Any]:
        return self.request_json("DELETE", "/portfolio/orders/batched", body={"ids": order_ids})

    def get_markets(
        self,
        *,
        series_ticker: str | None = None,
        status: str = "open",
        limit: int = 100,
        cursor: str | None = None,
    ) -> tuple[list[Market], str | None]:
        params: dict[str, Any] = {"status": status, "limit": limit}
        if series_ticker:
            params["series_ticker"] = series_ticker
        if cursor:
            params["cursor"] = cursor

        payload = self.get_json("/markets", params)
        markets = [Market.from_api(item) for item in payload.get("markets", [])]
        return markets, payload.get("cursor")

    def get_market(self, ticker: str) -> Market:
        payload = self.get_json(f"/markets/{ticker}")
        return Market.from_api(payload["market"])

    def get_orderbook(self, ticker: str) -> BinaryBook:
        payload = self.get_json(f"/markets/{ticker}/orderbook")
        book = payload.get("orderbook_fp") or payload.get("orderbook") or {}
        return BinaryBook(
            ticker=ticker,
            yes_bids=_levels(book.get("yes_dollars") or book.get("yes") or ()),
            no_bids=_levels(book.get("no_dollars") or book.get("no") or ()),
        )


def _levels(raw_levels: Any) -> tuple[Level, ...]:
    levels: list[Level] = []
    for raw in raw_levels or ():
        if len(raw) < 2:
            continue
        price = _price(raw[0])
        size = float(raw[1])
        levels.append(Level(price=price, size=size))
    return tuple(sorted(levels, key=lambda level: level.price, reverse=True))


def _price(value: Any) -> float:
    price = float(value)
    if price > 1.0:
        price /= 100.0
    return price
