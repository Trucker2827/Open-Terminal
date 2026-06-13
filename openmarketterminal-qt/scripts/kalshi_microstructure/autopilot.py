from __future__ import annotations

import asyncio
import json
import time
from dataclasses import asdict, dataclass, fields, replace
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Awaitable, Callable

from .bot import BotConfig, BotState, JsonlLogger
from .kalshi import KalshiRestClient
from .log_reconcile import BotLogAudit, audit_bot_log, audit_payload
from .market_hours import kalshi_maintenance_window
from .portfolio_bot import PortfolioAsset, run_portfolio_bot
from .report import StrategyGateConfig, StrategyReportConfig, build_strategy_gate, gate_payload
from .settlement import MarketSettlement, payout_price, settlement_from_market
from .skip_analysis import analyze_skipped_signals, focused_shadow_payload, skip_analysis_payload


DEFAULT_AUTOPILOT_WATCHLIST = (
    "KXBTC15M:BTC-USD",
    "KXETH15M:ETH-USD",
    "KXSOL15M:SOL-USD",
    "KXDOGE15M:DOGE-USD",
)


@dataclass(frozen=True)
class LiveFill:
    ticker: str
    side: str
    shares: float
    cost: float
    fees: float
    ts: str | None = None


@dataclass(frozen=True)
class AutopilotConfig:
    watchlist: tuple[str, ...] = DEFAULT_AUTOPILOT_WATCHLIST
    run_seconds: float = 1200.0
    poll_seconds: float = 4.0
    idle_seconds: float = 5.0
    max_cycles: int = 1
    min_edge: float = 0.06
    max_order_cost: float = 2.0
    max_run_cost: float = 6.0
    max_market_cost: float | None = 2.25
    max_entry_price: float | None = None
    max_trades_per_market: int = 3
    cooldown_seconds: float = 20.0
    max_entry_seconds_to_close: float | None = 240.0
    max_spot_age_seconds: float = 1.5
    settlement_average_seconds: float = 60.0
    min_settlement_proxy_coverage_seconds: float = 20.0
    require_settlement_proxy: bool = True
    fee_rate: float = 0.07
    slippage_per_contract: float = 0.0015
    max_live_order_total: float | None = 2.35
    live_focus_symbol: str | None = None
    live_focus_side: str | None = None
    live_focus_allow_spot_side_mismatch: bool = False
    live: bool = False
    log_dir: Path = Path("logs")
    supervisor_log_path: Path = Path("logs/autopilot-supervisor.jsonl")
    daily_stop_loss: float = 5.0
    max_consecutive_losing_runs: int = 2
    stop_on_errors: bool = True
    stop_on_resting_orders: bool = True
    no_new_run_with_portfolio_value: bool = True
    settlement_wait_seconds: float = 240.0
    settlement_poll_seconds: float = 10.0
    analyze_skips: bool = True
    skip_analysis_dedupe_seconds: float = 15.0
    skip_analysis_top: int = 5
    shadow_focus_asset: str | None = "ETH-USD"
    shadow_focus_side: str | None = "NO"
    shadow_max_entry_price: float | None = 0.50
    shadow_max_seconds_to_close: float | None = 180.0
    shadow_min_net_edge: float | None = 0.06
    shadow_min_settled_for_promotion: int = 10
    shadow_min_win_rate_for_promotion: float = 0.65
    shadow_min_pnl_for_promotion: float = 0.0
    require_strategy_gate: bool = True
    strategy_gate_path: Path | None = Path("logs/series-research/20260530")
    strategy_gate_edges: tuple[float, ...] = (0.08,)
    strategy_gate_max_files: int = 8
    strategy_gate_min_trades: int = 5
    strategy_gate_min_win_rate: float = 0.55
    strategy_gate_min_pnl: float = 0.0
    strategy_gate_max_open_positions: int = 0
    strategy_gate_mode: str = "strict"
    strategy_gate_probe_reasons: tuple[str, ...] = ("not_enough_trades",)
    strategy_gate_probe_max_order_cost: float = 1.0
    strategy_gate_probe_max_run_cost: float = 2.0
    strategy_gate_probe_daily_stop_loss: float = 5.0


