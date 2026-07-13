#!/usr/bin/env python3
"""Kalshi trading bridge for OpenMarketTerminal.

Invocation:
    python prediction_kalshi.py <command> <json_payload>

Commands:
    fee_quote         — local fee/cost estimate using Kalshi-style fees
    simulate_taker_buy — local taker-fill simulation from edge/depth inputs
    preflight_order   — orderbook-aware limit-order risk/depth check
    balance           — GET /portfolio/balance
    positions         — GET /portfolio/positions
    open_orders       — GET /portfolio/orders?status=resting
    queue_positions   — GET /portfolio/orders/queue_positions
    fills             — GET /portfolio/fills
    place_order       — POST /portfolio/events/orders
    cancel_order      — DELETE /portfolio/orders/{order_id}
    decrease_order    — POST /portfolio/orders/{order_id}/decrease
    settlements       — GET /portfolio/settlements

Payload shape for every command:
    {
        "api_key_id":      "UUID",
        "private_key_pem": "-----BEGIN …-----\n…\n-----END …-----",
        "use_demo":        false,
        …command-specific fields…
    }

Local commands such as fee_quote and simulate_taker_buy do not require
credentials. preflight_order can run from a supplied orderbook, public market
data, or authenticated data when credentials are present.

place_order fields:
        "ticker":          "KXFOO-25DEC-T3.00",
        "action":          "buy" | "sell",
        "side":            "yes" | "no",
        "count":           10,                  // number of contracts
        "order_type":      "limit" | "market",
        "yes_price_cents": 52,                   // integer cents 1-99 (limit only)
        "no_price_cents":  48,                   // alternative — limit sell NO side
        "expiration_ts":   1735689600,            // 0/omitted = GTC
        "client_order_id": "uuid-…"

The public UI/CLI still speaks in YES/NO terms. Kalshi's current event-order
endpoint only accepts bid/ask on the YES leg, so this bridge translates:
    buy YES  -> bid YES at yes_price
    sell YES -> ask YES at yes_price
    buy NO   -> ask YES at 1 - no_price
    sell NO  -> bid YES at 1 - no_price

All responses are a single JSON line on stdout.
"""
from __future__ import annotations

import base64
import datetime
import json
import sys
import traceback
import uuid

from kalshi_microstructure.execution import simulate_taker_buy
from kalshi_microstructure.fees import TAKER_FEE_RATE, fee_per_contract, rounded_fee
from kalshi_microstructure.models import BinaryBook, EdgeSignal, Level
from kalshi_microstructure.preflight import preflight_limit_order, preflight_payload


def _emit(obj: dict) -> None:
    sys.stdout.write(json.dumps(obj, separators=(",", ":")))
    sys.stdout.write("\n")
    sys.stdout.flush()


def _fail(msg: str, **extra) -> None:
    _emit({"ok": False, "error": msg, **extra})


def _require_crypto():
    try:
        from cryptography.hazmat.primitives import hashes  # noqa: F401
        import requests  # noqa: F401
        return True
    except Exception as exc:
        _fail("cryptography / requests is not installed", detail=str(exc))
        return False


def _require_requests():
    try:
        import requests  # noqa: F401
        return True
    except Exception as exc:
        _fail("requests is not installed", detail=str(exc))
        return False


def _has_credentials(payload: dict) -> bool:
    return bool(payload.get("api_key_id") and payload.get("private_key_pem"))


_API_PREFIX = "/trade-api/v2"


def _base_url(use_demo: bool) -> str:
    return "https://external-api.demo.kalshi.co" + _API_PREFIX if use_demo else \
           "https://external-api.kalshi.com" + _API_PREFIX


def _load_private_key(pem: str):
    from cryptography.hazmat.primitives.serialization import load_pem_private_key
    return load_pem_private_key(pem.encode("utf-8"), password=None)


def _sign_text(priv, text: str) -> str:
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding

    sig = priv.sign(
        text.encode("utf-8"),
        padding.PSS(
            mgf=padding.MGF1(hashes.SHA256()),
            salt_length=padding.PSS.DIGEST_LENGTH,
        ),
        hashes.SHA256(),
    )
    return base64.b64encode(sig).decode("utf-8")


