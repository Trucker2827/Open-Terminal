from __future__ import annotations

import json
import time
import asyncio
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from .auth import KalshiCredentials
from .book_cache import KalshiBookCache
from .fees import TAKER_FEE_RATE, net_expected_value, rounded_fee
from .kalshi import KalshiRestClient
from .models import EdgeSignal, Market
from .orders import LimitOrder
from .paper import PaperLedger
from .preflight import preflight_limit_order, preflight_payload
from .reference import reference_spot
from .spot_ws import SpotCache, maintain_spot_cache
from .strategy import find_edge, market_threshold
from .ws import KalshiWebSocketClient


@dataclass(frozen=True)
class BotConfig:
    series: str = "KXBTC15M"
    symbol: str = "BTC-USD"
    poll_seconds: float = 2.0
    run_seconds: float = 60.0
    annual_volatility: float = 0.70
    dynamic_volatility: bool = False
    vol_lookback_seconds: float = 60.0
    min_annual_volatility: float = 0.20
    max_annual_volatility: float = 2.50
    min_edge: float = 0.03
    min_size: float = 1.0
    max_order_cost: float = 2.0
    max_run_cost: float = 10.0
    max_market_cost: float | None = None
    max_entry_price: float | None = None
    max_trades_per_market: int = 1
    cooldown_seconds: float = 10.0
    max_entry_seconds_to_close: float | None = None
    max_spot_age_seconds: float = 2.0
    settlement_average_seconds: float = 0.0
    min_settlement_proxy_coverage_seconds: float = 0.0
    require_settlement_proxy: bool = False
    fee_rate: float = TAKER_FEE_RATE
    slippage_per_contract: float = 0.0
    log_path: Path = Path("logs/bot.jsonl")
    live: bool = False
    require_live_preflight: bool = True
    allow_live_preflight_warnings: bool = False
    max_live_order_total: float | None = None
    allow_live_requote: bool = True
    allow_live_opposite_side: bool = False
    live_focus_symbol: str | None = None
    live_focus_side: str | None = None
    live_focus_allow_spot_side_mismatch: bool = False


@dataclass
class BotState:
    ledger: PaperLedger
    spent_this_run: float = 0.0
    last_trade_at: dict[str, float] | None = None
    trades_by_market: dict[str, int] | None = None
    spent_by_market: dict[str, float] | None = None
    live_positions: dict[tuple[str, str], float] | None = None

    def __post_init__(self) -> None:
        if self.last_trade_at is None:
            self.last_trade_at = {}
        if self.trades_by_market is None:
            self.trades_by_market = {}
        if self.spent_by_market is None:
            self.spent_by_market = {}
        if self.live_positions is None:
            self.live_positions = {}


class JsonlLogger:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)

    def write(self, event: str, payload: dict[str, Any]) -> None:
        row = {
            "ts": datetime.now(timezone.utc).isoformat(),
            "event": event,
            **payload,
        }
        with self.path.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(row, sort_keys=True) + "\n")


def run_bot(client: KalshiRestClient, config: BotConfig) -> BotState:
    logger = JsonlLogger(config.log_path)
    state = BotState(ledger=PaperLedger(cash=config.max_run_cost))
    deadline = time.monotonic() + config.run_seconds
    logger.write("bot_started", _config_payload(config))

    while time.monotonic() < deadline:
        started = time.monotonic()
        try:
            run_once(client, config, state, logger)
        except Exception as exc:  # noqa: BLE001 - bot loop should log and keep breathing.
            logger.write("error", {"kind": exc.__class__.__name__, "message": str(exc)})
            print(f"bot error: {exc}", flush=True)

        sleep_for = max(0.0, config.poll_seconds - (time.monotonic() - started))
        if sleep_for:
            time.sleep(sleep_for)

    logger.write(
        "bot_stopped",
        {
            "cash": state.ledger.cash,
            "spent_this_run": state.spent_this_run,
            "positions": {f"{ticker}:{side}": size for (ticker, side), size in state.ledger.positions.items()},
            "live_positions": {f"{ticker}:{side}": size for (ticker, side), size in state.live_positions.items()},
        },
    )
    return state