@dataclass(frozen=True)
class AutopilotCycleResult:
    cycle: int
    log_path: Path
    audit: BotLogAudit
    fills: tuple[LiveFill, ...]
    settlements: dict[str, MarketSettlement]
    realized_pnl: float
    resting_orders: int
    skip_analysis: dict[str, Any] | None = None
    recommendation: str | None = None
    stopped_reason: str | None = None


PortfolioRunner = Callable[[KalshiRestClient, BotConfig, tuple[PortfolioAsset, ...]], Awaitable[BotState]]


async def run_autopilot(
    client: KalshiRestClient,
    config: AutopilotConfig,
    *,
    runner: PortfolioRunner = run_portfolio_bot,
    sleep: Callable[[float], Awaitable[None]] = asyncio.sleep,
) -> list[AutopilotCycleResult]:
    logger = JsonlLogger(config.supervisor_log_path)
    logger.write("autopilot_started", autopilot_config_payload(config))
    assets = portfolio_assets(config.watchlist)
    daily_pnl = 0.0
    consecutive_losing_runs = 0
    results: list[AutopilotCycleResult] = []

    for cycle in range(1, max(1, config.max_cycles) + 1):
        maintenance = kalshi_maintenance_window()
        if maintenance.active:
            logger.write("autopilot_stopped", {"reason": "scheduled_maintenance", "window": maintenance.label})
            break

        account = _safe_balance(client, config.live)
        portfolio_value = balance_portfolio_value(account)
        if config.live and config.no_new_run_with_portfolio_value and portfolio_value > 0:
            logger.write(
                "autopilot_stopped",
                {"reason": "portfolio_value_active", "portfolio_value": portfolio_value, "account": account},
            )
            break
        if config.live and config.require_strategy_gate:
            gate = live_strategy_gate_payload(config)
            if not gate.get("passed"):
                if strategy_gate_allows_probe(config, gate):
                    logger.write(
                        "strategy_gate_probe_allowed",
                        {"strategy_gate": gate, "account": account},
                    )
                    cycle_config = probe_cycle_config(config)
                else:
                    logger.write(
                        "autopilot_stopped",
                        {"reason": "strategy_gate_blocked", "strategy_gate": gate, "account": account},
                    )
                    break
            else:
                cycle_config = config
        else:
            cycle_config = config

        log_path = next_cycle_log_path(cycle_config.log_dir, cycle)
        bot_config = bot_config_for_cycle(cycle_config, log_path)
        logger.write(
            "autopilot_cycle_started",
            {"cycle": cycle, "log_path": str(log_path), "live": bot_config.live, "account": account},
        )
        await runner(client, bot_config, assets)

        audit = audit_bot_log(log_path)
        fills = live_fills_from_log(log_path)
        resting_orders = resting_order_count(client) if cycle_config.live else 0
        settlements = await wait_for_settlements(client, fills, cycle_config, sleep=sleep) if cycle_config.live else {}
        skip_analysis = cycle_skip_analysis(client, log_path, audit, cycle_config)
        recommendation = skip_recommendation(skip_analysis)
        realized_pnl = realized_live_pnl(fills, settlements)
        daily_pnl += realized_pnl
        consecutive_losing_runs = consecutive_losing_runs + 1 if realized_pnl < 0 else 0

        stopped_reason = stop_reason(
            audit=audit,
            fills=fills,
            settlements=settlements,
            resting_orders=resting_orders,
            realized_pnl=realized_pnl,
            daily_pnl=daily_pnl,
            consecutive_losing_runs=consecutive_losing_runs,
            config=cycle_config,
        )
        result = AutopilotCycleResult(
            cycle=cycle,
            log_path=log_path,
            audit=audit,
            fills=fills,
            settlements=settlements,
            realized_pnl=realized_pnl,
            resting_orders=resting_orders,
            skip_analysis=skip_analysis,
            recommendation=recommendation,
            stopped_reason=stopped_reason,
        )
        results.append(result)
        if skip_analysis is not None:
            logger.write(
                "autopilot_skip_analysis",
                {
                    "cycle": cycle,
                    "log_path": str(log_path),
                    "recommendation": recommendation,
                    "analysis": skip_analysis,
                },
            )
        logger.write("autopilot_cycle_stopped", autopilot_cycle_payload(result, daily_pnl))

        if stopped_reason is not None:
            logger.write("autopilot_stopped", {"reason": stopped_reason, "daily_pnl": daily_pnl})
            break
        if cycle < config.max_cycles:
            await sleep(config.idle_seconds)

    logger.write(
        "autopilot_finished",
        {"cycles": len(results), "daily_pnl": daily_pnl, "live": config.live},
    )
    return results