def _auth_headers(payload: dict, method: str, signing_path: str) -> dict:
    api_key_id = payload["api_key_id"]
    pem = payload["private_key_pem"]
    priv = _load_private_key(pem)

    ts = str(int(datetime.datetime.now().timestamp() * 1000))
    # Kalshi signs `timestamp_ms + METHOD + path` where path is the full path
    # under the host *including* the /trade-api/v2 prefix and *excluding* the
    # query string. Confirmed against docs.kalshi.com quickstart (Apr 2026).
    signature = _sign_text(priv, ts + method.upper() + signing_path)
    return {
        "KALSHI-ACCESS-KEY": api_key_id,
        "KALSHI-ACCESS-SIGNATURE": signature,
        "KALSHI-ACCESS-TIMESTAMP": ts,
        "Content-Type": "application/json",
    }


def _request(payload: dict, method: str, path: str, *, params=None, body=None):
    import requests
    base = _base_url(bool(payload.get("use_demo")))
    url = base + path
    signing_path = _API_PREFIX + path  # full path under host, no query string
    headers = _auth_headers(payload, method, signing_path)
    resp = requests.request(method, url, headers=headers, params=params, json=body, timeout=30)
    ok = resp.status_code in (200, 201)
    try:
        data = resp.json()
    except Exception:
        data = {"text": resp.text}
    return ok, resp.status_code, data


def _public_request(payload: dict, method: str, path: str, *, params=None):
    import requests
    base = _base_url(bool(payload.get("use_demo")))
    resp = requests.request(method, base + path, params=params, timeout=30)
    ok = resp.status_code in (200, 201)
    try:
        data = resp.json()
    except Exception:
        data = {"text": resp.text}
    return ok, resp.status_code, data


def _price_from_payload(payload: dict, side: str = "yes") -> float:
    if "price" in payload:
        return _to_float(payload["price"])
    key = f"{side}_price_dollars"
    if key in payload:
        return _to_float(payload[key])
    cents_key = f"{side}_price_cents"
    if cents_key in payload:
        return int(payload[cents_key]) / 100.0
    legacy_key = f"{side}_price"
    if legacy_key in payload:
        return int(payload[legacy_key]) / 100.0
    return 0.0


def _book_from_orderbook_payload(ticker: str, data: dict) -> BinaryBook:
    book = data.get("orderbook_fp") or data.get("orderbook") or {}

    def levels(raw):
        out = []
        for row in raw or []:
            if len(row) < 2:
                continue
            price = _to_float(row[0])
            if price > 1.0:
                price /= 100.0
            out.append(Level(price=price, size=_to_float(row[1])))
        return tuple(sorted(out, key=lambda level: level.price, reverse=True))

    return BinaryBook(
        ticker=ticker,
        yes_bids=levels(book.get("yes_dollars") or book.get("yes") or ()),
        no_bids=levels(book.get("no_dollars") or book.get("no") or ()),
    )


# ── Commands ────────────────────────────────────────────────────────────────


def _to_float(v, default=0.0):
    try:
        return float(v) if v is not None else default
    except (TypeError, ValueError):
        return default


def cmd_fee_quote(payload: dict) -> None:
    side = str(payload.get("side", "yes")).lower()
    price = _price_from_payload(payload, side)
    count = _to_float(payload.get("count", payload.get("contracts", 1.0)))
    fee_rate = _to_float(payload.get("fee_rate"), TAKER_FEE_RATE)
    fee = rounded_fee(price, count, fee_rate)
    notional_cost = max(0.0, price * count)
    _emit({
        "ok": True,
        "price": price,
        "count": count,
        "fee_rate": fee_rate,
        "estimated_fee": fee,
        "fee_per_contract": fee_per_contract(price, count, fee_rate),
        "notional_cost": notional_cost,
        "estimated_total_cost": notional_cost + fee,
    })