async def run_websocket_bot(
    client: KalshiRestClient,
    credentials: KalshiCredentials,
    env: str,
    config: BotConfig,
) -> BotState:
    logger = JsonlLogger(config.log_path)
    state = BotState(ledger=PaperLedger(cash=config.max_run_cost))
    deadline = time.monotonic() + config.run_seconds
    logger.write("bot_started", {**_config_payload(config), "mode": "websocket"})
    spot_cache = SpotCache(config.symbol)
    spot_task = asyncio.create_task(maintain_spot_cache(spot_cache))

    try:
        while time.monotonic() < deadline:
            market = find_active_market(client, config.series)
            if market is None:
                logger.write("no_market", {"series": config.series})
                await asyncio.sleep(min(2.0, max(0.0, deadline - time.monotonic())))
                continue

            print(f"{market.ticker}: websocket stream started", flush=True)
            logger.write(
                "websocket_market",
                {
                    "ticker": market.ticker,
                    "title": market.title,
                    "threshold": market_threshold(market),
                    "close_time": market.close_time.isoformat() if market.close_time else None,
                },
            )
            cache = KalshiBookCache(market.ticker)
            ws_client = KalshiWebSocketClient(credentials=credentials, env=env)
            next_eval = 0.0

            try:
                async for message in ws_client.stream(
                    market_tickers=[market.ticker],
                    channels=["orderbook_delta"],
                ):
                    now = time.monotonic()
                    if now >= deadline:
                        break

                    cache.apply(message)
                    if message.get("type") not in {"orderbook_snapshot", "orderbook_delta"}:
                        continue

                    if market.close_time and datetime.now(timezone.utc) >= market.close_time:
                        logger.write("market_closed", {"ticker": market.ticker})
                        break

                    if cache.seq is None or now < next_eval:
                        continue

                    evaluate_cached_book(
                        client,
                        config,
                        state,
                        logger,
                        market,
                        cache,
                        spot=spot_cache.price,
                        spot_age_seconds=spot_cache.age_seconds,
                        annual_volatility=current_annual_volatility(config, spot_cache),
                        volatility_tick_count=spot_cache.tick_count,
                    )
                    next_eval = now + config.poll_seconds
            except Exception as exc:  # noqa: BLE001 - reconnect loop should survive transient WS failures.
                logger.write(
                    "websocket_error",
                    {"ticker": market.ticker, "kind": exc.__class__.__name__, "message": str(exc)},
                )
                print(f"{market.ticker}: websocket error: {exc}", flush=True)
                await asyncio.sleep(1.0)
    finally:
        spot_task.cancel()
        try:
            await spot_task
        except asyncio.CancelledError:
            pass

    logger.write(
        "bot_stopped",
        {
            "mode": "websocket",
            "cash": state.ledger.cash,
            "spent_this_run": state.spent_this_run,
            "positions": {f"{ticker}:{side}": size for (ticker, side), size in state.ledger.positions.items()},
            "live_positions": {f"{ticker}:{side}": size for (ticker, side), size in state.live_positions.items()},
        },
    )
    return state


