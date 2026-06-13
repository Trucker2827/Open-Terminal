from __future__ import annotations

import asyncio
import json
import os
import subprocess
import sys
import threading
import time
from dataclasses import asdict, dataclass, field
from datetime import datetime, timedelta, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse

from .auth import KalshiCredentials
from .book_cache import KalshiBookCache
from .cfbenchmarks import (
    CF_INDEX_BY_SYMBOL,
    CFValueCache,
    maintain_kalshi_cf_cache,
    settlement_state_payload,
    tick_payload,
)
from .cf_liquidity import CFLiquidityConfig, cf_liquidity_payload
from .fees import TAKER_FEE_RATE, rounded_fee
from .execution import simulate_taker_buy
from .kalshi import KalshiRestClient
from .log_reconcile import audit_bot_log, audit_payload
from .models import BinaryBook, EdgeSignal, Market
from .settlement import MarketSettlement, payout_price, settlement_from_market
from .spot_ws import SpotCache, maintain_spot_cache
from .strategy import find_edge, market_threshold
from .ws import KalshiWebSocketClient


@dataclass(frozen=True)
class DashboardConfig:
    env: str = "prod"
    series: str = "KXBTC15M"
    symbol: str = "BTC-USD"
    watchlist: tuple[tuple[str, str], ...] = (
        ("KXBTC15M", "BTC-USD"),
        ("KXETH15M", "ETH-USD"),
        ("KXSOL15M", "SOL-USD"),
        ("KXDOGE15M", "DOGE-USD"),
    )
    poll_seconds: float = 1.0
    annual_volatility: float = 0.70
    dynamic_volatility: bool = True
    vol_lookback_seconds: float = 30.0
    min_annual_volatility: float = 0.20
    max_annual_volatility: float = 2.50
    min_edge: float = 0.03
    min_size: float = 1.0
    fee_rate: float = TAKER_FEE_RATE
    slippage_per_contract: float = 0.001
    max_spot_age_seconds: float = 2.0
    allow_live_cancel: bool = False
    paper_trading: bool = True
    max_paper_order_cost: float = 2.0
    max_paper_run_cost: float = 10.0
    max_paper_trades_per_market: int = 1
    paper_cooldown_seconds: float = 10.0
    paper_liquidity_fraction: float = 0.25
    paper_queue_ahead_contracts: float = 0.0
    min_paper_fill_cost: float = 0.01
    live_log_glob: str = "logs/live-session-bot*.jsonl"
    autopilot_log_path: str = "logs/autopilot-supervisor.jsonl"
    autopilot_config_path: str = "config/autopilot.json"
    autopilot_output_path: str = "logs/dashboard-autopilot-control.log"
    external_order_cooldown_hours: float = 24.0


@dataclass
class DashboardSnapshot:
    updated_at: str | None = None
    status: str = "starting"
    error: str | None = None
    market: dict[str, Any] | None = None
    book: dict[str, Any] | None = None
    spot: dict[str, Any] | None = None
    cf: dict[str, Any] | None = None
    signal: dict[str, Any] | None = None
    account: dict[str, Any] | None = None
    manual_trading: dict[str, Any] | None = None
    live_audit: dict[str, Any] | None = None
    autopilot: dict[str, Any] | None = None
    autopilot_control: dict[str, Any] | None = None
    watchlist: list[dict[str, Any]] = field(default_factory=list)
    risk: dict[str, Any] = field(default_factory=dict)
    paper: dict[str, Any] = field(default_factory=dict)
    history: dict[str, list[dict[str, Any]]] = field(
        default_factory=lambda: {"spot": [], "volatility": [], "signal": [], "equity": []}
    )


@dataclass
class PaperPosition:
    ticker: str
    side: str
    shares: float
    entry_price: float
    cost: float
    entry_fee: float
    opened_at: str
    fair_price: float
    net_edge: float | None


class DashboardPaperBroker:
    def __init__(self, config: DashboardConfig) -> None:
        self.cash = config.max_paper_run_cost
        self.spent = 0.0
        self.realized_pnl = 0.0
        self.positions: list[PaperPosition] = []
        self.fills: list[dict[str, Any]] = []
        self.last_trade_at: dict[str, float] = {}
        self.trades_by_market: dict[str, int] = {}

    def maybe_buy(self, signal: EdgeSignal | None, config: DashboardConfig, now: float) -> dict[str, Any] | None:
        if not config.paper_trading or signal is None:
            return None
        if self.spent >= config.max_paper_run_cost:
            return None
        if self.trades_by_market.get(signal.ticker, 0) >= config.max_paper_trades_per_market:
            return None
        last_trade = self.last_trade_at.get(signal.ticker)
        if last_trade is not None and now - last_trade < config.paper_cooldown_seconds:
            return None

        fill = simulate_taker_buy(
            signal,
            max_cost=config.max_paper_order_cost,
            remaining_budget=max(0.0, config.max_paper_run_cost - self.spent),
            available_cash=self.cash,
            fee_rate=config.fee_rate,
            liquidity_fraction=config.paper_liquidity_fraction,
            queue_ahead_contracts=config.paper_queue_ahead_contracts,
            min_fill_cost=config.min_paper_fill_cost,
        )
        if fill is None:
            return None

        opened_at = datetime.now(timezone.utc).isoformat()
        position = PaperPosition(
            ticker=signal.ticker,
            side=signal.side,
            shares=fill.shares,
            entry_price=signal.ask_price,
            cost=fill.cost,
            entry_fee=fill.fee,
            opened_at=opened_at,
            fair_price=signal.fair_price,
            net_edge=signal.net_edge,
        )
        self.cash -= fill.cost + fill.fee
        self.spent += fill.cost
        self.positions.append(position)
        self.last_trade_at[signal.ticker] = now
        self.trades_by_market[signal.ticker] = self.trades_by_market.get(signal.ticker, 0) + 1
        fill = {
            "ts": opened_at,
            "ticker": signal.ticker,
            "action": "BUY",
            "side": signal.side,
            "shares": fill.shares,
            "price": signal.ask_price,
            "cost": fill.cost,
            "fee": fill.fee,
            "net_edge": signal.net_edge,
            "requested_shares": fill.requested_shares,
            "visible_shares": fill.visible_shares,
            "fillable_shares": fill.fillable_shares,
            "unfilled_cost": fill.unfilled_cost,
            "fill_ratio": fill.fill_ratio,
        }
        self.fills.append(fill)
        del self.fills[:-25]
        return fill

    def unsettled_tickers(self) -> list[str]:
        return sorted({position.ticker for position in self.positions})

    def settle_market(self, settlement: MarketSettlement) -> list[dict[str, Any]]:
        if not settlement.finalized or settlement.result_side is None:
            return []
        settled: list[dict[str, Any]] = []
        remaining: list[PaperPosition] = []
        settled_at = datetime.now(timezone.utc).isoformat()
        for position in self.positions:
            if position.ticker != settlement.ticker:
                remaining.append(position)
                continue
            payout = position.shares * payout_price(position.side, settlement.result_side)
            realized_pnl = payout - position.cost - position.entry_fee
            self.cash += payout
            self.realized_pnl += realized_pnl
            fill = {
                "ts": settled_at,
                "ticker": position.ticker,
                "action": "SETTLE",
                "side": position.side,
                "shares": position.shares,
                "result_side": settlement.result_side,
                "payout": payout,
                "realized_pnl": realized_pnl,
            }
            settled.append(fill)
            self.fills.append(fill)
        self.positions = remaining
        del self.fills[:-25]
        return settled

    def payload(self, book: BinaryBook | None, config: DashboardConfig) -> dict[str, Any]:
        position_rows = [self._position_payload(position, book, config) for position in self.positions]
        liquidation_value = sum(
            row["liquidation_value"] for row in position_rows if row["liquidation_value"] is not None
        )
        unrealized_pnl = sum(row["unrealized_pnl"] for row in position_rows if row["unrealized_pnl"] is not None)
        equity = self.cash + liquidation_value
        return {
            "enabled": config.paper_trading,
            "cash": self.cash,
            "spent": self.spent,
            "equity": equity,
            "realized_pnl": self.realized_pnl,
            "unrealized_pnl": unrealized_pnl,
            "total_pnl": equity - config.max_paper_run_cost,
            "open_positions": len(self.positions),
            "positions": position_rows[-12:],
            "fills": self.fills[-12:],
        }

    def _position_payload(
        self,
        position: PaperPosition,
        book: BinaryBook | None,
        config: DashboardConfig,
    ) -> dict[str, Any]:
        position_book = book if book and book.ticker == position.ticker else None
        mark_price = _mark_price(position.side, position_book)
        mark_value = None
        exit_fee = None
        liquidation_value = None
        unrealized_pnl = None
        if mark_price is not None:
            mark_value = position.shares * mark_price
            exit_fee = rounded_fee(mark_price, position.shares, config.fee_rate)
            liquidation_value = mark_value - exit_fee
            unrealized_pnl = liquidation_value - position.cost - position.entry_fee
        return {
            "ticker": position.ticker,
            "side": position.side,
            "shares": position.shares,
            "entry_price": position.entry_price,
            "mark_price": mark_price,
            "cost": position.cost,
            "entry_fee": position.entry_fee,
            "exit_fee": exit_fee,
            "mark_value": mark_value,
            "liquidation_value": liquidation_value,
            "unrealized_pnl": unrealized_pnl,
            "opened_at": position.opened_at,
            "fair_price": position.fair_price,
            "net_edge": position.net_edge,
        }