def cmd_simulate_taker_buy(payload: dict) -> None:
    price = _price_from_payload(payload, str(payload.get("side", "yes")).lower())
    ask_price = _to_float(payload.get("ask_price"), price)
    count = _to_float(payload.get("size", payload.get("visible_size", payload.get("count", 0.0))))
    signal = EdgeSignal(
        ticker=str(payload.get("ticker", "")),
        side=str(payload.get("side", "YES")).upper(),
        fair_price=_to_float(payload.get("fair_price")),
        ask_price=ask_price,
        size=count,
        edge=_to_float(payload.get("edge")),
        cost=_to_float(payload.get("cost"), ask_price * count),
        expected_value=_to_float(payload.get("expected_value")),
        seconds_to_close=(
            _to_float(payload.get("seconds_to_close"))
            if payload.get("seconds_to_close") is not None else None
        ),
        reason=str(payload.get("reason", "manual")),
        fee_per_contract=_to_float(payload.get("fee_per_contract")),
        net_edge=(
            _to_float(payload.get("net_edge"))
            if payload.get("net_edge") is not None else None
        ),
        net_expected_value=(
            _to_float(payload.get("net_expected_value"))
            if payload.get("net_expected_value") is not None else None
        ),
        slippage_per_contract=_to_float(payload.get("slippage_per_contract")),
    )
    fill = simulate_taker_buy(
        signal,
        max_cost=_to_float(payload.get("max_cost"), signal.cost),
        remaining_budget=_to_float(payload.get("remaining_budget"), signal.cost),
        available_cash=_to_float(payload.get("available_cash"), signal.cost),
        fee_rate=_to_float(payload.get("fee_rate"), TAKER_FEE_RATE),
        liquidity_fraction=_to_float(payload.get("liquidity_fraction"), 1.0),
        queue_ahead_contracts=_to_float(payload.get("queue_ahead_contracts")),
        min_fill_cost=_to_float(payload.get("min_fill_cost"), 0.01),
    )
    if fill is None:
        _emit({"ok": True, "fill": None})
        return
    _emit({
        "ok": True,
        "fill": {
            "shares": fill.shares,
            "cost": fill.cost,
            "fee": fill.fee,
            "total_cost": fill.cost + fill.fee,
            "requested_shares": fill.requested_shares,
            "visible_shares": fill.visible_shares,
            "fillable_shares": fill.fillable_shares,
            "unfilled_shares": fill.unfilled_shares,
            "unfilled_cost": fill.unfilled_cost,
            "fill_ratio": fill.fill_ratio,
        },
    })


def cmd_preflight_order(payload: dict) -> None:
    ticker = str(payload.get("ticker", "")).strip()
    if not ticker:
        _fail("preflight_order requires ticker")
        return

    source = "payload"
    if payload.get("orderbook") or payload.get("orderbook_fp"):
        data = payload
    elif _has_credentials(payload):
        source = "authenticated"
        ok, code, data = _request(payload, "GET", f"/markets/{ticker}/orderbook")
        if not ok:
            _fail(f"HTTP {code}", detail=data)
            return
    else:
        source = "public"
        ok, code, data = _public_request(payload, "GET", f"/markets/{ticker}/orderbook")
        if not ok:
            _fail(f"HTTP {code}", detail=data)
            return

    book = _book_from_orderbook_payload(ticker, data)
    side = str(payload.get("side", "yes")).lower()
    action = str(payload.get("action", "buy")).lower()
    count = _to_float(payload.get("count", payload.get("contracts", 0.0)))
    price = _price_from_payload(payload, side)
    if price <= 0:
        _fail("preflight_order requires price, yes_price_dollars, yes_price_cents, no_price_dollars, or no_price_cents")
        return
    if count <= 0:
        _fail("preflight_order requires positive count/contracts")
        return

    resting_orders = None
    resting_error = None
    if _has_credentials(payload) and bool(payload.get("include_resting_orders", True)):
        params = {"status": "resting", "limit": 500, "ticker": ticker}
        ok_orders, code_orders, orders_data = _request(payload, "GET", "/portfolio/orders", params=params)
        if ok_orders:
            resting_orders = len(orders_data.get("orders", []) or [])
        else:
            resting_error = {"code": code_orders, "detail": orders_data}

    preflight = preflight_limit_order(
        book=book,
        ticker=ticker,
        side=side,
        action=action,
        price=price,
        count=count,
        fee_rate=_to_float(payload.get("fee_rate"), TAKER_FEE_RATE),
        resting_orders=resting_orders,
        max_cost=(
            _to_float(payload.get("max_cost"))
            if payload.get("max_cost") is not None else None
        ),
    )
    out = preflight_payload(preflight)
    if resting_error:
        out["resting_order_check_error"] = resting_error
    _emit({"ok": True, "source": source, "preflight": out})


def cmd_balance(payload: dict) -> None:
    ok, code, data = _request(payload, "GET", "/portfolio/balance")
    if not ok:
        _fail(f"HTTP {code}", detail=data)
        return
    cents = int(data.get("balance", 0))
    portfolio_cents = int(data.get("portfolio_value", cents))
    _emit({
        "ok": True,
        "available": cents / 100.0,
        "total_value": portfolio_cents / 100.0,
        "currency": "USD",
    })

