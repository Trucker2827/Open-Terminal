from __future__ import annotations

import base64
import hashlib
import hmac
import time
from typing import Any, Literal
from urllib.parse import urlencode

import requests

from .config import Settings
from .safety import RiskManager, TradingBlocked
from .utils import audit, json_safe


KRAKEN_API_BASE = "https://api.kraken.com"


class KrakenAPIError(RuntimeError):
    pass


class KrakenRestClient:
    def __init__(self, api_key: str | None, api_secret: str | None, *, base_url: str = KRAKEN_API_BASE):
        self.api_key = api_key or ""
        self.api_secret = api_secret or ""
        self.base_url = base_url.rstrip("/")

    def public(self, path: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        response = requests.get(f"{self.base_url}{path}", params=params or {}, timeout=10)
        return self._parse(response)

    def private(self, path: str, data: dict[str, Any] | None = None) -> dict[str, Any]:
        if not self.api_key or not self.api_secret:
            raise KrakenAPIError("Kraken credentials missing")
        payload = dict(data or {})
        nonce = str(int(time.time() * 1_000_000))
        payload["nonce"] = nonce
        body = urlencode(payload)
        headers = {
            "API-Key": self.api_key,
            "API-Sign": self._sign(path, nonce, body),
            "Content-Type": "application/x-www-form-urlencoded",
        }
        response = requests.post(f"{self.base_url}{path}", data=body, headers=headers, timeout=10)
        return self._parse(response)

    def ticker(self, pair: str) -> dict[str, Any]:
        return self.public("/0/public/Ticker", {"pair": pair})

    def balance(self) -> dict[str, Any]:
        return self.private("/0/private/Balance")

    def open_orders(self) -> dict[str, Any]:
        return self.private("/0/private/OpenOrders")

    def add_post_only_limit(self, *, pair: str, side: str, volume: float, price: float) -> dict[str, Any]:
        return self.private("/0/private/AddOrder", {
            "pair": pair,
            "type": side,
            "ordertype": "limit",
            "volume": f"{volume:.8f}",
            "price": str(price),
            "oflags": "post",
        })

    def cancel_order(self, txid: str) -> dict[str, Any]:
        return self.private("/0/private/CancelOrder", {"txid": txid})

    def _sign(self, path: str, nonce: str, body: str) -> str:
        try:
            secret = base64.b64decode(self.api_secret)
        except Exception as exc:
            raise KrakenAPIError("KRAKEN_API_SECRET is not valid base64") from exc
        sha = hashlib.sha256((nonce + body).encode("utf-8")).digest()
        mac = hmac.new(secret, path.encode("utf-8") + sha, hashlib.sha512)
        return base64.b64encode(mac.digest()).decode("ascii")

    @staticmethod
    def _parse(response: requests.Response) -> dict[str, Any]:
        response.raise_for_status()
        payload = response.json()
        errors = payload.get("error") or []
        if errors:
            raise KrakenAPIError("; ".join(errors))
        return payload.get("result", {})


class KrakenService:
    def __init__(self, settings: Settings, risk: RiskManager):
        self.settings = settings
        self.risk = risk
        self._client: KrakenRestClient | None = None

    def _require_enabled(self) -> None:
        if not self.settings.enable_kraken:
            raise RuntimeError("Kraken disabled by ENABLE_KRAKEN=false")

    def _require_credentials(self) -> None:
        self._require_enabled()
        if not self.settings.kraken_api_key or not self.settings.kraken_api_secret:
            raise RuntimeError("Kraken credentials missing")

    @property
    def client(self) -> KrakenRestClient:
        self._require_enabled()
        if self._client is None:
            self._client = KrakenRestClient(self.settings.kraken_api_key, self.settings.kraken_api_secret)
        return self._client

    def get_account_balance(self) -> dict[str, Any]:
        self.risk.check_read()
        self._require_credentials()
        return {"ok": True, "venue": "kraken", "balances": json_safe(self.client.balance())}

    def get_portfolio(self) -> dict[str, Any]:
        self.risk.check_read()
        self._require_credentials()
        return {"ok": True, "venue": "kraken", "balances": json_safe(self.client.balance())}

    def get_market_data(self, product_id: str) -> dict[str, Any]:
        self.risk.check_read()
        self._require_enabled()
        pair = self._api_pair(product_id)
        return {"ok": True, "venue": "kraken", "product_id": product_id, "pair": pair, "ticker": json_safe(self.client.ticker(pair))}

    def get_open_orders(self, product_id: str | None = None) -> dict[str, Any]:
        self.risk.check_read()
        self._require_credentials()
        # Kraken's OpenOrders endpoint does not filter by pair; keep the parameter
        # for MCP shape parity and let callers filter client-side if needed.
        return {"ok": True, "venue": "kraken", "product_id": product_id, "orders": json_safe(self.client.open_orders())}

    def cancel_order(self, order_id: str) -> dict[str, Any]:
        self.risk.check_read()
        if self.settings.dry_run or not self.settings.kraken_allow_trading:
            return {
                "ok": True,
                "dry_run": True,
                "venue": "kraken",
                "message": "Kraken cancel skipped because DRY_RUN=true or KRAKEN_ALLOW_TRADING=false",
                "order_id": order_id,
            }
        self._require_credentials()
        result = self.client.cancel_order(order_id)
        audit("kraken.cancel_order", {"order_id": order_id, "result": json_safe(result)})
        return {"ok": True, "venue": "kraken", "result": json_safe(result)}

    def place_order(
        self,
        product_id: str,
        side: Literal["buy", "sell"],
        quantity: float,
        type: Literal["market", "limit"] = "market",
        limit_price: float | None = None,
        confirmation_token: str | None = None,
    ) -> dict[str, Any]:
        if type != "limit" or limit_price is None:
            raise TradingBlocked("Kraken live lane is limit-only; provide limit_price")

        pair = self._api_pair(product_id)
        risk_report = self.risk.check_trade(
            venue="kraken",
            symbol=pair,
            side=side,
            quantity=quantity,
            estimated_price=limit_price,
            confirmation_token=confirmation_token,
            asset_class="crypto",
            current_position_notional=self._current_position_notional(pair, limit_price),
        )

        armed = self.settings.kraken_allow_trading
        allowlist = self.settings.kraken_symbol_allowlist()
        symbol_allowed = self._allowlist_key(pair) in allowlist
        if self.settings.dry_run or not armed:
            out = {
                "ok": True,
                "dry_run": True,
                "venue": "kraken",
                "order_policy": "spot limit post-only",
                "would_submit": {
                    "pair": pair,
                    "side": side,
                    "quantity": quantity,
                    "limit_price": limit_price,
                    "oflags": "post",
                    "risk": risk_report,
                },
            }
            if not self.settings.dry_run and not armed:
                out["disarmed"] = True
                out["note"] = (
                    "Kraken real execution is DISARMED (KRAKEN_ALLOW_TRADING=false) — "
                    "this is a preview. Arm it deliberately for a live crypto session."
                )
            if not symbol_allowed:
                out["live_execution_blocked"] = (
                    f"would be blocked for live execution: symbol {pair} not allowlisted "
                    "(KRAKEN_ALLOWED_SYMBOLS)"
                )
            return out

        if not symbol_allowed:
            raise TradingBlocked(
                f"symbol {pair} not in KRAKEN_ALLOWED_SYMBOLS — refusing live Kraken order "
                f"(allowed: {sorted(allowlist) or 'NONE'})"
            )

        self._require_credentials()
        result = self.client.add_post_only_limit(pair=pair, side=side, volume=quantity, price=limit_price)
        txids = result.get("txid") if isinstance(result, dict) else None
        order_id = txids[0] if isinstance(txids, list) and txids else None
        audit("kraken.place_order", {"risk": risk_report, "result": json_safe(result)})
        return {
            "ok": True,
            "venue": "kraken",
            "order_id": order_id,
            "status": "submitted",
            "order": json_safe(result),
            "risk": risk_report,
        }

    def _current_position_notional(self, pair: str, price: float | None) -> float | None:
        if not price or not self.settings.kraken_api_key or not self.settings.kraken_api_secret:
            return None
        try:
            balances = self.client.balance()
            base = self._base_asset(pair)
            amount = 0.0
            for key, value in balances.items():
                if key.upper().lstrip("XZ") == base:
                    amount += float(value)
            return abs(amount * price)
        except Exception:
            return None

    @staticmethod
    def _api_pair(product_id: str) -> str:
        cleaned = product_id.strip().upper().replace("/", "-")
        aliases = {
            "BTC-USD": "XBTUSD",
            "XBT-USD": "XBTUSD",
            "ETH-USD": "ETHUSD",
            "SOL-USD": "SOLUSD",
            "DOGE-USD": "DOGEUSD",
        }
        return aliases.get(cleaned, cleaned.replace("-", ""))

    @staticmethod
    def _allowlist_key(pair: str) -> str:
        return pair.strip().upper().replace("/", "").replace("-", "")

    @staticmethod
    def _base_asset(pair: str) -> str:
        pair = pair.upper()
        if pair.startswith("XBT"):
            return "XBT"
        for quote in ("USD", "USDT", "USDC", "EUR"):
            if pair.endswith(quote):
                return pair[: -len(quote)]
        return pair