class DashboardAutopilotController:
    def __init__(self, client: KalshiRestClient, config: DashboardConfig) -> None:
        self.client = client
        self.config = config
        self.lock = threading.Lock()
        self.process: subprocess.Popen[bytes] | None = None
        self.started_at: str | None = None
        self.last_action: dict[str, Any] | None = None

    def payload(self) -> dict[str, Any]:
        with self.lock:
            mode = "OFF"
            pid = None
            returncode = None
            if self.process is not None:
                returncode = self.process.poll()
                pid = self.process.pid
                mode = "RUNNING" if returncode is None else "OFF"
            return {
                "mode": mode,
                "pid": pid,
                "started_at": self.started_at if mode == "RUNNING" else None,
                "returncode": returncode,
                "last_action": self.last_action,
                "command": self._command(redact=True),
                "risk": self._risk_payload(),
            }

    def start_guarded_cycle(self) -> tuple[HTTPStatus, dict[str, Any]]:
        with self.lock:
            if self.process is not None and self.process.poll() is None:
                returncode = self.process.poll()
                payload = {
                    "mode": "RUNNING" if returncode is None else "OFF",
                    "pid": self.process.pid,
                    "started_at": self.started_at,
                    "returncode": returncode,
                    "last_action": self.last_action,
                    "command": self._command(redact=True),
                    "accepted": False,
                    "reason": "autopilot_already_running",
                }
                return HTTPStatus.CONFLICT, payload

        preflight = self._preflight()
        if not preflight["ok"]:
            self._set_last_action("blocked", preflight)
            return HTTPStatus.CONFLICT, {"accepted": False, **preflight, "control": self.payload()}

        output_path = Path(self.config.autopilot_output_path)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output = output_path.open("ab")
        try:
            process = subprocess.Popen(
                self._command(redact=False),
                cwd=Path.cwd(),
                stdin=subprocess.DEVNULL,
                stdout=output,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        finally:
            output.close()

        started_at = datetime.now(timezone.utc).isoformat()
        with self.lock:
            self.process = process
            self.started_at = started_at
            self.last_action = {
                "action": "start",
                "ts": started_at,
                "pid": process.pid,
                "preflight": preflight,
                "output_path": str(output_path),
            }
        return HTTPStatus.ACCEPTED, {"accepted": True, "control": self.payload(), "preflight": preflight}

    def stop(self) -> tuple[HTTPStatus, dict[str, Any]]:
        no_running = False
        with self.lock:
            process = self.process
            if process is None or process.poll() is not None:
                self.last_action = {
                    "action": "stop_noop",
                    "ts": datetime.now(timezone.utc).isoformat(),
                    "reason": "autopilot_not_running",
                }
                no_running = True
            else:
                process.terminate()
                self.last_action = {
                    "action": "stop",
                    "ts": datetime.now(timezone.utc).isoformat(),
                    "pid": process.pid,
                }
        if no_running:
            return HTTPStatus.OK, {"accepted": False, "reason": "autopilot_not_running", "control": self.payload()}
        return HTTPStatus.OK, {"accepted": True, "control": self.payload()}

    def _preflight(self) -> dict[str, Any]:
        external = _external_autopilot_processes()
        if external:
            return {"ok": False, "reason": "external_autopilot_running", "processes": external}

        risk = self._risk_payload()
        stop_loss = risk["daily_stop_loss"]
        recent_pnl = risk["recent_realized_pnl"]
        if stop_loss is not None and recent_pnl <= -float(stop_loss):
            return {
                "ok": False,
                "reason": "daily_stop_loss",
                "recent_realized_pnl": recent_pnl,
                "daily_stop_loss": float(stop_loss),
            }

        account = self.client.get_balance()
        portfolio_value = _float_or_none(account.get("portfolio_value")) or 0.0
        if portfolio_value > 0:
            return {
                "ok": False,
                "reason": "portfolio_value_active",
                "portfolio_value": portfolio_value,
                "account": _balance_payload(account),
            }

        orders, _ = self.client.get_orders(status="resting", limit=100)
        if orders:
            return {"ok": False, "reason": "resting_orders", "resting_orders": len(orders)}

        recent_orders, _ = self.client.get_orders(limit=25)
        manual = _manual_trading_payload(
            recent_orders,
            cooldown=timedelta(hours=self.config.external_order_cooldown_hours),
        )
        if manual["lock_active"]:
            return {"ok": False, "reason": "recent_external_orders", "manual_trading": manual}

        autopilot_path = Path(self.config.autopilot_log_path)
        autopilot_state = _autopilot_payload(autopilot_path) if autopilot_path.exists() else None
        if autopilot_state and autopilot_state.get("safe_to_run") is False:
            return {"ok": False, "reason": "autopilot_not_safe", "autopilot": autopilot_state}

        return {
            "ok": True,
            "reason": "ready",
            "account": _balance_payload(account),
            "resting_orders": 0,
            "autopilot": autopilot_state,
        }

    def _risk_payload(self) -> dict[str, Any]:
        stop_loss = _autopilot_config_value(Path(self.config.autopilot_config_path), "daily_stop_loss")
        recent_pnl = _recent_supervisor_pnl(Path(self.config.autopilot_log_path), window=timedelta(hours=24))
        blocked = stop_loss is not None and recent_pnl <= -float(stop_loss)
        return {
            "recent_realized_pnl": recent_pnl,
            "daily_stop_loss": float(stop_loss) if stop_loss is not None else None,
            "daily_stop_loss_blocked": blocked,
        }

    def _set_last_action(self, action: str, payload: dict[str, Any]) -> None:
        with self.lock:
            self.last_action = {
                "action": action,
                "ts": datetime.now(timezone.utc).isoformat(),
                **payload,
            }

    def _command(self, *, redact: bool) -> list[str]:
        executable = sys.executable if not redact else "python"
        return [
            executable,
            "-m",
            "kalshi_microstructure.cli",
            "autopilot",
            "--config",
            self.config.autopilot_config_path,
            "--max-cycles",
            "1",
            "--live",
        ]


class DashboardEngine:
    def __init__(
        self,
        *,
        client: KalshiRestClient,
        credentials: KalshiCredentials,
        config: DashboardConfig,
    ) -> None:
        self.client = client
        self.credentials = credentials
        self.config = config
        self.paper = DashboardPaperBroker(config)
        self.autopilot_control = DashboardAutopilotController(client, config)
        self.snapshot = DashboardSnapshot(risk=_risk_payload(config), paper=self.paper.payload(None, config))
        self.lock = threading.Lock()
        self.stop_event = threading.Event()
        self.next_settlement_check = 0.0
        self.next_balance_check = 0.0
        self.next_live_audit_check = 0.0
        self.next_autopilot_check = 0.0
        self.next_manual_check = 0.0

    def start(self) -> threading.Thread:
        thread = threading.Thread(target=lambda: asyncio.run(self.run()), daemon=True)
        thread.start()
        return thread

    def state(self) -> dict[str, Any]:
        with self.lock:
            payload = asdict(self.snapshot)
        payload["autopilot_control"] = self.autopilot_control.payload()
        return payload

    def set_snapshot(self, **updates: Any) -> None:
        with self.lock:
            for key, value in updates.items():
                setattr(self.snapshot, key, value)
            self.snapshot.updated_at = datetime.now(timezone.utc).isoformat()

    def append_history(
        self,
        *,
        spot: dict[str, Any] | None,
        signal: dict[str, Any] | None,
        paper: dict[str, Any] | None,
    ) -> None:
        now = datetime.now(timezone.utc).isoformat()
        with self.lock:
            if spot and spot.get("price") is not None:
                self.snapshot.history["spot"].append({"ts": now, "value": spot["price"]})
            if spot and spot.get("annual_volatility") is not None:
                self.snapshot.history["volatility"].append({"ts": now, "value": spot["annual_volatility"]})
            self.snapshot.history["signal"].append(
                {"ts": now, "value": signal.get("net_edge") if signal else None}
            )
            if paper and paper.get("equity") is not None:
                self.snapshot.history["equity"].append({"ts": now, "value": paper["equity"]})
            for series in self.snapshot.history.values():
                del series[:-240]

    async def run(self) -> None:
        spot_cache = SpotCache(self.config.symbol)
        spot_task = asyncio.create_task(maintain_spot_cache(spot_cache))
        cf_index = CF_INDEX_BY_SYMBOL.get(self.config.symbol)
        cf_cache = CFValueCache(cf_index) if cf_index is not None else None
        cf_task = (
            asyncio.create_task(maintain_kalshi_cf_cache(cf_cache, self.credentials, env=self.config.env))
            if cf_cache is not None
            else None
        )
        watch_caches = {series: SpotCache(symbol) for series, symbol in self.config.watchlist}
        watch_spot_tasks = [
            asyncio.create_task(maintain_spot_cache(cache)) for cache in watch_caches.values()
        ]
        watch_task = asyncio.create_task(self._monitor_watchlist(watch_caches))
        try:
            while not self.stop_event.is_set():
                await self._reconcile_paper_positions()
                await self._refresh_account_balance()
                await self._refresh_manual_trading()
                await self._refresh_live_audit()
                await self._refresh_autopilot()
                market = await asyncio.to_thread(_find_active_market, self.client, self.config.series)
                if market is None:
                    self.set_snapshot(status="waiting", error=f"No active market for {self.config.series}")
                    await asyncio.sleep(2.0)
                    continue
                await self._stream_market(market, spot_cache, cf_cache)
        finally:
            spot_task.cancel()
            if cf_task is not None:
                cf_task.cancel()
            watch_task.cancel()
            for task in watch_spot_tasks:
                task.cancel()
            try:
                await spot_task
            except asyncio.CancelledError:
                pass
            if cf_task is not None:
                try:
                    await cf_task
                except asyncio.CancelledError:
                    pass
            for task in [watch_task, *watch_spot_tasks]:
                try:
                    await task
                except asyncio.CancelledError:
                    pass

    async def _monitor_watchlist(self, spot_caches: dict[str, SpotCache]) -> None:
        while not self.stop_event.is_set():
            rows = await asyncio.gather(
                *[
                    asyncio.to_thread(_watchlist_row, self.client, series, spot_cache, self.config)
                    for series, spot_cache in spot_caches.items()
                ],
                return_exceptions=True,
            )
            payload: list[dict[str, Any]] = []
            for row in rows:
                if isinstance(row, Exception):
                    payload.append({"status": "error", "error": f"{row.__class__.__name__}: {row}"})
                elif row is not None:
                    payload.append(row)
            self.set_snapshot(watchlist=payload)
            await asyncio.sleep(max(2.0, self.config.poll_seconds))

    async def _stream_market(
        self,
        market: Market,
        spot_cache: SpotCache,
        cf_cache: CFValueCache | None,
    ) -> None:
        cache = KalshiBookCache(market.ticker)
        ws_client = KalshiWebSocketClient(credentials=self.credentials, env=self.config.env)
        next_eval = 0.0
        self.set_snapshot(status="streaming", error=None, market=_market_payload(market))

        try:
            async for message in ws_client.stream(
                market_tickers=[market.ticker],
                channels=["orderbook_delta"],
            ):
                if self.stop_event.is_set():
                    break

                cache.apply(message)
                if message.get("type") not in {"orderbook_snapshot", "orderbook_delta"}:
                    continue

                if market.close_time and datetime.now(timezone.utc) >= market.close_time:
                    self.set_snapshot(status="rolling", error=None)
                    break

                now = time.monotonic()
                if cache.seq is None or now < next_eval:
                    continue

                book = cache.to_book()
                signal = _find_dashboard_signal(market, book, spot_cache, self.config)
                self.paper.maybe_buy(signal, self.config, now)
                await self._refresh_account_balance()
                await self._refresh_manual_trading()
                await self._refresh_live_audit()
                await self._refresh_autopilot()
                spot = _spot_payload(spot_cache, self.config)
                cf = _dashboard_cf_payload(cf_cache, market, book)
                signal_payload = _signal_payload(signal)
                paper_payload = self.paper.payload(book, self.config)
                self.append_history(spot=spot, signal=signal_payload, paper=paper_payload)
                self.set_snapshot(
                    status="streaming",
                    error=None,
                    market=_market_payload(market),
                    book=_book_payload(book, cache.seq),
                    spot=spot,
                    cf=cf,
                    signal=signal_payload,
                    paper=paper_payload,
                    risk=_risk_payload(self.config),
                )
                next_eval = now + self.config.poll_seconds
        except Exception as exc:  # noqa: BLE001 - dashboard should reconnect.
            self.set_snapshot(status="error", error=f"{exc.__class__.__name__}: {exc}")
            await asyncio.sleep(1.0)

    async def _refresh_account_balance(self) -> None:
        now = time.monotonic()
        if now < self.next_balance_check:
            return
        self.next_balance_check = now + 10.0
        try:
            payload = await asyncio.to_thread(self.client.get_balance)
        except Exception as exc:  # noqa: BLE001 - balance is useful but not critical to stream.
            self.set_snapshot(account={"error": f"{exc.__class__.__name__}: {exc}"})
            return
        self.set_snapshot(account=_balance_payload(payload))

    async def _refresh_live_audit(self) -> None:
        now = time.monotonic()
        if now < self.next_live_audit_check:
            return
        self.next_live_audit_check = now + 3.0
        path = _latest_log_path(self.config.live_log_glob)
        if path is None:
            self.set_snapshot(live_audit={"error": "No live bot log found", "glob": self.config.live_log_glob})
            return
        try:
            payload = await asyncio.to_thread(lambda: audit_payload(audit_bot_log(path)))
        except Exception as exc:  # noqa: BLE001 - audit is useful but not critical to stream.
            self.set_snapshot(live_audit={"error": f"{exc.__class__.__name__}: {exc}", "path": str(path)})
            return
        self.set_snapshot(live_audit=payload)

    async def _refresh_manual_trading(self) -> None:
        now = time.monotonic()
        if now < self.next_manual_check:
            return
        self.next_manual_check = now + 10.0
        try:
            orders, _ = await asyncio.to_thread(self.client.get_orders, limit=25)
        except Exception as exc:  # noqa: BLE001 - manual detector should not break the stream.
            self.set_snapshot(manual_trading={"error": f"{exc.__class__.__name__}: {exc}"})
            return
        self.set_snapshot(
            manual_trading=_manual_trading_payload(
                orders,
                cooldown=timedelta(hours=self.config.external_order_cooldown_hours),
            )
        )

    async def _refresh_autopilot(self) -> None:
        now = time.monotonic()
        if now < self.next_autopilot_check:
            return
        self.next_autopilot_check = now + 3.0
        path = Path(self.config.autopilot_log_path)
        if not path.exists():
            self.set_snapshot(autopilot={"error": "No autopilot supervisor log found", "path": str(path)})
            return
        try:
            payload = await asyncio.to_thread(lambda: _autopilot_payload(path))
        except Exception as exc:  # noqa: BLE001 - autopilot panel should not break the stream.
            self.set_snapshot(autopilot={"error": f"{exc.__class__.__name__}: {exc}", "path": str(path)})
            return
        self.set_snapshot(autopilot=payload)

    async def _reconcile_paper_positions(self) -> None:
        if not self.config.paper_trading:
            return
        now = time.monotonic()
        if now < self.next_settlement_check:
            return
        self.next_settlement_check = now + 10.0
        for ticker in self.paper.unsettled_tickers():
            try:
                market = await asyncio.to_thread(self.client.get_market, ticker)
            except Exception:
                continue
            self.paper.settle_market(settlement_from_market(market))
        self.set_snapshot(paper=self.paper.payload(None, self.config))


def serve_dashboard(
    *,
    client: KalshiRestClient,
    credentials: KalshiCredentials,
    config: DashboardConfig,
    host: str,
    port: int,
) -> ThreadingHTTPServer:
    engine = DashboardEngine(client=client, credentials=credentials, config=config)
    engine.start()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:  # noqa: N802
            parsed = urlparse(self.path)
            if parsed.path == "/":
                self._send_bytes(HTTPStatus.OK, DASHBOARD_HTML.encode("utf-8"), "text/html; charset=utf-8")
                return
            if parsed.path == "/favicon.ico":
                self._send_bytes(HTTPStatus.NO_CONTENT, b"", "image/x-icon")
                return
            if parsed.path == "/api/state":
                self._send_json(HTTPStatus.OK, engine.state())
                return
            if parsed.path == "/api/open-orders":
                query = parse_qs(parsed.query)
                ticker = (query.get("ticker") or [None])[0]
                orders, _ = client.get_orders(ticker=ticker, status="resting", limit=100)
                self._send_json(HTTPStatus.OK, {"orders": [_order_payload(order) for order in orders]})
                return
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})

        def do_POST(self) -> None:  # noqa: N802
            parsed = urlparse(self.path)
            if parsed.path == "/api/autopilot/start":
                status, payload = engine.autopilot_control.start_guarded_cycle()
                self._send_json(status, payload)
                return
            if parsed.path == "/api/autopilot/stop":
                status, payload = engine.autopilot_control.stop()
                self._send_json(status, payload)
                return
            if parsed.path != "/api/cancel-resting":
                self._send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})
                return

            length = int(self.headers.get("content-length", "0") or 0)
            body = json.loads(self.rfile.read(length).decode("utf-8") or "{}")
            ticker = body.get("ticker")
            if not ticker:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "ticker required"})
                return
            orders, _ = client.get_orders(ticker=ticker, status="resting", limit=100)
            ids = [str(order["order_id"]) for order in orders if order.get("order_id")]
            if not config.allow_live_cancel:
                self._send_json(HTTPStatus.OK, {"dry_run": True, "order_ids": ids})
                return
            response = client.batch_cancel_orders(ids) if ids else {"orders": []}
            self._send_json(HTTPStatus.OK, {"dry_run": False, "order_ids": ids, "response": response})

        def log_message(self, format: str, *args: Any) -> None:
            return

        def _send_json(self, status: HTTPStatus, payload: dict[str, Any]) -> None:
            self._send_bytes(status, json.dumps(payload).encode("utf-8"), "application/json")

        def _send_bytes(self, status: HTTPStatus, payload: bytes, content_type: str) -> None:
            self.send_response(status.value)
            self.send_header("content-type", content_type)
            self.send_header("content-length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    server = ThreadingHTTPServer((host, port), Handler)
    server.engine = engine  # type: ignore[attr-defined]
    return server


def _find_active_market(client: KalshiRestClient, series: str) -> Market | None:
    markets, _ = client.get_markets(series_ticker=series, status="open", limit=20)
    active = [market for market in markets if market.status in {"active", "open"}]
    if not active:
        return None
    return min(active, key=lambda market: market.close_time or datetime.max.replace(tzinfo=timezone.utc))


def _find_dashboard_signal(
    market: Market,
    book: BinaryBook,
    spot_cache: SpotCache,
    config: DashboardConfig,
) -> EdgeSignal | None:
    if spot_cache.price is None:
        return None
    spot_age = spot_cache.age_seconds
    if spot_age is not None and spot_age > config.max_spot_age_seconds:
        return None
    annual_vol = (
        spot_cache.realized_volatility(
            lookback_seconds=config.vol_lookback_seconds,
            floor=config.min_annual_volatility,
            cap=config.max_annual_volatility,
        )
        if config.dynamic_volatility
        else None
    )
    return find_edge(
        market=market,
        book=book,
        spot=spot_cache.price,
        annual_volatility=annual_vol or config.annual_volatility,
        min_edge=config.min_edge,
        min_size=config.min_size,
        fee_rate=config.fee_rate,
        slippage_per_contract=config.slippage_per_contract,
    )


def _watchlist_row(
    client: KalshiRestClient,
    series: str,
    spot_cache: SpotCache,
    config: DashboardConfig,
) -> dict[str, Any]:
    try:
        market = _find_active_market(client, series)
        if market is None:
            return {
                "series": series,
                "symbol": spot_cache.symbol,
                "status": "waiting",
                "error": "No active market",
            }
        book = client.get_orderbook(market.ticker)
        signal = _find_dashboard_signal(market, book, spot_cache, config)
        return {
            "series": series,
            "symbol": spot_cache.symbol,
            "status": "active",
            "market": _market_payload(market),
            "book": _book_payload(book, None),
            "spot": _spot_payload(spot_cache, config),
            "signal": _signal_payload(signal),
        }
    except Exception as exc:  # noqa: BLE001 - one asset should not break the dashboard.
        return {
            "series": series,
            "symbol": spot_cache.symbol,
            "status": "error",
            "error": f"{exc.__class__.__name__}: {exc}",
        }


def _market_payload(market: Market) -> dict[str, Any]:
    return {
        "ticker": market.ticker,
        "title": market.title,
        "status": market.status,
        "threshold": market_threshold(market),
        "close_time": market.close_time.isoformat() if market.close_time else None,
    }


def _book_payload(book: BinaryBook, seq: int | None) -> dict[str, Any]:
    return {
        "seq": seq,
        "yes_bid": _level_payload(book.best_yes_bid),
        "yes_ask": _level_payload(book.best_yes_ask),
        "no_bid": _level_payload(book.best_no_bid),
        "no_ask": _level_payload(book.best_no_ask),
        "yes_spread": book.yes_spread,
    }


def _spot_payload(cache: SpotCache, config: DashboardConfig) -> dict[str, Any]:
    return {
        "symbol": cache.symbol,
        "price": cache.price,
        "age_seconds": cache.age_seconds,
        "exchange_ts": cache.exchange_ts,
        "tick_count": cache.tick_count,
        "annual_volatility": cache.realized_volatility(
            lookback_seconds=config.vol_lookback_seconds,
            floor=config.min_annual_volatility,
            cap=config.max_annual_volatility,
        ),
    }


def _dashboard_cf_payload(cache: CFValueCache | None, market: Market, book: BinaryBook | None = None) -> dict[str, Any] | None:
    if cache is None:
        return {"error": "No CF index configured for symbol"}
    latest = cache.latest
    threshold = market_threshold(market)
    if latest is None:
        return {"index": cache.index, "error": "Waiting for CF Benchmarks ticks"}
    if threshold is None or market.close_time is None:
        return {"index": cache.index, "tick": tick_payload(latest), "error": "Missing market threshold or close time"}
    state = cache.settlement_state(threshold=threshold, window_end=market.close_time)
    state_payload = settlement_state_payload(state)
    return {
        "index": cache.index,
        "tick": tick_payload(latest),
        "settlement_state": state_payload,
        "liquidity": cf_liquidity_payload(state_payload, book, CFLiquidityConfig()),
    }


def _signal_payload(signal: EdgeSignal | None) -> dict[str, Any] | None:
    return asdict(signal) if signal else None


def _level_payload(level: object) -> dict[str, float] | None:
    if level is None:
        return None
    return {"price": level.price, "size": level.size}


def _risk_payload(config: DashboardConfig) -> dict[str, Any]:
    return {
        "min_edge": config.min_edge,
        "fee_rate": config.fee_rate,
        "slippage_per_contract": config.slippage_per_contract,
        "max_spot_age_seconds": config.max_spot_age_seconds,
        "allow_live_cancel": config.allow_live_cancel,
        "dynamic_volatility": config.dynamic_volatility,
        "paper_trading": config.paper_trading,
        "max_paper_order_cost": config.max_paper_order_cost,
        "max_paper_run_cost": config.max_paper_run_cost,
        "max_paper_trades_per_market": config.max_paper_trades_per_market,
        "paper_cooldown_seconds": config.paper_cooldown_seconds,
        "paper_liquidity_fraction": config.paper_liquidity_fraction,
        "paper_queue_ahead_contracts": config.paper_queue_ahead_contracts,
        "min_paper_fill_cost": config.min_paper_fill_cost,
        "live_log_glob": config.live_log_glob,
        "autopilot_log_path": config.autopilot_log_path,
        "autopilot_config_path": config.autopilot_config_path,
        "autopilot_output_path": config.autopilot_output_path,
        "watchlist": [f"{series}:{symbol}" for series, symbol in config.watchlist],
    }


def _balance_payload(payload: dict[str, Any]) -> dict[str, Any]:
    return {
        "balance": payload.get("balance"),
        "balance_dollars": _float_or_none(payload.get("balance_dollars")),
        "portfolio_value": payload.get("portfolio_value"),
        "updated_ts": payload.get("updated_ts"),
        "raw": {
            "balance_dollars": payload.get("balance_dollars"),
            "balance_breakdown": payload.get("balance_breakdown"),
        },
    }


def _float_or_none(value: object) -> float | None:
    if value in (None, ""):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _mark_price(side: str, book: BinaryBook | None) -> float | None:
    if book is None:
        return None
    if side.upper() == "YES":
        return book.best_yes_bid.price if book.best_yes_bid else None
    if side.upper() == "NO":
        return book.best_no_bid.price if book.best_no_bid else None
    return None


def _latest_log_path(pattern: str) -> Path | None:
    candidates = [path for path in Path().glob(pattern) if path.is_file()]
    if not candidates:
        return None
    return max(candidates, key=lambda path: path.stat().st_mtime)


def _external_autopilot_processes() -> list[str]:
    try:
        result = subprocess.run(
            ["pgrep", "-fl", "kalshi.*autopilot"],
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError:
        return []
    current_pid = str(os.getpid())
    rows: list[str] = []
    for line in result.stdout.splitlines():
        if not line.strip():
            continue
        pid = line.split(maxsplit=1)[0]
        if pid == current_pid:
            continue
        if " dashboard " in line or " dashboard --" in line:
            continue
        if "kalshi-edge autopilot" not in line and "kalshi_microstructure.cli autopilot" not in line:
            continue
        rows.append(line.strip())
    return rows


def _autopilot_config_value(path: Path, key: str) -> Any:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(payload, dict):
        return None
    return payload.get(key)


def _recent_supervisor_pnl(path: Path, *, window: timedelta) -> float:
    if not path.exists():
        return 0.0
    cutoff = datetime.now(timezone.utc) - window
    total = 0.0
    for row in _jsonl_rows(path):
        if row.get("event") != "autopilot_cycle_stopped":
            continue
        ts = _parse_iso_datetime(row.get("ts"))
        if ts is None or ts < cutoff:
            continue
        total += _float_or_none(row.get("realized_pnl")) or 0.0
    return total


def _parse_iso_datetime(value: object) -> datetime | None:
    if not isinstance(value, str) or not value:
        return None
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc)


def _autopilot_payload(path: Path) -> dict[str, Any]:
    latest_started = latest_analysis = latest_stopped = latest_finished = None
    for row in _jsonl_rows(path):
        event = row.get("event")
        if event == "autopilot_cycle_started":
            latest_started = row
        elif event == "autopilot_skip_analysis":
            latest_analysis = row
        elif event == "autopilot_cycle_stopped":
            latest_stopped = row
        elif event == "autopilot_finished":
            latest_finished = row

    if latest_started is None and latest_analysis is None and latest_stopped is None:
        return {"path": str(path), "status": "waiting"}

    analysis = (latest_analysis or {}).get("analysis") or (latest_stopped or {}).get("skip_analysis") or {}
    audit = (latest_stopped or {}).get("audit") or {}
    account = (latest_started or {}).get("account") or {}
    recommendation = (latest_analysis or {}).get("recommendation") or (latest_stopped or {}).get("recommendation")
    status = "running"
    if latest_stopped and latest_finished:
        stopped_ts = str(latest_stopped.get("ts") or "")
        finished_ts = str(latest_finished.get("ts") or "")
        status = "stopped" if finished_ts >= stopped_ts else "running"
    return {
        "path": str(path),
        "status": status,
        "cycle": (latest_started or latest_stopped or {}).get("cycle"),
        "live": bool((latest_started or {}).get("live")),
        "log_path": (latest_started or latest_stopped or latest_analysis or {}).get("log_path"),
        "started_ts": (latest_started or {}).get("ts"),
        "stopped_ts": (latest_stopped or {}).get("ts"),
        "recommendation": recommendation,
        "realized_pnl": (latest_stopped or {}).get("realized_pnl"),
        "resting_orders": (latest_stopped or {}).get("resting_orders"),
        "stopped_reason": (latest_stopped or {}).get("stopped_reason"),
        "account": {
            "balance_dollars": _float_or_none(account.get("balance_dollars")),
            "portfolio_value": _float_or_none(account.get("portfolio_value")),
        },
        "audit": {
            "errors": audit.get("errors"),
            "live_submissions": audit.get("live_submissions"),
            "live_filled_orders": audit.get("live_filled_orders"),
            "live_preflight_blocks": audit.get("live_preflight_blocks"),
            "resting_order_warnings": audit.get("resting_order_warnings"),
            "final_spent_this_run": audit.get("final_spent_this_run"),
        },
        "skip_analysis": analysis,
        "safe_to_run": _autopilot_safe_to_run(account, audit, latest_stopped),
    }


def _autopilot_safe_to_run(
    account: dict[str, Any],
    audit: dict[str, Any],
    latest_stopped: dict[str, Any] | None,
) -> bool:
    if latest_stopped is None:
        return False
    return (
        (_float_or_none(account.get("portfolio_value")) or 0.0) == 0.0
        and int(audit.get("errors") or 0) == 0
        and int(audit.get("live_preflight_blocks") or 0) == 0
        and int(audit.get("resting_order_warnings") or 0) == 0
        and int(latest_stopped.get("resting_orders") or 0) == 0
    )


def _jsonl_rows(path: Path):
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            if line.strip():
                yield json.loads(line)


def _manual_trading_payload(orders: list[dict[str, Any]], *, cooldown: timedelta) -> dict[str, Any]:
    now = datetime.now(timezone.utc)
    external = [_manual_order_payload(order, now=now) for order in orders if _is_external_executed_order(order)]
    external = [order for order in external if order is not None]
    cutoff = now - cooldown
    recent = [
        order
        for order in external
        if (parsed := _parse_iso_datetime(order.get("created_time"))) is not None and parsed >= cutoff
    ]
    return {
        "checked_at": now.isoformat(),
        "cooldown_hours": cooldown.total_seconds() / 3600.0,
        "external_order_count": len(external),
        "recent_external_order_count": len(recent),
        "lock_active": bool(recent),
        "orders": recent[:10],
    }


def _is_external_executed_order(order: dict[str, Any]) -> bool:
    status = str(order.get("status") or "").lower()
    if status not in {"executed", "filled"}:
        return False
    client_order_id = str(order.get("client_order_id") or "")
    return not client_order_id.startswith("km-")


def _manual_order_payload(order: dict[str, Any], *, now: datetime) -> dict[str, Any] | None:
    created = _parse_iso_datetime(order.get("created_time"))
    age_minutes = ((now - created).total_seconds() / 60.0) if created else None
    side = str(order.get("side") or "").lower()
    count = _float_or_none(order.get("fill_count_fp") or order.get("count_fp") or order.get("count"))
    cost = _float_or_none(order.get("taker_fill_cost_dollars") or order.get("maker_fill_cost_dollars"))
    display_price = (cost / count) if cost is not None and count else None
    if display_price is None:
        display_price = _float_or_none(order.get("yes_price_dollars") if side == "yes" else order.get("no_price_dollars"))
    return {
        "order_id": order.get("order_id"),
        "ticker": order.get("ticker"),
        "side": order.get("side"),
        "action": order.get("action"),
        "type": order.get("type"),
        "status": order.get("status"),
        "client_order_id": order.get("client_order_id") or "",
        "count": count,
        "price": display_price,
        "cost": cost,
        "fee": _float_or_none(order.get("taker_fees_dollars") or order.get("maker_fees_dollars")),
        "created_time": order.get("created_time"),
        "age_minutes": age_minutes,
    }


def _order_payload(order: dict[str, Any]) -> dict[str, Any]:
    return {
        "order_id": order.get("order_id"),
        "ticker": order.get("ticker"),
        "side": order.get("side"),
        "action": order.get("action"),
        "status": order.get("status"),
        "remaining_count_fp": order.get("remaining_count_fp"),
        "yes_price_dollars": order.get("yes_price_dollars"),
        "no_price_dollars": order.get("no_price_dollars"),
    }


DASHBOARD_HTML = r"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Kalshi Microstructure</title>
  <style>
    :root {
      color-scheme: dark;
      --bg: #101214;
      --panel: #181b1f;
      --panel-2: #20242a;
      --text: #eef1f4;
      --muted: #9aa4ae;
      --line: #303741;
      --good: #35c48b;
      --bad: #ff6b6b;
      --warn: #f5c451;
      --info: #67b7ff;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      background: var(--bg);
      color: var(--text);
      font: 14px/1.4 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    }
    header {
      height: 64px;
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 0 20px;
      border-bottom: 1px solid var(--line);
      background: #121519;
    }
    h1 { font-size: 18px; margin: 0; font-weight: 650; letter-spacing: 0; }
    main { padding: 16px; max-width: 1280px; margin: 0 auto; }
    .status { display: flex; gap: 10px; align-items: center; color: var(--muted); }
    .dot { width: 10px; height: 10px; border-radius: 50%; background: var(--warn); }
    .dot.streaming { background: var(--good); }
    .dot.error { background: var(--bad); }
    .grid { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 12px; }
    .wide { grid-column: span 2; }
    .full { grid-column: 1 / -1; }
    section {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 8px;
      min-height: 128px;
      padding: 14px;
    }
    h2 { margin: 0 0 12px; font-size: 13px; color: var(--muted); font-weight: 600; letter-spacing: 0; }
    .value { font-size: 28px; line-height: 1.1; font-weight: 700; letter-spacing: 0; overflow-wrap: anywhere; }
    .sub { margin-top: 6px; color: var(--muted); overflow-wrap: anywhere; }
    .rows { display: grid; gap: 8px; }
    .row { display: flex; justify-content: space-between; gap: 12px; border-bottom: 1px solid #252b33; padding-bottom: 6px; }
    .row:last-child { border-bottom: 0; padding-bottom: 0; }
    .label { color: var(--muted); }
    .num { font-variant-numeric: tabular-nums; text-align: right; }
    .good { color: var(--good); }
    .bad { color: var(--bad); }
    .warn { color: var(--warn); }
    .info { color: var(--info); }
    .pill {
      display: inline-flex;
      align-items: center;
      min-height: 28px;
      padding: 4px 9px;
      border: 1px solid var(--line);
      border-radius: 7px;
      background: var(--panel-2);
      color: var(--muted);
      font-size: 12px;
      font-weight: 650;
    }
    .pill.good { color: var(--good); border-color: rgba(53, 196, 139, 0.45); }
    .pill.bad { color: var(--bad); border-color: rgba(255, 107, 107, 0.45); }
    .pill.warn { color: var(--warn); border-color: rgba(245, 196, 81, 0.45); }
    .metricGrid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 8px; margin-top: 12px; }
    .metric { background: #14181d; border: 1px solid #252b33; border-radius: 7px; padding: 8px; min-height: 58px; }
    .metric .label { display: block; font-size: 12px; margin-bottom: 4px; }
    .metric .num { display: block; text-align: left; font-size: 18px; font-weight: 700; overflow-wrap: anywhere; }
    .hidden { display: none !important; }
    button {
      height: 36px;
      border: 1px solid var(--line);
      border-radius: 7px;
      background: var(--panel-2);
      color: var(--text);
      padding: 0 12px;
      cursor: pointer;
    }
    button:hover { border-color: var(--info); }
    button:disabled { opacity: 0.45; cursor: not-allowed; }
    button.danger { border-color: rgba(255, 107, 107, 0.55); color: var(--bad); }
    .controlRow { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; margin-top: 12px; }
    .arm {
      display: inline-flex;
      align-items: center;
      gap: 7px;
      height: 36px;
      padding: 0 10px;
      border: 1px solid var(--line);
      border-radius: 7px;
      background: #14181d;
      color: var(--muted);
      font-weight: 650;
    }
    .arm input { margin: 0; }
    pre {
      white-space: pre-wrap;
      word-break: break-word;
      margin: 10px 0 0;
      color: var(--muted);
      max-height: 220px;
      overflow: auto;
    }
    canvas {
      display: block;
      width: 100%;
      height: 120px;
      background: #13171b;
      border: 1px solid #252b33;
      border-radius: 6px;
    }
    .chartGrid { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 12px; }
    .chartLabel { display: flex; justify-content: space-between; color: var(--muted); margin-bottom: 8px; }
    .fills {
      width: 100%;
      border-collapse: collapse;
      font-variant-numeric: tabular-nums;
    }
    .fills th, .fills td {
      border-bottom: 1px solid #252b33;
      padding: 7px 4px;
      text-align: right;
      white-space: nowrap;
    }
    .fills th:first-child, .fills td:first-child { text-align: left; }
    .fills th { color: var(--muted); font-weight: 600; }
    .fillsWrap { overflow-x: auto; }
    @media (max-width: 900px) {
      .grid { grid-template-columns: 1fr 1fr; }
      .wide { grid-column: span 2; }
      .chartGrid { grid-template-columns: 1fr; }
    }
    @media (max-width: 560px) {
      header { height: auto; padding: 14px; align-items: flex-start; gap: 8px; flex-direction: column; }
      .grid { grid-template-columns: 1fr; }
      .wide { grid-column: span 1; }
    }
  </style>
</head>
<body>
  <header>
    <h1>Kalshi Microstructure</h1>
    <div class="status"><span id="dot" class="dot"></span><span id="status">starting</span><span id="updated"></span></div>
  </header>
  <main class="grid">
    <section class="wide">
      <h2>Market</h2>
      <div id="ticker" class="value">--</div>
      <div id="marketSub" class="sub">Waiting for market stream</div>
    </section>
    <section>
      <h2>BTC Spot</h2>
      <div id="spot" class="value">--</div>
      <div id="spotSub" class="sub">--</div>
    </section>
    <section>
      <h2>CF Settlement</h2>
      <div id="cfValue" class="value">--</div>
      <div id="cfSub" class="sub">Waiting for CF feed</div>
    </section>
    <section>
      <h2>Volatility</h2>
      <div id="vol" class="value">--</div>
      <div id="volSub" class="sub">--</div>
    </section>
    <section>
      <h2>Kalshi Account</h2>
      <div id="accountBalance" class="value">--</div>
      <div id="accountSub" class="sub">Authenticated balance</div>
    </section>
    <section class="wide">
      <h2>Settlement Math</h2>
      <div class="rows">
        <div class="row"><span class="label">Observed average</span><span id="cfObserved" class="num">--</span></div>
        <div class="row"><span class="label">Projected average</span><span id="cfProjected" class="num">--</span></div>
        <div class="row"><span class="label">Required for YES</span><span id="cfRequiredYes" class="num">--</span></div>
        <div class="row"><span class="label">Window progress</span><span id="cfProgress" class="num">--</span></div>
        <div class="row"><span class="label">CF decision</span><span id="cfDecision" class="num">--</span></div>
        <div class="row"><span class="label">Executable ask</span><span id="cfAsk" class="num">--</span></div>
      </div>
    </section>
    <section class="wide">
      <h2>Top Of Book</h2>
      <div class="rows">
        <div class="row"><span class="label">YES bid / ask</span><span id="yesBook" class="num">--</span></div>
        <div class="row"><span class="label">NO bid / ask</span><span id="noBook" class="num">--</span></div>
        <div class="row"><span class="label">YES spread</span><span id="spread" class="num">--</span></div>
        <div class="row"><span class="label">Sequence</span><span id="seq" class="num">--</span></div>
      </div>
    </section>
    <section class="wide">
      <h2>Latest Signal</h2>
      <div id="signalValue" class="value">None</div>
      <div id="signalSub" class="sub">No net edge over threshold</div>
    </section>
    <section class="wide">
      <h2>Live Bot Audit</h2>
      <div id="liveFillCost" class="value">--</div>
      <div id="liveAuditSub" class="sub">Waiting for bot log</div>
    </section>
    <section class="wide">
      <h2>Manual Trade Guard</h2>
      <div id="manualGuard" class="value">--</div>
      <div id="manualSub" class="sub">Waiting for recent orders</div>
      <div id="manualRows" class="rows"><div class="row"><span class="label">Waiting</span><span class="num">--</span></div></div>
    </section>
    <section class="wide">
      <h2>Autopilot Brain</h2>
      <div id="autoRec" class="value">--</div>
      <div id="autoSub" class="sub">Waiting for supervisor log</div>
      <div class="metricGrid">
        <div class="metric"><span class="label">Safe</span><span id="autoSafe" class="num">--</span></div>
        <div class="metric"><span class="label">Actual P/L</span><span id="autoActual" class="num">--</span></div>
        <div class="metric"><span class="label">Skipped P/L</span><span id="autoSkipped" class="num">--</span></div>
        <div class="metric"><span class="label">Skipped W/L</span><span id="autoWL" class="num">--</span></div>
        <div class="metric"><span class="label">Shadow P/L</span><span id="autoShadow" class="num">--</span></div>
        <div class="metric"><span class="label">Shadow W/L</span><span id="autoShadowWL" class="num">--</span></div>
      </div>
    </section>
    <section class="wide">
      <h2>Control Tower</h2>
      <div id="autoMode" class="value">OFF</div>
      <div id="autoControlSub" class="sub">Waiting for control state</div>
      <div class="controlRow">
        <label class="arm"><input id="autoArm" type="checkbox" /> ARM LIVE</label>
        <button id="autoStart">Start Guarded Cycle</button>
        <button id="autoStop" class="danger">Emergency Stop</button>
      </div>
      <pre id="autoControlOut">No control action yet.</pre>
    </section>
    <section class="wide">
      <h2>Skipped By Asset</h2>
      <div id="autoAssets" class="rows"><div class="row"><span class="label">Waiting</span><span class="num">--</span></div></div>
    </section>
    <section class="wide">
      <h2>Focused Shadow</h2>
      <div id="autoShadowRows" class="rows"><div class="row"><span class="label">Waiting</span><span class="num">--</span></div></div>
    </section>
    <section class="full">
      <h2>Crypto Watchlist</h2>
      <div class="fillsWrap">
        <table class="fills">
          <thead>
            <tr><th>Asset</th><th>Market</th><th>Spot</th><th>YES</th><th>NO</th><th>Signal</th><th>Net Edge</th><th>Status</th></tr>
          </thead>
          <tbody id="watchlistBody"><tr><td colspan="8">Loading watchlist.</td></tr></tbody>
        </table>
      </div>
    </section>
    <section id="paperSection" class="wide">
      <h2>Paper Execution</h2>
      <div id="paperEquity" class="value">--</div>
      <div id="paperSub" class="sub">No simulated fills yet</div>
    </section>
    <section class="full">
      <h2>History</h2>
      <div class="chartGrid">
        <div>
          <div class="chartLabel"><span>BTC spot</span><span id="spotChartLabel">--</span></div>
          <canvas id="spotChart" width="360" height="120"></canvas>
        </div>
        <div>
          <div class="chartLabel"><span>Volatility</span><span id="volChartLabel">--</span></div>
          <canvas id="volChart" width="360" height="120"></canvas>
        </div>
        <div>
          <div class="chartLabel"><span>Net edge</span><span id="edgeChartLabel">--</span></div>
          <canvas id="edgeChart" width="360" height="120"></canvas>
        </div>
        <div id="equityChartBox">
          <div class="chartLabel"><span>Paper equity</span><span id="equityChartLabel">--</span></div>
          <canvas id="equityChart" width="360" height="120"></canvas>
        </div>
      </div>
    </section>
    <section id="fillsSection" class="full">
      <h2>Paper Fills</h2>
      <div class="fillsWrap">
        <table class="fills">
          <thead>
            <tr><th>Time</th><th>Action</th><th>Side</th><th>Shares</th><th>Price/Payout</th><th>Cost/PnL</th><th>Fee/Result</th><th>Edge / Fill</th></tr>
          </thead>
          <tbody id="fillsBody"><tr><td colspan="8">No fills yet.</td></tr></tbody>
        </table>
      </div>
    </section>
    <section class="wide">
      <h2>Risk Settings</h2>
      <div class="rows">
        <div class="row"><span class="label">Min net edge</span><span id="riskEdge" class="num">--</span></div>
        <div class="row"><span class="label">Fee rate</span><span id="riskFee" class="num">--</span></div>
        <div class="row"><span class="label">Slippage</span><span id="riskSlip" class="num">--</span></div>
        <div class="row"><span class="label">Live log</span><span id="riskLiveLog" class="num">--</span></div>
        <div class="row"><span class="label">Live cancel</span><span id="riskCancel" class="num">--</span></div>
      </div>
    </section>
    <section class="full">
      <h2>Kill Switch</h2>
      <button id="checkOrders">Check Resting Orders</button>
      <button id="cancelOrders">Cancel Resting Orders</button>
      <pre id="ordersOut">No check yet.</pre>
    </section>
  </main>
  <script>
    let lastTicker = null;
    const $ = (id) => document.getElementById(id);
    const pct = (v) => v == null ? "--" : (v * 100).toFixed(2) + "%";
    const price = (v) => v == null ? "--" : Number(v).toFixed(4);
    const money = (v) => v == null ? "--" : Number(v).toLocaleString(undefined, { maximumFractionDigits: 2 });
    const usd = (v) => v == null ? "--" : Number(v).toLocaleString(undefined, { style: "currency", currency: "USD", minimumFractionDigits: 2, maximumFractionDigits: 2 });
    const level = (l) => l ? `${price(l.price)} x ${money(l.size)}` : "--";
    const colors = { spot: "#67b7ff", vol: "#f5c451", edge: "#35c48b", equity: "#d88cff", muted: "#303741", text: "#9aa4ae" };

    function drawChart(canvas, points, color, formatter) {
      const ctx = canvas.getContext("2d");
      const width = canvas.width;
      const height = canvas.height;
      ctx.clearRect(0, 0, width, height);
      ctx.strokeStyle = colors.muted;
      ctx.lineWidth = 1;
      for (let i = 1; i < 4; i++) {
        const y = (height / 4) * i;
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(width, y);
        ctx.stroke();
      }
      const values = (points || []).map(p => p.value).filter(v => v !== null && v !== undefined && Number.isFinite(Number(v))).map(Number);
      if (values.length < 2) return "--";
      let min = Math.min(...values);
      let max = Math.max(...values);
      if (min === max) {
        min -= Math.abs(min || 1) * 0.01;
        max += Math.abs(max || 1) * 0.01;
      }
      const usable = (points || []).filter(p => p.value !== null && p.value !== undefined && Number.isFinite(Number(p.value)));
      ctx.strokeStyle = color;
      ctx.lineWidth = 2;
      ctx.beginPath();
      usable.forEach((point, index) => {
        const x = usable.length === 1 ? width : (index / (usable.length - 1)) * width;
        const y = height - ((Number(point.value) - min) / (max - min)) * (height - 14) - 7;
        if (index === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      });
      ctx.stroke();
      return `${formatter(values[values.length - 1])} | ${formatter(min)}-${formatter(max)}`;
    }

    function drawSignalChart(canvas, points) {
      const mapped = (points || []).map(p => ({ ...p, value: p.value == null ? 0 : p.value }));
      return drawChart(canvas, mapped, colors.edge, pct);
    }

    function renderFills(fills) {
      if (!fills || fills.length === 0) {
        $("fillsBody").innerHTML = `<tr><td colspan="8">No fills yet.</td></tr>`;
        return;
      }
      $("fillsBody").innerHTML = fills.slice().reverse().map(fill => `
        <tr>
          <td>${new Date(fill.ts).toLocaleTimeString()}</td>
          <td>${fill.action}</td>
          <td class="${fill.side === "YES" ? "good" : "info"}">${fill.side}</td>
          <td>${money(fill.shares)}</td>
          <td>${fill.action === "SETTLE" ? usd(fill.payout) : price(fill.price)}</td>
          <td>${fill.action === "SETTLE" ? usd(fill.realized_pnl) : usd(fill.cost)}</td>
          <td>${fill.action === "SETTLE" ? fill.result_side : usd(fill.fee)}</td>
          <td>${fill.action === "SETTLE" ? "--" : `${pct(fill.net_edge)} / ${pct(fill.fill_ratio)}`}</td>
        </tr>
      `).join("");
    }

    function renderLiveAudit(live) {
      if (!live || live.error) {
        $("liveFillCost").textContent = "--";
        $("liveFillCost").className = "value";
        $("liveAuditSub").textContent = live?.error || "Waiting for bot log";
        return;
      }
      $("liveFillCost").textContent = usd(live.live_fill_cost);
      $("liveFillCost").className = live.live_fill_cost > 0 ? "value good" : "value";
      $("liveAuditSub").textContent =
        `submitted ${live.live_submissions} | filled ${live.live_filled_orders} | blocked ${live.live_preflight_blocks} | fees ${usd(live.live_fees)}`;
    }

    function renderManualTrading(manual) {
      if (!manual || manual.error) {
        $("manualGuard").textContent = "--";
        $("manualGuard").className = "value";
        $("manualSub").textContent = manual?.error || "Waiting for recent orders";
        $("manualRows").innerHTML = `<div class="row"><span class="label">Waiting</span><span class="num">--</span></div>`;
        return;
      }
      const locked = !!manual.lock_active;
      $("manualGuard").textContent = locked ? "LOCKED" : "CLEAR";
      $("manualGuard").className = `value ${locked ? "bad" : "good"}`;
      $("manualSub").textContent =
        `${manual.recent_external_order_count || 0} recent external orders | cooldown ${manual.cooldown_hours || 0}h`;
      const orders = manual.orders || [];
      $("manualRows").innerHTML = orders.length ? orders.map(order => {
        const age = order.age_minutes == null ? "--" : `${Number(order.age_minutes).toFixed(0)}m`;
        return `<div class="row"><span class="label">${order.ticker || "--"} ${String(order.side || "").toUpperCase()} <span class="pill">${age}</span></span><span class="num bad">${order.type || "external"}</span></div>`;
      }).join("") : `<div class="row"><span class="label">No recent external orders</span><span class="num good">clear</span></div>`;
    }

    function renderCF(cf) {
      const state = cf?.settlement_state || {};
      const tick = cf?.tick || {};
      if (!cf || cf.error) {
        $("cfValue").textContent = "--";
        $("cfValue").className = "value";
        $("cfSub").textContent = cf?.error || "Waiting for CF feed";
        $("cfObserved").textContent = "--";
        $("cfProjected").textContent = "--";
        $("cfRequiredYes").textContent = "--";
        $("cfProgress").textContent = "--";
        $("cfDecision").textContent = "--";
        $("cfDecision").className = "num";
        $("cfAsk").textContent = "--";
        $("cfAsk").className = "num";
        return;
      }
      const locked = state.locked_side;
      const liquidity = cf.liquidity || {};
      $("cfValue").textContent = money(tick.value);
      $("cfValue").className = `value ${locked === "YES" ? "good" : locked === "NO" ? "bad" : "info"}`;
      $("cfSub").textContent =
        `${cf.index || "--"} | target ${money(state.threshold)} | latest ${tick.ts ? new Date(tick.ts).toLocaleTimeString() : "--"}`;
      $("cfObserved").textContent = money(state.observed_average);
      $("cfProjected").textContent = money(state.projected_average);
      $("cfProjected").className = `num ${Number(state.projected_average || 0) > Number(state.threshold || 0) ? "good" : "bad"}`;
      $("cfRequiredYes").textContent = money(state.required_remaining_average_for_yes);
      $("cfProgress").textContent = `${state.observed_count || 0}/${state.target_count || 60} | ${Math.max(0, Number(state.seconds_remaining || 0)).toFixed(0)}s`;
      if (liquidity.executable) {
        $("cfDecision").textContent = `${liquidity.decision_side} executable`;
        $("cfDecision").className = `num ${liquidity.decision_side === "YES" ? "good" : "bad"}`;
      } else if (liquidity.decision_side) {
        $("cfDecision").textContent = `${liquidity.decision_side} blocked: ${liquidity.reject_reason || "--"}`;
        $("cfDecision").className = "num info";
      } else {
        $("cfDecision").textContent = liquidity.reject_reason || "--";
        $("cfDecision").className = "num";
      }
      $("cfAsk").textContent = liquidity.ask ? `${price(liquidity.ask.price)} x ${money(liquidity.ask.size)}` : "--";
      $("cfAsk").className = `num ${liquidity.executable ? "good" : ""}`;
    }

    function renderAutopilot(auto) {
      if (!auto || auto.error) {
        $("autoRec").textContent = "--";
        $("autoRec").className = "value";
        $("autoSub").textContent = auto?.error || "Waiting for supervisor log";
        $("autoSafe").textContent = "--";
        $("autoSafe").className = "num";
        $("autoActual").textContent = "--";
        $("autoSkipped").textContent = "--";
        $("autoWL").textContent = "--";
        $("autoShadow").textContent = "--";
        $("autoShadowWL").textContent = "--";
        $("autoAssets").innerHTML = `<div class="row"><span class="label">Waiting</span><span class="num">--</span></div>`;
        $("autoShadowRows").innerHTML = `<div class="row"><span class="label">Waiting</span><span class="num">--</span></div>`;
        return;
      }
      const analysis = auto.skip_analysis || {};
      const shadow = analysis.focused_shadow || {};
      const skippedPnl = Number(analysis.hypothetical_pnl || 0);
      const shadowPnl = Number(shadow.hypothetical_pnl || 0);
      const actualPnl = Number(auto.realized_pnl || 0);
      const rec = auto.recommendation || "collect_more_data";
      $("autoRec").textContent = rec.replaceAll("_", " ");
      $("autoRec").className = `value ${rec.includes("keep_gates") ? "warn" : "info"}`;
      $("autoSub").textContent = `${auto.status || "--"} | ${auto.live ? "live" : "paper"} | ${auto.log_path || "--"}`;
      $("autoSafe").textContent = auto.safe_to_run ? "YES" : "NO";
      $("autoSafe").className = `num ${auto.safe_to_run ? "good" : "bad"}`;
      $("autoActual").textContent = usd(actualPnl);
      $("autoActual").className = `num ${actualPnl >= 0 ? "good" : "bad"}`;
      $("autoSkipped").textContent = usd(skippedPnl);
      $("autoSkipped").className = `num ${skippedPnl >= 0 ? "good" : "bad"}`;
      $("autoWL").textContent = `${analysis.hypothetical_wins || 0} / ${analysis.hypothetical_losses || 0}`;
      $("autoShadow").textContent = usd(shadowPnl);
      $("autoShadow").className = `num ${shadowPnl >= 0 ? "good" : "bad"}`;
      $("autoShadowWL").textContent = `${shadow.wins || 0} / ${shadow.losses || 0}`;
      const byAsset = analysis.by_asset || {};
      const names = Object.keys(byAsset).sort();
      $("autoAssets").innerHTML = names.length ? names.map(name => {
        const row = byAsset[name] || {};
        const pnl = Number(row.hypothetical_pnl || 0);
        return `<div class="row"><span class="label">${name} <span class="pill">${row.wins || 0}/${row.losses || 0}</span></span><span class="num ${pnl >= 0 ? "good" : "bad"}">${usd(pnl)}</span></div>`;
      }).join("") : `<div class="row"><span class="label">No settled skipped signals</span><span class="num">--</span></div>`;
      const bySide = shadow.by_side || {};
      const sideNames = Object.keys(bySide).sort();
      const promotion = shadow.promotion || {};
      const shadowTitle = `${shadow.asset || "--"} ${shadow.side || "ANY"} <= ${price(shadow.max_entry_price)} | ${Math.round(Number(shadow.max_seconds_to_close || 0))}s`;
      const sideRows = sideNames.map(name => {
        const row = bySide[name] || {};
        const pnl = Number(row.hypothetical_pnl || 0);
        return `<div class="row"><span class="label">${name} <span class="pill">${row.wins || 0}/${row.losses || 0}</span></span><span class="num ${pnl >= 0 ? "good" : "bad"}">${usd(pnl)}</span></div>`;
      }).join("");
      $("autoShadowRows").innerHTML = `
        <div class="row"><span class="label">${shadowTitle}</span><span class="num">${shadow.settled || 0}/${shadow.opportunities || 0}</span></div>
        <div class="row"><span class="label">promotion</span><span class="num ${promotion.status === "candidate_passed_shadow" ? "good" : "warn"}">${(promotion.status || "collect_more_data").replaceAll("_", " ")}</span></div>
        ${sideRows || `<div class="row"><span class="label">No matching settled signals</span><span class="num">--</span></div>`}
      `;
    }

    function renderAutopilotControl(control, auto) {
      const mode = control?.mode || "OFF";
      const running = mode === "RUNNING";
      const safe = !!auto?.safe_to_run;
      const armed = $("autoArm").checked;
      $("autoMode").textContent = mode;
      $("autoMode").className = `value ${running ? "warn" : "info"}`;
      const risk = control?.risk || {};
      const riskText = risk.daily_stop_loss_blocked
        ? `stop-loss ${usd(risk.recent_realized_pnl)} / ${usd(-Math.abs(Number(risk.daily_stop_loss || 0)))}`
        : `24h P/L ${usd(risk.recent_realized_pnl)} | limit ${usd(risk.daily_stop_loss)}`;
      $("autoControlSub").textContent = running
        ? `pid ${control?.pid || "--"} | started ${control?.started_at ? new Date(control.started_at).toLocaleTimeString() : "--"}`
        : `safe ${safe ? "YES" : "NO"} | ${control?.last_action?.reason || control?.last_action?.action || "idle"} | ${riskText}`;
      $("autoStart").disabled = running || !safe || !armed;
      $("autoStop").disabled = !running;
    }

    function renderWatchlist(rows) {
      if (!rows || rows.length === 0) {
        $("watchlistBody").innerHTML = `<tr><td colspan="8">No assets configured.</td></tr>`;
        return;
      }
      $("watchlistBody").innerHTML = rows.map(row => {
        const signal = row.signal;
        const signalText = signal ? `${signal.side} @ ${price(signal.ask_price)}` : "--";
        const edgeClass = signal?.net_edge > 0 ? "good" : "";
        return `
          <tr>
            <td>${row.symbol || row.series || "--"}</td>
            <td>${row.market?.ticker || "--"}</td>
            <td>${row.spot?.price ? money(row.spot.price) : "--"}</td>
            <td>${level(row.book?.yes_bid)} / ${level(row.book?.yes_ask)}</td>
            <td>${level(row.book?.no_bid)} / ${level(row.book?.no_ask)}</td>
            <td class="${signal ? "good" : ""}">${signalText}</td>
            <td class="${edgeClass}">${pct(signal?.net_edge)}</td>
            <td>${row.error || row.status || "--"}</td>
          </tr>
        `;
      }).join("");
    }

    async function refresh() {
      const state = await fetch("/api/state").then(r => r.json());
      lastTicker = state.market?.ticker || lastTicker;
      $("status").textContent = state.error ? `${state.status}: ${state.error}` : state.status;
      $("dot").className = `dot ${state.status || ""}`;
      $("updated").textContent = state.updated_at ? new Date(state.updated_at).toLocaleTimeString() : "";
      $("ticker").textContent = state.market?.ticker || "--";
      $("marketSub").textContent = state.market ? `${state.market.title} | target ${money(state.market.threshold)} | close ${state.market.close_time || "--"}` : "Waiting for market stream";
      $("spot").textContent = state.spot?.price ? money(state.spot.price) : "--";
      $("spotSub").textContent = state.spot ? `age ${state.spot.age_seconds?.toFixed(2) ?? "--"}s | ticks ${state.spot.tick_count ?? 0}` : "--";
      renderCF(state.cf);
      $("vol").textContent = pct(state.spot?.annual_volatility);
      $("volSub").textContent = state.risk?.dynamic_volatility ? "dynamic realized" : "fixed fallback";
      $("accountBalance").textContent = state.account?.error ? "Error" : usd(state.account?.balance_dollars);
      $("accountBalance").className = state.account?.error ? "value bad" : "value";
      $("accountSub").textContent = state.account?.error
        ? state.account.error
        : `updated ${state.account?.updated_ts ? new Date(state.account.updated_ts * 1000).toLocaleTimeString() : "--"}`;
      $("yesBook").textContent = `${level(state.book?.yes_bid)} / ${level(state.book?.yes_ask)}`;
      $("noBook").textContent = `${level(state.book?.no_bid)} / ${level(state.book?.no_ask)}`;
      $("spread").textContent = price(state.book?.yes_spread);
      $("seq").textContent = state.book?.seq ?? "--";
      if (state.signal) {
        $("signalValue").textContent = `${state.signal.side} @ ${price(state.signal.ask_price)}`;
        $("signalValue").className = "value good";
        $("signalSub").textContent = `fair ${price(state.signal.fair_price)} | net edge ${pct(state.signal.net_edge)} | EV ${usd(state.signal.net_expected_value)}`;
      } else {
        $("signalValue").textContent = "None";
        $("signalValue").className = "value";
        $("signalSub").textContent = "No net edge over threshold";
      }
      renderLiveAudit(state.live_audit);
      renderManualTrading(state.manual_trading);
      renderAutopilot(state.autopilot);
      renderAutopilotControl(state.autopilot_control, state.autopilot);
      renderWatchlist(state.watchlist);
      const paperEnabled = !!state.paper?.enabled;
      $("paperSection").className = paperEnabled ? "wide" : "wide hidden";
      $("fillsSection").className = paperEnabled ? "full" : "full hidden";
      $("equityChartBox").className = paperEnabled ? "" : "hidden";
      const pnl = state.paper?.unrealized_pnl;
      $("paperEquity").textContent = usd(state.paper?.equity);
      $("paperEquity").className = pnl == null ? "value" : `value ${pnl >= 0 ? "good" : "bad"}`;
      $("paperSub").textContent = state.paper
        ? `cash ${usd(state.paper.cash)} | open ${state.paper.open_positions ?? 0} | unrealized ${usd(pnl)} | realized ${usd(state.paper.realized_pnl)}`
        : "Paper simulation unavailable";
      renderFills(state.paper?.fills);
      $("riskEdge").textContent = pct(state.risk?.min_edge);
      $("riskFee").textContent = pct(state.risk?.fee_rate);
      $("riskSlip").textContent = price(state.risk?.slippage_per_contract);
      $("riskLiveLog").textContent = state.risk?.live_log_glob || "--";
      $("riskCancel").textContent = state.risk?.allow_live_cancel ? "enabled" : "dry-run";
      $("riskCancel").className = state.risk?.allow_live_cancel ? "num bad" : "num warn";
      $("spotChartLabel").textContent = drawChart($("spotChart"), state.history?.spot, colors.spot, money);
      $("volChartLabel").textContent = drawChart($("volChart"), state.history?.volatility, colors.vol, pct);
      $("edgeChartLabel").textContent = drawSignalChart($("edgeChart"), state.history?.signal);
      $("equityChartLabel").textContent = drawChart($("equityChart"), state.history?.equity, colors.equity, money);
    }

    async function openOrders() {
      if (!lastTicker) return;
      const data = await fetch(`/api/open-orders?ticker=${encodeURIComponent(lastTicker)}`).then(r => r.json());
      $("ordersOut").textContent = JSON.stringify(data, null, 2);
    }

    async function cancelOrders() {
      if (!lastTicker) return;
      const data = await fetch("/api/cancel-resting", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ ticker: lastTicker })
      }).then(r => r.json());
      $("ordersOut").textContent = JSON.stringify(data, null, 2);
    }

    async function startAutopilot() {
      const data = await fetch("/api/autopilot/start", { method: "POST" }).then(r => r.json());
      $("autoControlOut").textContent = JSON.stringify(data, null, 2);
      await refresh();
    }

    async function stopAutopilot() {
      const data = await fetch("/api/autopilot/stop", { method: "POST" }).then(r => r.json());
      $("autoControlOut").textContent = JSON.stringify(data, null, 2);
      await refresh();
    }

    $("checkOrders").addEventListener("click", openOrders);
    $("cancelOrders").addEventListener("click", cancelOrders);
    $("autoArm").addEventListener("change", refresh);
    $("autoStart").addEventListener("click", startAutopilot);
    $("autoStop").addEventListener("click", stopAutopilot);
    refresh();
    setInterval(refresh, 1000);
  </script>
</body>
</html>
"""