def cmd_positions(payload: dict) -> None:
    ok, code, data = _request(payload, "GET", "/portfolio/positions",
                              params={"limit": 500, "count_filter": "position,total_traded"})
    if not ok:
        _fail(f"HTTP {code}", detail=data)
        return
    positions = []
    # As of Mar 12, 2026 Kalshi removed integer cent fields from responses.
    # Use position_fp (signed contract count, 2dp) and *_dollars (decimal $).
    # Kalshi does not expose unrealized PnL directly — the UI shows 0 rather
    # than the cumulative traded notional (which was mislabeled previously).
    for p in data.get("market_positions", []) or []:
        # V2 currently uses position_fp. Keep the older `position` fallback
        # because it preserves the signed YES/NO direction if Kalshi returns a
        # legacy-shaped response for an archived or migrated market.
        position_fp = _to_float(p.get("position_fp"))
        if position_fp == 0.0 and p.get("position") is not None:
            position_fp = _to_float(p.get("position"))
        market_exposure = _to_float(p.get("market_exposure_dollars"))
        size = abs(position_fp)
        # Kalshi may sign exposure with the YES/NO position direction. Entry
        # price and current notional are magnitudes; direction is carried by
        # `outcome` above.
        exposure_value = abs(market_exposure)
        avg_price = exposure_value / size if size > 0 else 0.0
        positions.append({
            "asset_id": (p.get("ticker") or "") + (":yes" if position_fp >= 0 else ":no"),
            "market_id": p.get("ticker", ""),
            "outcome": "YES" if position_fp >= 0 else "NO",
            "size": size,
            "avg_price": avg_price,
            "realized_pnl": _to_float(p.get("realized_pnl_dollars")),
            "unrealized_pnl": 0.0,
            "current_value": exposure_value,
            "total_traded_dollars": _to_float(p.get("total_traded_dollars")),
            "fees_paid_dollars": _to_float(p.get("fees_paid_dollars")),
            "total_traded": _to_float(p.get("total_traded_dollars")),
            "fees_paid": _to_float(p.get("fees_paid_dollars")),
        })
    _emit({"ok": True, "positions": positions})


def cmd_open_orders(payload: dict) -> None:
    ok, code, data = _request(payload, "GET", "/portfolio/orders",
                              params={"status": "resting", "limit": 500})
    if not ok:
        _fail(f"HTTP {code}", detail=data)
        return
    orders = []
    # Mar 12, 2026: bare `count` / `yes_price` / `no_price` removed from order
    # responses. Use `*_fp` for counts and `*_dollars` for prices.
    for o in data.get("orders", []) or []:
        side = (o.get("side") or "yes").lower()
        price_field = "yes_price_dollars" if side == "yes" else "no_price_dollars"
        initial = _to_float(o.get("initial_count_fp"))
        remaining = _to_float(o.get("remaining_count_fp"))
        # ISO 8601 created_time → epoch ms
        created_ms = 0
        ct = o.get("created_time") or ""
        if ct:
            try:
                created_ms = int(datetime.datetime.fromisoformat(
                    ct.replace("Z", "+00:00")).timestamp() * 1000)
            except ValueError:
                created_ms = 0
        expires_ms = 0
        et = o.get("expiration_time") or ""
        if et:
            try:
                expires_ms = int(datetime.datetime.fromisoformat(
                    et.replace("Z", "+00:00")).timestamp() * 1000)
            except ValueError:
                expires_ms = 0
        orders.append({
            "order_id": o.get("order_id", ""),
            "asset_id": (o.get("ticker") or "") + ":" + side,
            "market_id": o.get("ticker", ""),
            "outcome": side.upper(),
            "side": (o.get("action") or "buy").upper(),
            "order_type": (o.get("type") or "limit").upper(),
            "price": _to_float(o.get(price_field)),
            "size": remaining,
            "filled": max(0.0, initial - remaining),
            "status": (o.get("status") or "").upper(),
            "created_ms": created_ms,
            "expires_ms": expires_ms,
            "client_order_id": o.get("client_order_id", ""),
        })
    _emit({"ok": True, "orders": orders})


