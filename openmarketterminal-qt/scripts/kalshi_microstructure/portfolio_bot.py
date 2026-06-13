from __future__ import annotations

import asyncio
import time
from dataclasses import asdict, dataclass
from typing import Any

from .bot import (
    BotConfig,
    BotState,
    JsonlLogger,
    current_annual_volatility,
    find_active_market,
    live_fill_payload,
    live_order_plan,
    record_live_fill,
    should_trade,
    submit_live_signal,
    target_order_cost,
)
from .fees import net_expected_value, rounded_fee
from .kalshi import KalshiRestClient
from .market_hours import kalshi_maintenance_window
from .models import BinaryBook, EdgeSignal, Market
from .paper import PaperLedger
from .spot_ws import SpotCache, maintain_spot_cache
from .strategy import find_edge, market_is_above_contract, market_threshold


MIN_LIVE_TARGET_COST = 0.25


@dataclass(frozen=True)
class PortfolioAsset:
    series: str
    symbol: str


@dataclass(frozen=True)
class PortfolioCandidate:
    asset: PortfolioAsset
    market: Market
    book: BinaryBook
    signal: EdgeSignal
    spot: float
    spot_age_seconds: float | None
    annual_volatility: float


async def run_portfolio_bot(
    client: KalshiRestClient,
    config: BotConfig,
    assets: tuple[PortfolioAsset, ...],
) -> BotState:
    logger = JsonlLogger(config.log_path)
    state = BotState(ledger=PaperLedger(cash=config.max_run_cost))
    deadline = time.monotonic() + config.run_seconds
    spot_caches = {asset: SpotCache(asset.symbol) for asset in assets}
    spot_tasks = [asyncio.create_task(maintain_spot_cache(cache)) for cache in spot_caches.values()]
    logger.write(
        "bot_started",
        {**_config_payload(config), "mode": "portfolio", "assets": [asdict(asset) for asset in assets]},
    )

    try:
        while time.monotonic() < deadline:
            started = time.monotonic()
            maintenance = kalshi_maintenance_window()
            if maintenance.active:
                logger.write(
                    "trading_paused",
                    {"reason": "scheduled_maintenance", "window": maintenance.label},
                )
                print(f"portfolio: scheduled Kalshi maintenance ({maintenance.label})", flush=True)
                await asyncio.sleep(min(30.0, max(0.0, deadline - time.monotonic())))
                continue
            candidates = await _scan_assets(client, config, state, logger, spot_caches)
            candidate = _rank_candidate(candidates)
            if candidate is None:
                print("portfolio: no tradeable signal", flush=True)
            else:
                _execute_candidate(client, config, state, logger, candidate)

            sleep_for = max(0.0, config.poll_seconds - (time.monotonic() - started))
            if sleep_for:
                await asyncio.sleep(min(sleep_for, max(0.0, deadline - time.monotonic())))
    finally:
        for task in spot_tasks:
            task.cancel()
        for task in spot_tasks:
            try:
                await task
            except asyncio.CancelledError:
                pass

    logger.write(
        "bot_stopped",
        {
            "mode": "portfolio",
            "cash": state.ledger.cash,
            "spent_this_run": state.spent_this_run,
            "positions": {f"{ticker}:{side}": size for (ticker, side), size in state.ledger.positions.items()},
            "live_positions": {f"{ticker}:{side}": size for (ticker, side), size in state.live_positions.items()},
            "tickers": sorted({ticker for ticker, _side in state.live_positions}),
        },
    )
    return state


async def _scan_assets(
    client: KalshiRestClient,
    config: BotConfig,
    state: BotState,
    logger: JsonlLogger,
    spot_caches: dict[PortfolioAsset, SpotCache],
) -> list[PortfolioCandidate]:
    rows = await asyncio.gather(
        *[
            asyncio.to_thread(_scan_asset, client, config, state, logger, asset, spot_cache)
            for asset, spot_cache in spot_caches.items()
        ],
        return_exceptions=True,
    )
    candidates: list[PortfolioCandidate] = []
    for row in rows:
        if isinstance(row, Exception):
            logger.write("error", {"mode": "portfolio", "kind": row.__class__.__name__, "message": str(row)})
        elif row is not None:
            candidates.append(row)
    return candidates