def run_once(
    client: KalshiRestClient,
    config: BotConfig,
    state: BotState,
    logger: JsonlLogger,
) -> EdgeSignal | None:
    market = find_active_market(client, config.series)
    if market is None:
        logger.write("no_market", {"series": config.series})
        return None

    book = client.get_orderbook(market.ticker)
    spot = reference_spot(config.symbol)
    signal = find_edge(
        market=market,
        book=book,
        spot=spot,
        annual_volatility=config.annual_volatility,
        min_edge=config.min_edge,
        min_size=config.min_size,
        fee_rate=config.fee_rate,
        slippage_per_contract=config.slippage_per_contract,
    )

    logger.write(
        "scan",
        {
            "ticker": market.ticker,
            "title": market.title,
            "spot": spot,
            "threshold": market_threshold(market),
            "yes_bid": _level_payload(book.best_yes_bid),
            "yes_ask": _level_payload(book.best_yes_ask),
            "no_bid": _level_payload(book.best_no_bid),
            "no_ask": _level_payload(book.best_no_ask),
            "signal": _signal_payload(signal),
        },
    )

    if signal is None:
        print(f"{market.ticker}: no signal", flush=True)
        return None

    if not should_trade(signal, config, state, now=time.monotonic()):
        logger.write("signal_skipped", {"signal": _signal_payload(signal), "reason": "risk_gate"})
        print(f"{market.ticker}: signal skipped by risk gate", flush=True)
        return signal

    cost = target_order_cost(signal, config, state)
    if cost <= 0:
        logger.write("signal_skipped", {"signal": _signal_payload(signal), "reason": "run_budget"})
        return signal
    shares = cost / signal.ask_price if signal.ask_price > 0 else 0.0
    fee = rounded_fee(signal.ask_price, shares, config.fee_rate)
    net_ev = net_expected_value(
        fair_price=signal.fair_price,
        entry_price=signal.ask_price,
        contracts=shares,
        fee_rate=config.fee_rate,
        slippage_per_contract=config.slippage_per_contract,
    )
    if net_ev <= 0:
        logger.write(
            "signal_skipped",
            {"signal": _signal_payload(signal), "reason": "net_ev", "shares": shares, "fee": fee, "net_ev": net_ev},
        )
        print(f"{market.ticker}: signal skipped net_ev={net_ev:.4f} fee={fee:.2f}", flush=True)
        return signal

    if config.live:
        plan = live_order_plan(client, signal, target_cost=cost, config=config)
        preflight = plan["preflight"]
        logger.write("live_preflight", {"signal": _signal_payload(signal), "preflight": preflight})
        if not plan["approved"]:
            logger.write(
                "signal_skipped",
                {"signal": _signal_payload(signal), "reason": "live_preflight", "preflight": preflight},
            )
            print(f"{signal.ticker}: LIVE preflight blocked order warnings={preflight['warnings']}", flush=True)
            return signal
        response = submit_live_signal(client, signal, plan["shares"], price=plan["price"])
        logger.write("live_order_submitted", {"signal": _signal_payload(signal), "response": response})
        live_fill = live_fill_payload(response, signal, price=plan["price"])
        logger.write("live_order_result", {"signal": _signal_payload(signal), "fill": live_fill})
        if live_fill["fill_count"] <= 0:
            print(f"{signal.ticker}: LIVE IOC did not fill", flush=True)
            return signal
        cost = live_fill["fill_cost"]
        record_live_fill(state, signal, live_fill["fill_count"])
        print(f"{signal.ticker}: LIVE order submitted for {plan['shares']:.2f} {signal.side}", flush=True)
    else:
        shares = state.ledger.buy(signal, max_cost=cost)
        logger.write(
            "paper_order",
            {
                "signal": _signal_payload(signal),
                "shares": shares,
                "cost": cost,
                "fee": fee,
                "net_ev": net_ev,
                "cash": state.ledger.cash,
            },
        )
        print(
            f"{signal.ticker}: PAPER BUY {signal.side} {shares:.2f} @ {signal.ask_price:.4f} "
            f"edge={signal.edge:.4f}",
            flush=True,
        )

    state.spent_this_run += cost
    state.spent_by_market[signal.ticker] = state.spent_by_market.get(signal.ticker, 0.0) + cost
    state.last_trade_at[signal.ticker] = time.monotonic()
    state.trades_by_market[signal.ticker] = state.trades_by_market.get(signal.ticker, 0) + 1
    return signal