def cmd_queue_positions(payload: dict) -> None:
    """Return resting orders joined with their price-time queue positions.

    Kalshi requires a market or event filter on the bulk queue endpoint. For
    the user-friendly no-filter case, discover the account's resting markets
    first and use those tickers as the filter. This command is read-only.
    """
    market_tickers = str(payload.get("market_tickers", "")).strip()
    event_ticker = str(payload.get("event_ticker", "")).strip()
    subaccount = int(payload.get("subaccount", 0))

    orders_ok, orders_code, orders_data = _request(
        payload, "GET", "/portfolio/orders",
        params={"status": "resting", "limit": 1000, "subaccount": subaccount})
    if not orders_ok:
        _fail(f"HTTP {orders_code}", detail=orders_data)
        return
    orders = orders_data.get("orders", []) or []

    if not market_tickers and not event_ticker:
        tickers = sorted({str(o.get("ticker", "")).strip() for o in orders
                          if str(o.get("ticker", "")).strip()})
        market_tickers = ",".join(tickers)
    if not market_tickers and not event_ticker:
        _emit({"ok": True, "queue_positions": [], "resting_orders": 0})
        return

    params = {"subaccount": subaccount}
    if market_tickers:
        params["market_tickers"] = market_tickers
    else:
        params["event_ticker"] = event_ticker
    ok, code, data = _request(payload, "GET",
                              "/portfolio/orders/queue_positions", params=params)
    if not ok:
        _fail(f"HTTP {code}", detail=data)
        return

    orders_by_id = {str(o.get("order_id", "")): o for o in orders}
    rows = []
    for q in data.get("queue_positions", []) or []:
        order_id = str(q.get("order_id", ""))
        order = orders_by_id.get(order_id, {})
        side = str(order.get("side") or order.get("outcome_side") or "").lower()
        price_field = "yes_price_dollars" if side == "yes" else "no_price_dollars"
        ahead = _to_float(q.get("queue_position_fp"))
        rows.append({
            "order_id": order_id,
            "market_ticker": q.get("market_ticker") or order.get("ticker", ""),
            "side": side.upper(),
            "action": str(order.get("action", "")).upper(),
            "price": _to_float(order.get(price_field)),
            "remaining": _to_float(order.get("remaining_count_fp")),
            "queue_position": ahead,
            "queue_position_fp": str(q.get("queue_position_fp", "0.00")),
            "readiness": "FRONT" if ahead <= 0 else ("NEAR" if ahead <= 10 else "WAITING"),
        })
    _emit({"ok": True, "queue_positions": rows,
           "resting_orders": len(orders)})


def cmd_fills(payload: dict) -> None:
    ok, code, data = _request(payload, "GET", "/portfolio/fills",
                              params={"limit": int(payload.get("limit", 100))})
    if not ok:
        _fail(f"HTTP {code}", detail=data)
        return
    # Normalize Kalshi fills to a shape consumers can render without
    # knowing Kalshi-specific field names. ts_ms is epoch ms; price is
    # decimal dollars; side is YES/NO.
    fills_out = []
    for f in data.get("fills", []) or []:
        side = (f.get("side") or "").lower()
        ts_ms = 0
        ct = f.get("created_time") or ""
        if ct:
            try:
                ts_ms = int(datetime.datetime.fromisoformat(
                    ct.replace("Z", "+00:00")).timestamp() * 1000)
            except ValueError:
                ts_ms = 0
        fills_out.append({
            "type": "FILL",
            "fill_id": f.get("fill_id", ""),
            "trade_id": f.get("trade_id", ""),
            "order_id": f.get("order_id", ""),
            "market_id": f.get("ticker") or f.get("market_ticker", ""),
            "side": side.upper(),
            "action": (f.get("action") or "").upper(),
            "size": _to_float(f.get("count_fp")),
            "price": _to_float(f.get("yes_price_dollars") if side == "yes"
                               else f.get("no_price_dollars")),
            "fee_cost": _to_float(f.get("fee_cost")),
            "is_taker": bool(f.get("is_taker", False)),
            "ts_ms": ts_ms,
        })
    _emit({"ok": True, "fills": fills_out})


