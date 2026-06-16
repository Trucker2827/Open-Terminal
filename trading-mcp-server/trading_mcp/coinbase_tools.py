from __future__ import annotations

import uuid
from typing import Any, Literal

from .config import Settings
from .safety import RiskManager, TradingBlocked
from .utils import audit, json_safe


class CoinbaseService:
    def __init__(self, settings: Settings, risk: RiskManager):
        self.settings = settings
        self.risk = risk
        self._client = None

    def _require_enabled(self) -> None:
        if not self.settings.enable_coinbase:
            raise RuntimeError("Coinbase disabled by ENABLE_COINBASE=false")
        if not self.settings.coinbase_api_key or not self.settings.coinbase_api_secret:
            raise RuntimeError("Coinbase credentials missing")

    @property
    def client(self):
        self._require_enabled()
        if self._client is None:
            from coinbase.rest import RESTClient
            self._client = RESTClient(api_key=self.settings.coinbase_api_key, api_secret=self.settings.coinbase_api_secret)
        return self._client

    def get_account_balance(self) -> dict[str, Any]:
        self.risk.check_read()
        return {"ok": True, "venue": "coinbase", "accounts": json_safe(self.client.get_accounts())}

    def get_portfolio(self) -> dict[str, Any]:
        self.risk.check_read()
        # Advanced Trade accounts are the practical portfolio source for most users.
        return {"ok": True, "venue": "coinbase", "accounts": json_safe(self.client.get_accounts())}

    def get_market_data(self, product_id: str) -> dict[str, Any]:
        self.risk.check_read()
        return {"ok": True, "venue": "coinbase", "product": json_safe(self.client.get_product(product_id))}

    def get_open_orders(self, product_id: str | None = None) -> dict[str, Any]:
        self.risk.check_read()
        kwargs = {"order_status": ["OPEN"]}
        if product_id:
            kwargs["product_id"] = product_id
        return {"ok": True, "venue": "coinbase", "orders": json_safe(self.client.list_orders(**kwargs))}

    def cancel_order(self, order_id: str) -> dict[str, Any]:
        self.risk.check_read()
        if self.settings.dry_run:
            return {"ok": True, "dry_run": True, "message": "Coinbase cancel skipped because DRY_RUN=true", "order_id": order_id}
        result = self.client.cancel_orders(order_ids=[order_id])
        audit("coinbase.cancel_order", {"order_id": order_id, "result": json_safe(result)})
        return {"ok": True, "venue": "coinbase", "result": json_safe(result)}

    def place_order(
        self,
        product_id: str,
        side: Literal["buy", "sell"],
        quantity: float,
        type: Literal["market", "limit"] = "market",
        limit_price: float | None = None,
        confirmation_token: str | None = None,
    ) -> dict[str, Any]:
        estimated_price = limit_price if limit_price is not None else self._estimate_price(product_id)
        risk_report = self.risk.check_trade(
            venue="coinbase", symbol=product_id, side=side, quantity=quantity,
            estimated_price=estimated_price, confirmation_token=confirmation_token, asset_class="crypto",
            current_position_notional=self._current_position_notional(product_id, estimated_price),
        )
        if self.settings.dry_run:
            return {"ok": True, "dry_run": True, "would_submit": risk_report}

        client_order_id = str(uuid.uuid4())
        order_configuration: dict[str, Any]
        if type == "market":
            # quantity is interpreted as base size. For quote-size buys, add a separate explicit tool.
            order_configuration = {"market_market_ioc": {"base_size": str(quantity)}}
        elif type == "limit" and limit_price is not None:
            order_configuration = {"limit_limit_gtc": {"base_size": str(quantity), "limit_price": str(limit_price), "post_only": False}}
        else:
            raise ValueError("limit orders require limit_price")
        result = self.client.create_order(
            client_order_id=client_order_id,
            product_id=product_id,
            side=side.upper(),
            order_configuration=order_configuration,
        )
        audit("coinbase.place_order", {"risk": risk_report, "result": json_safe(result)})
        return {"ok": True, "venue": "coinbase", "order": json_safe(result), "risk": risk_report}

    def _estimate_price(self, product_id: str) -> float | None:
        # Best-effort latest product price for sizing a MARKET order. Fail-SAFE:
        # None on error → the risk manager BLOCKS the order (fail-closed).
        try:
            product = self.client.get_product(product_id)
            price = product.get("price") if isinstance(product, dict) else getattr(product, "price", None)
            return float(price) if price else None
        except Exception:
            return None

    def _current_position_notional(self, product_id: str, price: float | None) -> float | None:
        # base-asset holding * price, for the position cap. None when unavailable
        # (cap then skipped for this order; the per-order notional cap still applies).
        if not price:
            return None
        try:
            base = product_id.split("-")[0].split("/")[0].upper()
            accounts = self.client.get_accounts()
            items = accounts.get("accounts") if isinstance(accounts, dict) else getattr(accounts, "accounts", None)
            for a in (items or []):
                cur = a.get("currency") if isinstance(a, dict) else getattr(a, "currency", None)
                if cur and str(cur).upper() == base:
                    bal = a.get("available_balance") if isinstance(a, dict) else getattr(a, "available_balance", None)
                    val = bal.get("value") if isinstance(bal, dict) else getattr(bal, "value", None)
                    return abs(float(val) * price) if val is not None else None
            return None
        except Exception:
            return None

    def send_crypto(self, asset: str, amount: float, destination: str, network: str | None = None, confirmation_token: str | None = None) -> dict[str, Any]:
        self.risk.check_read()
        if not self.settings.allow_crypto_withdrawals:
            raise TradingBlocked("crypto withdrawals disabled; set ALLOW_CRYPTO_WITHDRAWALS=true and implement a custody-specific withdrawal backend")
        if self.settings.trading_mode == "live" and confirmation_token != self.settings.confirmation_token:
            raise TradingBlocked("live crypto transfer requires confirmation_token")
        # Intentionally not implemented: Coinbase Advanced Trade trading APIs and wallet/payment APIs have different permission models.
        raise NotImplementedError("withdrawal backend intentionally omitted; use Coinbase CDP/Wallet APIs with separate withdrawal keys and allowlist checks")