def evaluate_cached_book(
    client: KalshiRestClient,
    config: BotConfig,
    state: BotState,
    logger: JsonlLogger,
    market: Market,
    cache: KalshiBookCache,
    spot: float | None = None,
    spot_age_seconds: float | None = None,
    annual_volatility: float | None = None,
    volatility_tick_count: int | None = None,
) -> EdgeSignal | None:
    if spot_age_seconds is not None and spot_age_seconds > config.max_spot_age_seconds:
        logger.write(
            "scan_skipped",
            {
                "ticker": market.ticker,
                "reason": "stale_spot",
                "spot_age_seconds": spot_age_seconds,
                "max_spot_age_seconds": config.max_spot_age_seconds,
            },
        )
        print(f"{market.ticker}: skipped stale spot age={spot_age_seconds:.2f}s", flush=True)
        return None

    book = cache.to_book()
    if spot is None:
        spot = reference_spot(config.symbol)
    annual_volatility = annual_volatility or config.annual_volatility
    signal = find_edge(
        market=market,
        book=book,
        spot=spot,
        annual_volatility=annual_volatility,
        min_edge=config.min_edge,
        min_size=config.min_size,
        fee_rate=config.fee_rate,
        slippage_per_contract=config.slippage_per_contract,
    )
    logger.write(
        "ws_scan",
        {
            "ticker": market.ticker,
            "seq": cache.seq,
            "spot": spot,
            "spot_age_seconds": spot_age_seconds,
            "annual_volatility": annual_volatility,
            "volatility_tick_count": volatility_tick_count,
            "threshold": market_threshold(market),
            "yes_bid": _level_payload(book.best_yes_bid),
            "yes_ask": _level_payload(book.best_yes_ask),
            "no_bid": _level_payload(book.best_no_bid),
            "no_ask": _level_payload(book.best_no_ask),
            "signal": _signal_payload(signal),
        },
    )

    if signal is None:
        print(f"{market.ticker}: ws no signal seq={cache.seq}", flush=True)
        return None

    if not should_trade(signal, config, state, now=time.monotonic()):
        logger.write("signal_skipped", {"signal": _signal_payload(signal), "reason": "risk_gate"})
        print(f"{market.ticker}: ws signal skipped by risk gate", flush=True)
        return signal

    cost = target_order_cost(signal, config, state)
    if cost <= 0:
        logger.write("signal_skipped", {"signal": _signal_payload(signal), "reason": "run_budget"})
        return signal
    shares = cost / signal.ask_price if signal.ask_price > 0 else 0.0
    fee = rounded_fee(signal.ask_price, shares, config.fee_rate)
    net_ev = net_expected_value(
        fair_price=signal.fair_price,
        entry_price=signal.ask_price,
        contracts=shares,
        fee_rate=config.fee_rate,
        slippage_per_contract=config.slippage_per_contract,
    )
    if net_ev <= 0:
        logger.write(
            "signal_skipped",
            {"signal": _signal_payload(signal), "reason": "net_ev", "shares": shares, "fee": fee, "net_ev": net_ev},
        )
        print(f"{market.ticker}: ws signal skipped net_ev={net_ev:.4f} fee={fee:.2f}", flush=True)
        return signal

    if config.live:
        plan = live_order_plan(client, signal, target_cost=cost, config=config)
        preflight = plan["preflight"]
        logger.write("live_preflight", {"signal": _signal_payload(signal), "preflight": preflight})
        if not plan["approved"]:
            logger.write(
                "signal_skipped",
                {"signal": _signal_payload(signal), "reason": "live_preflight", "preflight": preflight},
            )
            print(f"{signal.ticker}: LIVE ws preflight blocked order warnings={preflight['warnings']}", flush=True)
            return signal
        response = submit_live_signal(client, signal, plan["shares"], price=plan["price"])
        logger.write("live_order_submitted", {"signal": _signal_payload(signal), "response": response})
        live_fill = live_fill_payload(response, signal, price=plan["price"])
        logger.write("live_order_result", {"signal": _signal_payload(signal), "fill": live_fill})
        if live_fill["fill_count"] <= 0:
            print(f"{signal.ticker}: LIVE ws IOC did not fill", flush=True)
            return signal
        cost = live_fill["fill_cost"]
        record_live_fill(state, signal, live_fill["fill_count"])
        print(f"{signal.ticker}: LIVE ws order submitted for {plan['shares']:.2f} {signal.side}", flush=True)
    else:
        shares = state.ledger.buy(signal, max_cost=cost)
        logger.write(
            "paper_order",
            {
                "mode": "websocket",
                "signal": _signal_payload(signal),
                "shares": shares,
                "cost": cost,
                "fee": fee,
                "net_ev": net_ev,
                "cash": state.ledger.cash,
            },
        )
        print(
            f"{signal.ticker}: PAPER WS BUY {signal.side} {shares:.2f} @ {signal.ask_price:.4f} "
            f"edge={signal.edge:.4f}",
            flush=True,
        )

    state.spent_this_run += cost
    state.spent_by_market[signal.ticker] = state.spent_by_market.get(signal.ticker, 0.0) + cost
    state.last_trade_at[signal.ticker] = time.monotonic()
    state.trades_by_market[signal.ticker] = state.trades_by_market.get(signal.ticker, 0) + 1
    return signal