def cmd_settlements(payload: dict) -> None:
    params = {"limit": int(payload.get("limit", 100))}
    if payload.get("cursor"):
        params["cursor"] = str(payload["cursor"])
    ok, code, data = _request(payload, "GET", "/portfolio/settlements",
                              params=params)
    if not ok:
        _fail(f"HTTP {code}", detail=data)
        return
    settlements_out = []
    for s in data.get("settlements", []) or []:
        yes_count = _to_float(s.get("yes_count_fp"))
        no_count = _to_float(s.get("no_count_fp"))
        yes_cost = _to_float(s.get("yes_total_cost_dollars"))
        no_cost = _to_float(s.get("no_total_cost_dollars"))
        fees = _to_float(s.get("fee_cost"))
        # Revenue remains a legacy integer-cent field in the current endpoint;
        # prefer a future fixed-point dollar field if Kalshi adds one.
        revenue = (_to_float(s.get("revenue_dollars"))
                   if s.get("revenue_dollars") is not None
                   else _to_float(s.get("revenue")) / 100.0)
        ticker = s.get("ticker", "")
        settled_time = s.get("settled_time", "")
        side = ("YES" if yes_count > 0 and no_count == 0 else
                "NO" if no_count > 0 and yes_count == 0 else "MIXED")
        realized_pnl = (revenue - yes_cost - no_cost - fees) if side != "MIXED" else None
        settlements_out.append({
            "internal_id": f"{ticker}:{settled_time}",
            "market_id": ticker,
            "event_ticker": s.get("event_ticker", ""),
            "market_result": (s.get("market_result") or "").upper(),
            "side": side,
            "yes_count": yes_count,
            "no_count": no_count,
            "stake": yes_cost + no_cost,
            "fees": fees,
            "payout": revenue,
            "realized_pnl": realized_pnl,
            "accounting_status": ("exact_one_sided" if realized_pnl is not None
                                  else "mixed_requires_fill_reconciliation"),
            "settled_time": settled_time,
        })
    # Do not depend on exchange response order. ISO-8601 UTC timestamps sort
    # lexicographically, so the newest closed bet is always rendered first.
    settlements_out.sort(key=lambda row: row.get("settled_time", ""), reverse=True)
    _emit({"ok": True, "settlements": settlements_out,
           "cursor": data.get("cursor", "")})


def _event_order_time_in_force(order_type: str, expiration_ts=None) -> str:
    order_type = (order_type or "limit").lower()
    if order_type in ("fok", "fill_or_kill"):
        return "fill_or_kill"
    if order_type in ("fak", "ioc", "immediate_or_cancel"):
        return "immediate_or_cancel"
    if expiration_ts:
        return "good_till_date"
    return "good_till_canceled"


def _event_order_body(order_payload: dict) -> dict:
    """Translate OpenTerminal's YES/NO order shape to Kalshi Event Orders V2."""
    ticker = str(order_payload.get("ticker", "")).strip()
    if not ticker:
        raise ValueError("place_order requires ticker")
    count = _to_float(order_payload.get("count", order_payload.get("contracts", 0)))
    if count <= 0:
        raise ValueError("place_order requires positive count/contracts")

    action = str(order_payload.get("action", "buy")).lower()
    if action not in ("buy", "sell"):
        raise ValueError("action must be buy or sell")
    outcome_side = str(order_payload.get("side", "yes")).lower()
    if outcome_side not in ("yes", "no"):
        raise ValueError("side must be yes or no")

    outcome_price = _price_from_payload(order_payload, outcome_side)
    if outcome_price <= 0:
        raise ValueError("limit order requires price, yes_price_dollars, "
                         "yes_price_cents, no_price_dollars, or no_price_cents")

    # Event Orders V2 expresses every order as bid/ask on the YES leg.
    if outcome_side == "yes":
        event_side = "bid" if action == "buy" else "ask"
        event_price = outcome_price
    else:
        event_side = "ask" if action == "buy" else "bid"
        event_price = 1.0 - outcome_price

    event_price = min(max(event_price, 0.01), 0.99)
    expiration_ts = order_payload.get("expiration_ts")
    order_type = str(order_payload.get("order_type", "limit")).lower()
    body = {
        "ticker": ticker,
        "client_order_id": order_payload.get("client_order_id") or str(uuid.uuid4()),
        "side": event_side,
        "count": f"{count:.2f}",
        "price": f"{event_price:.4f}",
        "time_in_force": _event_order_time_in_force(order_type, expiration_ts),
        "self_trade_prevention_type": str(order_payload.get(
            "self_trade_prevention_type", "taker_at_cross")),
        "post_only": bool(order_payload.get("post_only", False)),
        "cancel_order_on_pause": bool(order_payload.get("cancel_order_on_pause", False)),
        "reduce_only": bool(order_payload.get("reduce_only", False)),
        "subaccount": int(order_payload.get("subaccount", 0)),
        "exchange_index": int(order_payload.get("exchange_index", -1)),
    }
    if expiration_ts:
        body["expiration_time"] = int(expiration_ts)
    return body