def load_autopilot_config(path: Path | None) -> AutopilotConfig:
    if path is None:
        return AutopilotConfig()
    with path.open("r", encoding="utf-8") as handle:
        raw = json.load(handle)
    if not isinstance(raw, dict):
        raise ValueError("Autopilot config must be a JSON object.")
    field_names = {field.name for field in fields(AutopilotConfig)}
    values = {key: value for key, value in raw.items() if key in field_names}
    if "watchlist" in values:
        values["watchlist"] = _watchlist_tuple(values["watchlist"])
    if "strategy_gate_edges" in values:
        values["strategy_gate_edges"] = _float_tuple(values["strategy_gate_edges"])
    if "strategy_gate_probe_reasons" in values:
        values["strategy_gate_probe_reasons"] = _string_tuple(values["strategy_gate_probe_reasons"])
    for path_key in ("log_dir", "supervisor_log_path", "strategy_gate_path"):
        if path_key in values:
            values[path_key] = Path(values[path_key]) if values[path_key] is not None else None
    return AutopilotConfig(**values)


def bot_config_for_cycle(config: AutopilotConfig, log_path: Path) -> BotConfig:
    return BotConfig(
        poll_seconds=config.poll_seconds,
        run_seconds=config.run_seconds,
        dynamic_volatility=True,
        vol_lookback_seconds=30.0,
        min_edge=config.min_edge,
        max_order_cost=config.max_order_cost,
        max_run_cost=config.max_run_cost,
        max_market_cost=config.max_market_cost,
        max_entry_price=config.max_entry_price,
        max_trades_per_market=config.max_trades_per_market,
        cooldown_seconds=config.cooldown_seconds,
        max_entry_seconds_to_close=config.max_entry_seconds_to_close,
        max_spot_age_seconds=config.max_spot_age_seconds,
        settlement_average_seconds=config.settlement_average_seconds,
        min_settlement_proxy_coverage_seconds=config.min_settlement_proxy_coverage_seconds,
        require_settlement_proxy=config.require_settlement_proxy,
        fee_rate=config.fee_rate,
        slippage_per_contract=config.slippage_per_contract,
        log_path=log_path,
        live=config.live,
        require_live_preflight=True,
        allow_live_preflight_warnings=False,
        max_live_order_total=config.max_live_order_total,
        allow_live_requote=True,
        allow_live_opposite_side=False,
        live_focus_symbol=config.live_focus_symbol,
        live_focus_side=config.live_focus_side,
        live_focus_allow_spot_side_mismatch=config.live_focus_allow_spot_side_mismatch,
    )


def portfolio_assets(watchlist: tuple[str, ...]) -> tuple[PortfolioAsset, ...]:
    assets: list[PortfolioAsset] = []
    for item in watchlist:
        series, symbol = _parse_watchlist_item(item)
        asset = PortfolioAsset(series=series, symbol=symbol)
        if asset not in assets:
            assets.append(asset)
    return tuple(assets)