def _scan_asset(
    client: KalshiRestClient,
    config: BotConfig,
    state: BotState,
    logger: JsonlLogger,
    asset: PortfolioAsset,
    spot_cache: SpotCache,
) -> PortfolioCandidate | None:
    market = find_active_market(client, asset.series)
    if market is None:
        logger.write("portfolio_scan", {"asset": asdict(asset), "reason": "no_active_market"})
        return None
    if spot_cache.price is None:
        logger.write("portfolio_scan", {"asset": asdict(asset), "ticker": market.ticker, "reason": "no_spot"})
        return None
    spot_age = spot_cache.age_seconds
    if spot_age is not None and spot_age > config.max_spot_age_seconds:
        logger.write(
            "portfolio_scan",
            {
                "asset": asdict(asset),
                "ticker": market.ticker,
                "reason": "stale_spot",
                "spot_age_seconds": spot_age,
                "max_spot_age_seconds": config.max_spot_age_seconds,
            },
        )
        return None

    book = client.get_orderbook(market.ticker)
    annual_volatility = current_annual_volatility(config, spot_cache)
    settlement_proxy = _settlement_proxy(spot_cache, config)
    if config.require_settlement_proxy and settlement_proxy is None:
        logger.write(
            "portfolio_scan",
            {
                "asset": asdict(asset),
                "ticker": market.ticker,
                "reason": "settlement_proxy_not_ready",
                "spot": spot_cache.price,
                "spot_age_seconds": spot_age,
                "threshold": market_threshold(market),
            },
        )
        return None
    model_spot = settlement_proxy["price"] if settlement_proxy is not None else spot_cache.price
    signal = find_edge(
        market=market,
        book=book,
        spot=model_spot,
        annual_volatility=annual_volatility,
        min_edge=config.min_edge,
        min_size=config.min_size,
        fee_rate=config.fee_rate,
        slippage_per_contract=config.slippage_per_contract,
    )
    logger.write(
        "portfolio_scan",
        {
            "asset": asdict(asset),
            "ticker": market.ticker,
            "spot": spot_cache.price,
            "model_spot": model_spot,
            "spot_age_seconds": spot_age,
            "settlement_proxy": settlement_proxy,
            "annual_volatility": annual_volatility,
            "threshold": market_threshold(market),
            "signal": _signal_payload(signal),
        },
    )
    if signal is None:
        return None
    if config.live and not signal_matches_live_focus(asset, signal, config):
        logger.write(
            "signal_skipped",
            {"asset": asdict(asset), "signal": _signal_payload(signal), "reason": "live_focus_gate"},
        )
        return None
    if (
        config.max_entry_seconds_to_close is not None
        and signal.seconds_to_close is not None
        and signal.seconds_to_close > config.max_entry_seconds_to_close
    ):
        logger.write(
            "signal_skipped",
            {
                "signal": _signal_payload(signal),
                "reason": "too_early",
                "seconds_to_close": signal.seconds_to_close,
                "max_entry_seconds_to_close": config.max_entry_seconds_to_close,
            },
        )
        return None
    if not signal_matches_spot_side(signal, market, model_spot):
        if config.live and focused_signal_allows_spot_side_mismatch(asset, signal, config):
            logger.write(
                "focused_shadow_candidate",
                {
                    "asset": asdict(asset),
                    "signal": _signal_payload(signal),
                    "reason": "spot_side_mismatch_allowed",
                    "spot": spot_cache.price,
                    "model_spot": model_spot,
                    "settlement_proxy": settlement_proxy,
                    "threshold": market_threshold(market),
                },
            )
        else:
            logger.write(
                "signal_skipped",
                {
                    "signal": _signal_payload(signal),
                    "reason": "spot_side_mismatch",
                    "spot": spot_cache.price,
                    "model_spot": model_spot,
                    "settlement_proxy": settlement_proxy,
                    "threshold": market_threshold(market),
                },
            )
            return None
    if not should_trade(signal, config, state, now=time.monotonic()):
        logger.write(
            "signal_skipped",
            {"asset": asdict(asset), "signal": _signal_payload(signal), "reason": "risk_gate"},
        )
        return None
    return PortfolioCandidate(
        asset=asset,
        market=market,
        book=book,
        signal=signal,
        spot=spot_cache.price,
        spot_age_seconds=spot_age,
        annual_volatility=annual_volatility,
    )


def _rank_candidate(candidates: list[PortfolioCandidate]) -> PortfolioCandidate | None:
    if not candidates:
        return None
    return max(
        candidates,
        key=lambda row: (
            row.signal.net_edge if row.signal.net_edge is not None else row.signal.edge,
            row.signal.net_expected_value if row.signal.net_expected_value is not None else row.signal.expected_value,
        ),
    )