def _order_status_from_event_order(data: dict) -> str:
    status = str(data.get("status") or data.get("order_status") or "").upper()
    if status:
        return status
    remaining = _to_float(data.get("remaining_count", data.get("remaining_count_fp")))
    filled = _to_float(data.get("fill_count", data.get("filled_count_fp")))
    if remaining > 0:
        return "RESTING"
    if filled > 0:
        return "FILLED"
    return "SUBMITTED"


def cmd_place_order(payload: dict) -> None:
    try:
        body = _event_order_body(payload)
    except ValueError as exc:
        _fail(str(exc))
        return

    ok, code, data = _request(payload, "POST", "/portfolio/events/orders", body=body)
    if not ok:
        _fail(f"HTTP {code}", detail=data)
        return
    order = data.get("order", data) or {}
    _emit({
        "ok": True,
        "order_id": order.get("order_id", data.get("order_id", "")),
        "status": _order_status_from_event_order(order),
        "client_order_id": order.get("client_order_id", body["client_order_id"]),
        "raw": data,
    })


def cmd_cancel_order(payload: dict) -> None:
    order_id = str(payload["order_id"])
    ok, code, data = _request(payload, "DELETE", f"/portfolio/orders/{order_id}")
    if not ok:
        _fail(f"HTTP {code}", detail=data)
        return
    _emit({"ok": True, "order_id": order_id, "raw": data})


def cmd_decrease_order(payload: dict) -> None:
    order_id = str(payload["order_id"])
    body = {"reduce_by": int(payload.get("reduce_by", 0))}
    ok, code, data = _request(payload, "POST",
                              f"/portfolio/orders/{order_id}/decrease", body=body)
    if not ok:
        _fail(f"HTTP {code}", detail=data)
        return
    _emit({"ok": True, "raw": data})


def cmd_get_order(payload: dict) -> None:
    order_id = str(payload["order_id"])
    ok, code, data = _request(payload, "GET", f"/portfolio/orders/{order_id}")
    if not ok:
        _fail(f"HTTP {code}", detail=data)
        return
    _emit({"ok": True, "order": data.get("order", data)})


def cmd_amend_order(payload: dict) -> None:
    """POST /portfolio/orders/{order_id}/amend

    Kalshi accepts a price change (cents integer OR *_dollars string on
    request) + an updated buy_max_cost in cents. Only include fields the
    caller provided so we don't accidentally reset other constraints.
    """
    order_id = str(payload["order_id"])
    body = {}
    if "yes_price_cents" in payload:
        body["yes_price"] = int(payload["yes_price_cents"])
    elif "no_price_cents" in payload:
        body["no_price"] = int(payload["no_price_cents"])
    if "buy_max_cost" in payload:
        body["buy_max_cost"] = int(payload["buy_max_cost"])
    if "client_order_id" in payload:
        body["client_order_id"] = str(payload["client_order_id"])

    if not body:
        _fail("amend_order requires yes_price_cents, no_price_cents, "
              "or buy_max_cost")
        return

    ok, code, data = _request(payload, "POST",
                              f"/portfolio/orders/{order_id}/amend", body=body)
    if not ok:
        _fail(f"HTTP {code}", detail=data)
        return
    _emit({"ok": True, "order": data.get("order", data)})


def cmd_place_orders_batch(payload: dict) -> None:
    """POST /portfolio/events/orders/batched

    `orders` is a list of order dicts with the same shape accepted by
    `cmd_place_order`. Kalshi returns {orders: [...]} with per-order
    success/error; we surface the whole array for the caller to inspect.
    """
    orders_in = payload.get("orders") or []
    if not isinstance(orders_in, list) or not orders_in:
        _fail("place_orders_batch requires a non-empty `orders` list")
        return

    body = {"orders": []}
    for o in orders_in:
        merged = dict(payload)
        merged.update(o)
        try:
            body["orders"].append(_event_order_body(merged))
        except ValueError as exc:
            _fail(str(exc))
            return

    ok, code, data = _request(payload, "POST", "/portfolio/events/orders/batched",
                              body=body)
    if not ok:
        _fail(f"HTTP {code}", detail=data)
        return
    _emit({"ok": True, "orders": data.get("orders", []) or [], "raw": data})