def find_active_market(client: KalshiRestClient, series: str) -> Market | None:
    markets, _ = client.get_markets(series_ticker=series, status="open", limit=20)
    active = [market for market in markets if market.status in {"active", "open"}]
    if not active:
        return None
    return min(
        active,
        key=lambda market: market.close_time or datetime.max.replace(tzinfo=timezone.utc),
    )


def should_trade(signal: EdgeSignal, config: BotConfig, state: BotState, now: float) -> bool:
    if config.max_entry_price is not None and signal.ask_price > config.max_entry_price:
        return False
    if state.spent_this_run >= config.max_run_cost:
        return False
    if config.max_market_cost is not None and state.spent_by_market.get(signal.ticker, 0.0) >= config.max_market_cost:
        return False
    if state.trades_by_market.get(signal.ticker, 0) >= config.max_trades_per_market:
        return False
    if config.live and not config.allow_live_opposite_side and has_live_opposite_side(state, signal):
        return False
    last_trade = state.last_trade_at.get(signal.ticker)
    if last_trade is not None and now - last_trade < config.cooldown_seconds:
        return False
    return True


def target_order_cost(signal: EdgeSignal, config: BotConfig, state: BotState) -> float:
    remaining_run_budget = config.max_run_cost - state.spent_this_run
    remaining_market_budget = remaining_run_budget
    if config.max_market_cost is not None:
        remaining_market_budget = config.max_market_cost - state.spent_by_market.get(signal.ticker, 0.0)
    return min(config.max_order_cost, remaining_run_budget, remaining_market_budget, signal.cost)


def record_live_fill(state: BotState, signal: EdgeSignal, fill_count: float) -> None:
    if fill_count <= 0:
        return
    side = signal.side.upper()
    key = (signal.ticker, side)
    state.live_positions[key] = state.live_positions.get(key, 0.0) + fill_count


def has_live_opposite_side(state: BotState, signal: EdgeSignal) -> bool:
    opposite_key = (signal.ticker, _opposite_side(signal.side))
    return state.live_positions.get(opposite_key, 0.0) > 0


def _opposite_side(side: str) -> str:
    return "NO" if side.upper() == "YES" else "YES"


def submit_live_signal(
    client: KalshiRestClient,
    signal: EdgeSignal,
    shares: float,
    *,
    price: float | None = None,
) -> dict[str, Any]:
    order_price = signal.ask_price if price is None else price
    order = LimitOrder(
        ticker=signal.ticker,
        side=signal.side.lower(),
        action="buy",
        price=order_price,
        count=shares,
        time_in_force="immediate_or_cancel",
        post_only=False,
    )
    order.validate(max_buy_cost=order_price * shares)
    return client.create_order(order.to_api())


def live_order_plan(
    client: KalshiRestClient,
    signal: EdgeSignal,
    *,
    target_cost: float,
    config: BotConfig,
) -> dict[str, Any]:
    if target_cost <= 0 or signal.ask_price <= 0:
        return {
            "approved": False,
            "price": signal.ask_price,
            "shares": 0.0,
            "preflight": {"approved": False, "warnings": ["invalid_target_cost"]},
        }
    shares = target_cost / signal.ask_price
    preflight = live_preflight_payload(client, signal, shares, config, price=signal.ask_price)
    price = signal.ask_price

    if (
        not preflight.get("approved")
        and config.allow_live_requote
        and "buy_order_would_rest" in (preflight.get("warnings") or [])
        and preflight.get("best_ask") is not None
    ):
        reprice = float(preflight["best_ask"])
        reshares = target_cost / reprice if reprice > 0 else 0.0
        net_edge = _net_edge_at_price(signal, reprice, reshares, config)
        if net_edge >= config.min_edge:
            requoted = live_preflight_payload(client, signal, reshares, config, price=reprice)
            requoted["requote"] = True
            requoted["original_price"] = signal.ask_price
            requoted["net_edge_at_price"] = net_edge
            preflight = requoted
            price = reprice
            shares = reshares
        else:
            warnings = list(preflight.get("warnings") or [])
            warnings.append("requote_edge_below_min")
            preflight["warnings"] = warnings
            preflight["requote_attempted"] = True
            preflight["requote_price"] = reprice
            preflight["net_edge_at_requote"] = net_edge

    return {
        "approved": bool(preflight.get("approved")),
        "price": price,
        "shares": shares,
        "preflight": preflight,
    }