def _execute_candidate(
    client: KalshiRestClient,
    config: BotConfig,
    state: BotState,
    logger: JsonlLogger,
    candidate: PortfolioCandidate,
) -> None:
    signal = candidate.signal
    cost = target_order_cost(signal, config, state)
    if cost <= 0:
        logger.write("signal_skipped", {"signal": _signal_payload(signal), "reason": "run_budget"})
        return
    if config.live and cost < MIN_LIVE_TARGET_COST:
        logger.write(
            "signal_skipped",
            {"signal": _signal_payload(signal), "reason": "min_live_target_cost", "target_cost": cost},
        )
        return
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
        return

    if config.live:
        plan = live_order_plan(client, signal, target_cost=cost, config=config)
        preflight = plan["preflight"]
        logger.write(
            "live_preflight",
            {"asset": asdict(candidate.asset), "signal": _signal_payload(signal), "preflight": preflight},
        )
        if not plan["approved"]:
            logger.write(
                "signal_skipped",
                {"signal": _signal_payload(signal), "reason": "live_preflight", "preflight": preflight},
            )
            print(f"{signal.ticker}: portfolio live preflight blocked warnings={preflight['warnings']}", flush=True)
            return
        response = submit_live_signal(client, signal, plan["shares"], price=plan["price"])
        logger.write(
            "live_order_submitted",
            {"asset": asdict(candidate.asset), "signal": _signal_payload(signal), "response": response},
        )
        live_fill = live_fill_payload(response, signal, price=plan["price"])
        logger.write(
            "live_order_result",
            {"asset": asdict(candidate.asset), "signal": _signal_payload(signal), "fill": live_fill},
        )
        if live_fill["fill_count"] <= 0:
            print(f"{signal.ticker}: portfolio live IOC did not fill", flush=True)
            return
        cost = live_fill["fill_cost"]
        record_live_fill(state, signal, live_fill["fill_count"])
        print(
            f"{signal.ticker}: PORTFOLIO LIVE {candidate.asset.symbol} {signal.side} "
            f"{live_fill['fill_count']:.2f} cost={cost:.4f}",
            flush=True,
        )
    else:
        shares = state.ledger.buy(signal, max_cost=cost)
        logger.write(
            "paper_order",
            {
                "mode": "portfolio",
                "asset": asdict(candidate.asset),
                "signal": _signal_payload(signal),
                "shares": shares,
                "cost": cost,
                "fee": fee,
                "net_ev": net_ev,
                "cash": state.ledger.cash,
            },
        )

    state.spent_this_run += cost
    state.spent_by_market[signal.ticker] = state.spent_by_market.get(signal.ticker, 0.0) + cost
    state.last_trade_at[signal.ticker] = time.monotonic()
    state.trades_by_market[signal.ticker] = state.trades_by_market.get(signal.ticker, 0) + 1


def signal_matches_spot_side(signal: EdgeSignal, market: Market, spot: float) -> bool:
    threshold = market_threshold(market)
    if threshold is None:
        return False
    above_side = "YES" if market_is_above_contract(market.text) else "NO"
    below_side = "NO" if above_side == "YES" else "YES"
    implied_side = above_side if spot > threshold else below_side
    return signal.side.upper() == implied_side


def signal_matches_live_focus(asset: PortfolioAsset, signal: EdgeSignal, config: BotConfig) -> bool:
    if config.live_focus_symbol is not None and asset.symbol != config.live_focus_symbol:
        return False
    if config.live_focus_side is not None and signal.side.upper() != config.live_focus_side.upper():
        return False
    return True


def focused_signal_allows_spot_side_mismatch(
    asset: PortfolioAsset,
    signal: EdgeSignal,
    config: BotConfig,
) -> bool:
    return config.live_focus_allow_spot_side_mismatch and signal_matches_live_focus(asset, signal, config)


def _settlement_proxy(spot_cache: SpotCache, config: BotConfig) -> dict[str, float | int] | None:
    if config.settlement_average_seconds <= 0:
        return None
    window = spot_cache.time_weighted_average(lookback_seconds=config.settlement_average_seconds)
    if window is None:
        return None
    if window.span_seconds < config.min_settlement_proxy_coverage_seconds:
        return None
    return {
        "price": window.price,
        "span_seconds": window.span_seconds,
        "tick_count": window.tick_count,
        "lookback_seconds": config.settlement_average_seconds,
    }


def _signal_payload(signal: EdgeSignal | None) -> dict[str, Any] | None:
    return asdict(signal) if signal else None


def _config_payload(config: BotConfig) -> dict[str, Any]:
    payload = asdict(config)
    payload["log_path"] = str(config.log_path)
    return payload