def cmd_cancel_orders_batch(payload: dict) -> None:
    """DELETE /portfolio/orders/batched

    Kalshi expects the order_ids in the body on DELETE. Sends a single
    round-trip regardless of list size.
    """
    ids = payload.get("order_ids") or []
    if not ids:
        _fail("cancel_orders_batch requires `order_ids`")
        return
    body = {"order_ids": [str(i) for i in ids]}
    ok, code, data = _request(payload, "DELETE", "/portfolio/orders/batched",
                              body=body)
    if not ok:
        _fail(f"HTTP {code}", detail=data)
        return
    _emit({"ok": True, "results": data.get("orders", []) or [], "raw": data})


def cmd_historical_fills(payload: dict) -> None:
    params = {"limit": int(payload.get("limit", 200))}
    if payload.get("cursor"):
        params["cursor"] = str(payload["cursor"])
    if payload.get("ticker"):
        params["ticker"] = str(payload["ticker"])
    if payload.get("min_ts"):
        params["min_ts"] = int(payload["min_ts"])
    if payload.get("max_ts"):
        params["max_ts"] = int(payload["max_ts"])
    ok, code, data = _request(payload, "GET", "/historical/fills",
                              params=params)
    if not ok:
        _fail(f"HTTP {code}", detail=data)
        return
    _emit({
        "ok": True,
        "fills": data.get("fills", []) or [],
        "cursor": data.get("cursor", ""),
    })


def cmd_historical_orders(payload: dict) -> None:
    params = {"limit": int(payload.get("limit", 200))}
    if payload.get("cursor"):
        params["cursor"] = str(payload["cursor"])
    if payload.get("ticker"):
        params["ticker"] = str(payload["ticker"])
    if payload.get("status"):
        params["status"] = str(payload["status"])
    ok, code, data = _request(payload, "GET", "/historical/orders",
                              params=params)
    if not ok:
        _fail(f"HTTP {code}", detail=data)
        return
    _emit({
        "ok": True,
        "orders": data.get("orders", []) or [],
        "cursor": data.get("cursor", ""),
    })


COMMANDS = {
    "fee_quote": cmd_fee_quote,
    "simulate_taker_buy": cmd_simulate_taker_buy,
    "preflight_order": cmd_preflight_order,
    "balance": cmd_balance,
    "positions": cmd_positions,
    "open_orders": cmd_open_orders,
    "queue_positions": cmd_queue_positions,
    "fills": cmd_fills,
    "settlements": cmd_settlements,
    "place_order": cmd_place_order,
    "cancel_order": cmd_cancel_order,
    "decrease_order": cmd_decrease_order,
    "get_order": cmd_get_order,
    "amend_order": cmd_amend_order,
    "place_orders_batch": cmd_place_orders_batch,
    "cancel_orders_batch": cmd_cancel_orders_batch,
    "historical_fills": cmd_historical_fills,
    "historical_orders": cmd_historical_orders,
}

AUTH_COMMANDS = {
    "balance",
    "positions",
    "open_orders",
    "queue_positions",
    "fills",
    "settlements",
    "place_order",
    "cancel_order",
    "decrease_order",
    "get_order",
    "amend_order",
    "place_orders_batch",
    "cancel_orders_batch",
    "historical_fills",
    "historical_orders",
}


def main() -> int:
    if len(sys.argv) < 2:
        _fail("usage: prediction_kalshi.py <command> [<json_payload>]")
        return 2
    command = sys.argv[1]
    payload_str = sys.argv[2] if len(sys.argv) > 2 else "{}"
    try:
        payload = json.loads(payload_str)
    except json.JSONDecodeError as exc:
        _fail(f"invalid JSON payload: {exc}")
        return 2

    handler = COMMANDS.get(command)
    if handler is None:
        _fail(f"unknown command: {command}", available=list(COMMANDS.keys()))
        return 2

    if command in AUTH_COMMANDS or (command == "preflight_order" and _has_credentials(payload)):
        if not _require_crypto():
            return 3
    elif command == "preflight_order" and not (payload.get("orderbook") or payload.get("orderbook_fp")):
        if not _require_requests():
            return 3

    try:
        handler(payload)
        return 0
    except Exception as exc:
        traceback.print_exc(file=sys.stderr)
        _fail(f"{type(exc).__name__}: {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