def live_strategy_gate_payload(config: AutopilotConfig) -> dict[str, Any]:
    if config.strategy_gate_path is None:
        return {"passed": False, "reason": "missing_strategy_gate_path"}
    if not config.strategy_gate_path.exists():
        return {
            "passed": False,
            "reason": "missing_strategy_gate_path",
            "path": str(config.strategy_gate_path),
        }
    report_config = StrategyReportConfig(
        path=config.strategy_gate_path,
        edges=config.strategy_gate_edges,
        max_order_cost=config.max_order_cost,
        max_trades=max(1, int(config.max_run_cost // config.max_order_cost)) if config.max_order_cost > 0 else 1,
        cooldown_seconds=config.cooldown_seconds,
        fee_rate=config.fee_rate,
        slippage_per_contract=config.slippage_per_contract,
        use_recorded_volatility=True,
        max_files=config.strategy_gate_max_files,
        latest_first=True,
    )
    gate_config = StrategyGateConfig(
        report=report_config,
        min_trades=config.strategy_gate_min_trades,
        min_win_rate=config.strategy_gate_min_win_rate,
        min_pnl=config.strategy_gate_min_pnl,
        max_open_positions=config.strategy_gate_max_open_positions,
    )
    payload = gate_payload(build_strategy_gate(gate_config))
    payload["path"] = str(config.strategy_gate_path)
    return payload


def strategy_gate_allows_probe(config: AutopilotConfig, gate: dict[str, Any]) -> bool:
    if config.strategy_gate_mode != "probe":
        return False
    reason = str(gate.get("reason") or "")
    return reason in set(config.strategy_gate_probe_reasons)


def probe_cycle_config(config: AutopilotConfig) -> AutopilotConfig:
    max_order_cost = min(config.max_order_cost, config.strategy_gate_probe_max_order_cost)
    max_run_cost = min(config.max_run_cost, config.strategy_gate_probe_max_run_cost)
    max_market_cost = config.max_market_cost
    if max_market_cost is not None:
        max_market_cost = min(max_market_cost, max_order_cost)
    max_live_order_total = config.max_live_order_total
    if max_live_order_total is not None:
        max_live_order_total = min(max_live_order_total, max_order_cost)
    return replace(
        config,
        max_order_cost=max_order_cost,
        max_run_cost=max_run_cost,
        max_market_cost=max_market_cost,
        max_live_order_total=max_live_order_total,
        daily_stop_loss=min(abs(config.daily_stop_loss), abs(config.strategy_gate_probe_daily_stop_loss)),
    )


def live_fills_from_log(path: Path) -> tuple[LiveFill, ...]:
    fills: list[LiveFill] = []
    for row in _rows(path):
        if row.get("event") != "live_order_result":
            continue
        signal = row.get("signal") or {}
        fill = row.get("fill") or {}
        shares = float(fill.get("fill_count") or 0.0)
        if shares <= 0:
            continue
        fills.append(
            LiveFill(
                ticker=str(signal["ticker"]),
                side=str(signal["side"]).upper(),
                shares=shares,
                cost=float(fill.get("fill_cost") or 0.0),
                fees=float(fill.get("fees") or 0.0),
                ts=row.get("ts"),
            )
        )
    return tuple(fills)


async def wait_for_settlements(
    client: KalshiRestClient,
    fills: tuple[LiveFill, ...],
    config: AutopilotConfig,
    *,
    sleep: Callable[[float], Awaitable[None]] = asyncio.sleep,
) -> dict[str, MarketSettlement]:
    tickers = sorted({fill.ticker for fill in fills})
    if not tickers:
        return {}
    deadline = time.monotonic() + max(0.0, config.settlement_wait_seconds)
    settlements: dict[str, MarketSettlement] = {}
    while True:
        settlements = {}
        for ticker in tickers:
            settlements[ticker] = settlement_from_market(client.get_market(ticker))
        if all(settlement.finalized for settlement in settlements.values()):
            return settlements
        if time.monotonic() >= deadline:
            return settlements
        await sleep(config.settlement_poll_seconds)


def realized_live_pnl(fills: tuple[LiveFill, ...], settlements: dict[str, MarketSettlement]) -> float:
    total = 0.0
    for fill in fills:
        settlement = settlements.get(fill.ticker)
        if settlement is None or not settlement.finalized or settlement.result_side is None:
            continue
        payout = fill.shares * payout_price(fill.side, settlement.result_side)
        total += payout - fill.cost - fill.fees
    return total


def cycle_skip_analysis(
    client: KalshiRestClient,
    log_path: Path,
    audit: BotLogAudit,
    config: AutopilotConfig,
) -> dict[str, Any] | None:
    if not config.analyze_skips:
        return None
    settlements = fetch_settlements(client, audit.tickers)
    analysis = analyze_skipped_signals(
        log_path,
        settlements,
        max_hypothetical_cost=config.max_order_cost,
        fee_rate=config.fee_rate,
        dedupe_seconds=config.skip_analysis_dedupe_seconds,
    )
    payload = skip_analysis_payload(analysis, top=config.skip_analysis_top)
    compact = compact_skip_analysis_payload(payload)
    compact["focused_shadow"] = focused_shadow_payload(
        analysis,
        asset=config.shadow_focus_asset,
        side=config.shadow_focus_side,
        max_entry_price=config.shadow_max_entry_price,
        max_seconds_to_close=config.shadow_max_seconds_to_close,
        min_net_edge=config.shadow_min_net_edge,
        min_settled_for_promotion=config.shadow_min_settled_for_promotion,
        min_win_rate_for_promotion=config.shadow_min_win_rate_for_promotion,
        min_pnl_for_promotion=config.shadow_min_pnl_for_promotion,
        top=config.skip_analysis_top,
    )
    return compact


def fetch_settlements(client: KalshiRestClient, tickers: tuple[str, ...]) -> dict[str, MarketSettlement]:
    settlements: dict[str, MarketSettlement] = {}
    for ticker in tickers:
        try:
            settlements[ticker] = settlement_from_market(client.get_market(ticker))
        except Exception:
            continue
    return settlements


def compact_skip_analysis_payload(payload: dict[str, Any]) -> dict[str, Any]:
    return {
        "skipped_signals_raw": payload.get("skipped_signals_raw", 0),
        "opportunities_deduped": payload.get("opportunities_deduped", 0),
        "settled_opportunities": payload.get("settled_opportunities", 0),
        "hypothetical_pnl": payload.get("hypothetical_pnl", 0.0),
        "hypothetical_wins": payload.get("hypothetical_wins", 0),
        "hypothetical_losses": payload.get("hypothetical_losses", 0),
        "by_asset": payload.get("by_asset", {}),
        "by_reason": payload.get("by_reason", {}),
        "top_opportunities": payload.get("top_opportunities", []),
    }


def skip_recommendation(analysis: dict[str, Any] | None) -> str | None:
    if analysis is None:
        return None
    settled = int(analysis.get("settled_opportunities") or 0)
    if settled < 5:
        return "collect_more_data"
    pnl = float(analysis.get("hypothetical_pnl") or 0.0)
    by_asset = analysis.get("by_asset") or {}
    positive_assets = [
        asset
        for asset, row in by_asset.items()
        if int(row.get("settled") or 0) >= 2 and float(row.get("hypothetical_pnl") or 0.0) > 0
    ]
    if pnl < 0:
        if positive_assets:
            return "keep_gates_review_" + "_".join(_asset_slug(asset) for asset in positive_assets)
        return "keep_gates"
    if positive_assets:
        return "review_entry_window_" + "_".join(_asset_slug(asset) for asset in positive_assets)
    return "review_entry_window"


def stop_reason(
    *,
    audit: BotLogAudit,
    fills: tuple[LiveFill, ...],
    settlements: dict[str, MarketSettlement],
    resting_orders: int,
    realized_pnl: float,
    daily_pnl: float,
    consecutive_losing_runs: int,
    config: AutopilotConfig,
) -> str | None:
    if config.stop_on_errors and audit.errors:
        return "audit_errors"
    if config.stop_on_errors and audit.live_preflight_blocks:
        return "preflight_blocks"
    if config.stop_on_resting_orders and (audit.resting_order_warnings or resting_orders):
        return "resting_orders"
    if fills and any(not settlement.finalized for settlement in settlements.values()):
        return "settlement_timeout"
    if daily_pnl <= -abs(config.daily_stop_loss):
        return "daily_stop_loss"
    if consecutive_losing_runs >= config.max_consecutive_losing_runs:
        return "consecutive_losing_runs"
    return None


def resting_order_count(client: KalshiRestClient) -> int:
    orders, _cursor = client.get_orders(status="resting", limit=100)
    return len(orders)


def balance_portfolio_value(payload: dict[str, Any]) -> float:
    value = payload.get("portfolio_value")
    if value in (None, ""):
        return 0.0
    try:
        return float(value)
    except (TypeError, ValueError):
        return 0.0


def next_cycle_log_path(log_dir: Path, cycle: int) -> Path:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return log_dir / f"live-portfolio-bot-autopilot-{stamp}-cycle-{cycle}.jsonl"


def autopilot_config_payload(config: AutopilotConfig) -> dict[str, Any]:
    payload = asdict(config)
    payload["log_dir"] = str(config.log_dir)
    payload["supervisor_log_path"] = str(config.supervisor_log_path)
    payload["strategy_gate_path"] = str(config.strategy_gate_path) if config.strategy_gate_path is not None else None
    return payload


def autopilot_cycle_payload(result: AutopilotCycleResult, daily_pnl: float) -> dict[str, Any]:
    return {
        "cycle": result.cycle,
        "log_path": str(result.log_path),
        "audit": audit_payload(result.audit),
        "fills": [asdict(fill) for fill in result.fills],
        "settlements": {ticker: asdict(settlement) for ticker, settlement in result.settlements.items()},
        "realized_pnl": result.realized_pnl,
        "daily_pnl": daily_pnl,
        "resting_orders": result.resting_orders,
        "skip_analysis": result.skip_analysis,
        "recommendation": result.recommendation,
        "stopped_reason": result.stopped_reason,
    }


def _safe_balance(client: KalshiRestClient, live: bool) -> dict[str, Any]:
    if not live:
        return {}
    return client.get_balance()


def _watchlist_tuple(value: Any) -> tuple[str, ...]:
    if isinstance(value, str):
        return tuple(item.strip() for item in value.split(",") if item.strip())
    if isinstance(value, list):
        items: list[str] = []
        for item in value:
            if isinstance(item, str):
                items.append(item)
            elif isinstance(item, dict):
                items.append(f"{item['series']}:{item['symbol']}")
            else:
                raise ValueError(f"Unsupported watchlist item: {item!r}")
        return tuple(items)
    raise ValueError("watchlist must be a comma-separated string or list.")


def _float_tuple(value: Any) -> tuple[float, ...]:
    if isinstance(value, str):
        return tuple(float(item.strip()) for item in value.split(",") if item.strip())
    if isinstance(value, list):
        return tuple(float(item) for item in value)
    if isinstance(value, tuple):
        return tuple(float(item) for item in value)
    return (float(value),)


def _string_tuple(value: Any) -> tuple[str, ...]:
    if isinstance(value, str):
        return tuple(item.strip() for item in value.split(",") if item.strip())
    if isinstance(value, list):
        return tuple(str(item) for item in value)
    if isinstance(value, tuple):
        return tuple(str(item) for item in value)
    return (str(value),)


def _parse_watchlist_item(item: str) -> tuple[str, str]:
    if ":" not in item:
        raise ValueError(f"Invalid watchlist item {item!r}; expected SERIES:SYMBOL")
    series, symbol = [part.strip() for part in item.split(":", 1)]
    if not series or not symbol:
        raise ValueError(f"Invalid watchlist item {item!r}; expected SERIES:SYMBOL")
    return series, symbol


def _asset_slug(asset: str) -> str:
    return asset.lower().replace("-usd", "").replace("-", "_")


def _rows(path: Path):
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            if line.strip():
                yield json.loads(line)