def live_preflight_payload(
    client: KalshiRestClient,
    signal: EdgeSignal,
    shares: float,
    config: BotConfig,
    *,
    price: float | None = None,
) -> dict[str, Any]:
    if not config.require_live_preflight:
        return {"approved": True, "warnings": [], "disabled": True}
    order_price = signal.ask_price if price is None else price
    try:
        book = client.get_orderbook(signal.ticker)
        orders, _ = client.get_orders(ticker=signal.ticker, status="resting", limit=100)
        preflight = preflight_payload(
            preflight_limit_order(
                book=book,
                ticker=signal.ticker,
                side=signal.side,
                action="buy",
                price=order_price,
                count=shares,
                fee_rate=config.fee_rate,
                resting_orders=len(orders),
                max_cost=config.max_live_order_total,
            )
        )
    except Exception as exc:  # noqa: BLE001 - failed preflight should block live orders.
        return {
            "approved": False,
            "warnings": ["preflight_error"],
            "error": f"{exc.__class__.__name__}: {exc}",
        }
    warnings = list(preflight["warnings"])
    approved = bool(preflight["would_cross"]) and (not warnings or config.allow_live_preflight_warnings)
    preflight["approved"] = approved
    return preflight


def live_fill_payload(response: dict[str, Any], signal: EdgeSignal, *, price: float | None = None) -> dict[str, Any]:
    order = response.get("order", response)
    fill_count = _float_field(order, "fill_count_fp")
    fill_cost = (
        _float_field(order, "taker_fill_cost_dollars")
        + _float_field(order, "maker_fill_cost_dollars")
    )
    fees = (
        _float_field(order, "taker_fees_dollars")
        + _float_field(order, "maker_fees_dollars")
    )
    if fill_count > 0 and fill_cost <= 0:
        fill_cost = fill_count * (signal.ask_price if price is None else price)
    return {
        "order_id": order.get("order_id"),
        "status": order.get("status"),
        "fill_count": fill_count,
        "fill_cost": fill_cost,
        "fees": fees,
        "remaining_count": _float_field(order, "remaining_count_fp"),
    }


def _net_edge_at_price(signal: EdgeSignal, price: float, shares: float, config: BotConfig) -> float:
    fee_per_contract = rounded_fee(price, shares, config.fee_rate) / shares if shares > 0 else 0.0
    return signal.fair_price - price - fee_per_contract - config.slippage_per_contract


def _float_field(payload: dict[str, Any], key: str) -> float:
    value = payload.get(key)
    if value in (None, ""):
        return 0.0
    try:
        return float(value)
    except (TypeError, ValueError):
        return 0.0


def current_annual_volatility(config: BotConfig, spot_cache: SpotCache) -> float:
    if not config.dynamic_volatility:
        return config.annual_volatility
    realized = spot_cache.realized_volatility(
        lookback_seconds=config.vol_lookback_seconds,
        floor=config.min_annual_volatility,
        cap=config.max_annual_volatility,
    )
    return realized if realized is not None else config.annual_volatility


def _level_payload(level: object) -> dict[str, float] | None:
    if level is None:
        return None
    return {"price": level.price, "size": level.size}


def _signal_payload(signal: EdgeSignal | None) -> dict[str, Any] | None:
    if signal is None:
        return None
    return asdict(signal)


def _config_payload(config: BotConfig) -> dict[str, Any]:
    payload = asdict(config)
    payload["log_path"] = str(config.log_path)
    return payload
