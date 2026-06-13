from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
from dataclasses import replace as dataclass_replace
from datetime import timezone
from pathlib import Path
from urllib.error import HTTPError

from .auth import DEFAULT_KEYS_PATH, load_credentials
from .autopilot import load_autopilot_config, run_autopilot
from .backtest import (
    BacktestConfig,
    CFDecisionConfig,
    cf_decision_payload,
    replay_cf_final_window,
    replay_recording,
    replay_recording_to_settlement,
    result_payload,
)
from .book_cache import KalshiBookCache
from .bot import BotConfig, run_bot, run_websocket_bot
from .cfbenchmarks import (
    CF_INDEX_BY_SYMBOL,
    CFValueCache,
    sample_kalshi_cf_values,
    settlement_state_payload,
    tick_payload,
)
from .cf_liquidity import (
    CFLiquidityConfig,
    aggregate_cf_liquidity_summaries,
    summarize_cf_liquidity_recording,
)
from .cf_opportunity_score import score_cf_opportunity_log, scored_cf_payload
from .dashboard import DashboardConfig, serve_dashboard
from .kalshi import KalshiRestClient
from .log_reconcile import audit_bot_log, audit_payload, settle_bot_log, settled_payload
from .orders import LimitOrder
from .opportunity_watcher import (
    OpportunityLiveConfig,
    OpportunityWatchConfig,
    parse_watch_targets,
    run_opportunity_watcher,
)
from .portfolio_bot import PortfolioAsset, run_portfolio_bot
from .preflight import preflight_limit_order, preflight_payload
from .recorder import SeriesRecordConfig, SessionRecordConfig, record_series, record_session
from .range_observer import (
    RangeObserverConfig,
    fetch_range_candidates,
    parse_range_targets,
    range_relation,
    run_range_observer,
    strike_text,
)
from .range_score import score_range_observer_log, scored_range_payload
from .reference import reference_spot
from .report import (
    StrategyGateConfig,
    StrategyReportConfig,
    build_strategy_gate,
    build_strategy_report,
    gate_payload,
    report_payload,
)
from .settlement import settlement_from_market
from .skip_analysis import analyze_skipped_signals, focused_shadow_payload, skip_analysis_payload
from .strategy import find_edge, market_strike_bounds, market_threshold
from .ws import KalshiWebSocketClient


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="kalshi-edge",
        description="Kalshi binary-market microstructure scanner, recorder, and bot.",
    )
    parser.add_argument("--env", choices=("prod", "demo"), default=os.getenv("KALSHI_ENV", "prod"))
    parser.add_argument(
        "--keys",
        default=os.getenv("KALSHI_KEYS_PATH", str(DEFAULT_KEYS_PATH)),
        help="Path to Kalshi key file. Used only by authenticated commands.",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    discover = subparsers.add_parser("discover", help="Find open Kalshi markets.")
    discover.add_argument("--series", help="Filter by series ticker, e.g. KXBTC.")
    discover.add_argument("--query", default="", help="Case-insensitive text filter.")
    discover.add_argument("--limit", type=int, default=100)
    discover.add_argument("--pages", type=int, default=5, help="Maximum API pages to search.")

    range_scan = subparsers.add_parser(
        "range-scan",
        help="Research-only scan of nearest Kalshi range-market buckets.",
    )
    range_scan.add_argument("--series", default="KXBTC", help="Range series ticker, e.g. KXBTC or KXDOGE.")
    range_scan.add_argument("--symbol", default="BTC-USD", help="Reference spot symbol if --spot is omitted.")
    range_scan.add_argument("--spot", type=float, help="Override reference spot price.")
    range_scan.add_argument("--limit", type=int, default=100)
    range_scan.add_argument("--pages", type=int, default=3, help="Maximum API pages to search.")
    range_scan.add_argument("--near-count", type=int, default=8, help="Number of nearby buckets to quote.")
    range_scan.add_argument("--max-close-seconds", type=float, default=24.0 * 60.0 * 60.0)
    range_scan.add_argument("--all-closes", action="store_true", help="Do not restrict output to the soonest close.")

    observe_ranges = subparsers.add_parser(
        "observe-ranges",
        help="Research-only observer for Kalshi crypto range buckets.",
    )
    observe_ranges.add_argument(
        "--watchlist",
        default="KXBTC:BTC-USD,KXETH:ETH-USD,KXDOGE:DOGE-USD",
        help="Comma-separated SERIES:SYMBOL pairs.",
    )
    observe_ranges.add_argument("--seconds", type=float, default=60.0 * 60.0)
    observe_ranges.add_argument("--eval-seconds", type=float, default=30.0)
    observe_ranges.add_argument("--limit", type=int, default=100)
    observe_ranges.add_argument("--pages", type=int, default=3)
    observe_ranges.add_argument("--near-count", type=int, default=7)
    observe_ranges.add_argument("--max-close-seconds", type=float, default=24.0 * 60.0 * 60.0)
    observe_ranges.add_argument("--out", default="logs/range-observer.jsonl")

    range_score = subparsers.add_parser(
        "score-range-observer",
        help="Score research-only Kalshi range observer snapshots against final observed spot.",
    )
    range_score.add_argument("path")
    range_score.add_argument("--max-entry-price", type=float, default=0.98)
    range_score.add_argument("--max-cost", type=float, default=5.0)
    range_score.add_argument("--fee-rate", type=float, default=0.07)
    range_score.add_argument("--max-seconds-to-close", type=float, default=30.0 * 60.0)
    range_score.add_argument("--min-seconds-to-close", type=float, default=0.0)
    range_score.add_argument("--max-final-spot-age-seconds", type=float, default=180.0)
    range_score.add_argument("--include-duplicates", action="store_true")
    range_score.add_argument("--details", action="store_true")

    scan = subparsers.add_parser("scan", help="Scan one market for a paper edge.")
    scan.add_argument("ticker")
    scan.add_argument("--spot", type=float, help="Override reference spot price.")
    scan.add_argument("--symbol", default="BTC-USD", help="Reference spot symbol if --spot is omitted.")
    scan.add_argument("--annual-vol", type=float, default=0.70)
    scan.add_argument("--min-edge", type=float, default=0.03)
    scan.add_argument("--min-size", type=float, default=1.0)

    subparsers.add_parser("auth-check", help="Verify Kalshi API credentials without printing account data.")

    watch = subparsers.add_parser("watch", help="Watch live Kalshi WebSocket market data.")
    watch.add_argument("ticker")
    watch.add_argument("--seconds", type=float, default=10.0)
    watch.add_argument("--channels", default="ticker,orderbook_delta")

    watch_book = subparsers.add_parser("watch-book", help="Watch a live WebSocket book and print top of book.")
    watch_book.add_argument("ticker")
    watch_book.add_argument("--seconds", type=float, default=5.0)

    record = subparsers.add_parser("record-book", help="Record raw WebSocket book messages to JSONL.")
    record.add_argument("ticker")
    record.add_argument("--seconds", type=float, default=60.0)
    record.add_argument("--out", default="logs/book-recording.jsonl")

    record_session_parser = subparsers.add_parser(
        "record-session",
        help="Record market metadata, book messages, and spot samples to JSONL.",
    )
    record_session_parser.add_argument("ticker")
    record_session_parser.add_argument("--symbol", default="BTC-USD")
    record_session_parser.add_argument("--seconds", type=float, default=60.0)
    record_session_parser.add_argument("--spot-seconds", type=float, default=2.0)
    record_session_parser.add_argument("--vol-lookback-seconds", type=float, default=60.0)
    record_session_parser.add_argument("--min-annual-vol", type=float, default=0.20)
    record_session_parser.add_argument("--max-annual-vol", type=float, default=2.50)
    record_session_parser.add_argument("--disable-cf", action="store_true")
    record_session_parser.add_argument("--cf-seconds", type=float, default=1.0)
    record_session_parser.add_argument("--out", default="logs/session-recording.jsonl")

    record_series_parser = subparsers.add_parser(
        "record-series",
        help="Continuously record successive active markets in a series.",
    )
    record_series_parser.add_argument("--series", default="KXBTC15M")
    record_series_parser.add_argument("--symbol", default="BTC-USD")
    record_series_parser.add_argument("--run-seconds", type=float, default=60.0 * 60.0)
    record_series_parser.add_argument("--max-market-seconds", type=float, default=15.0 * 60.0)
    record_series_parser.add_argument("--spot-seconds", type=float, default=2.0)
    record_series_parser.add_argument("--vol-lookback-seconds", type=float, default=60.0)
    record_series_parser.add_argument("--min-annual-vol", type=float, default=0.20)
    record_series_parser.add_argument("--max-annual-vol", type=float, default=2.50)
    record_series_parser.add_argument("--disable-cf", action="store_true")
    record_series_parser.add_argument("--cf-seconds", type=float, default=1.0)
    record_series_parser.add_argument("--out-dir", default="logs/series-recordings")
    record_series_parser.add_argument("--idle-seconds", type=float, default=3.0)

    backtest = subparsers.add_parser("backtest-recording", help="Replay a session recording.")
    backtest.add_argument("path")
    backtest.add_argument("--annual-vol", type=float, default=0.70)
    backtest.add_argument("--min-edge", type=float, default=0.03)
    backtest.add_argument("--min-size", type=float, default=1.0)
    backtest.add_argument("--max-order-cost", type=float, default=2.0)
    backtest.add_argument("--max-trades", type=int, default=10)
    backtest.add_argument("--hold-seconds", type=float, default=5.0)
    backtest.add_argument("--cooldown-seconds", type=float, default=10.0)
    backtest.add_argument("--fee-rate", type=float, default=0.07)
    backtest.add_argument("--slippage-per-contract", type=float, default=0.0)
    backtest.add_argument("--use-recorded-volatility", action="store_true")
    backtest.add_argument("--liquidity-fraction", type=float, default=1.0)
    backtest.add_argument("--queue-ahead-contracts", type=float, default=0.0)
    backtest.add_argument("--min-fill-cost", type=float, default=0.01)
    backtest.add_argument("--details", action="store_true")

    sweep = subparsers.add_parser("sweep-recording", help="Run a min-edge sweep over a session recording.")
    sweep.add_argument("path")
    sweep.add_argument("--edges", default="0.02,0.03,0.04,0.05")
    sweep.add_argument("--annual-vol", type=float, default=0.70)
    sweep.add_argument("--min-size", type=float, default=1.0)
    sweep.add_argument("--max-order-cost", type=float, default=2.0)
    sweep.add_argument("--max-trades", type=int, default=10)
    sweep.add_argument("--hold-seconds", type=float, default=5.0)
    sweep.add_argument("--cooldown-seconds", type=float, default=10.0)
    sweep.add_argument("--fee-rate", type=float, default=0.07)
    sweep.add_argument("--slippage-per-contract", type=float, default=0.0)
    sweep.add_argument("--use-recorded-volatility", action="store_true")
    sweep.add_argument("--liquidity-fraction", type=float, default=1.0)
    sweep.add_argument("--queue-ahead-contracts", type=float, default=0.0)
    sweep.add_argument("--min-fill-cost", type=float, default=0.01)

    report = subparsers.add_parser("strategy-report", help="Run a strategy grid over a recording file or folder.")
    report.add_argument("path")
    report.add_argument("--edges", default="0.02,0.03,0.04,0.05")
    report.add_argument("--liquidity-fractions", default="1.0")
    report.add_argument("--queue-ahead-contracts", default="0")
    report.add_argument("--annual-vol", type=float, default=0.70)
    report.add_argument("--min-size", type=float, default=1.0)
    report.add_argument("--max-order-cost", type=float, default=2.0)
    report.add_argument("--max-trades", type=int, default=10)
    report.add_argument("--hold-seconds", type=float, default=5.0)
    report.add_argument("--cooldown-seconds", type=float, default=10.0)
    report.add_argument("--fee-rate", type=float, default=0.07)
    report.add_argument("--slippage-per-contract", type=float, default=0.0)
    report.add_argument("--use-recorded-volatility", action="store_true")
    report.add_argument("--min-fill-cost", type=float, default=0.01)
    report.add_argument("--max-files", type=int, default=None)
    report.add_argument("--latest-first", action="store_true")
    report.add_argument("--top", type=int, default=20)

    gate = subparsers.add_parser("strategy-gate", help="Run a recent replay gate before allowing live betting.")
    gate.add_argument("path")
    gate.add_argument("--edges", default="0.06,0.08,0.10")
    gate.add_argument("--liquidity-fractions", default="1.0")
    gate.add_argument("--queue-ahead-contracts", default="0")
    gate.add_argument("--annual-vol", type=float, default=0.70)
    gate.add_argument("--min-size", type=float, default=1.0)
    gate.add_argument("--max-order-cost", type=float, default=5.0)
    gate.add_argument("--max-trades", type=int, default=10)
    gate.add_argument("--hold-seconds", type=float, default=5.0)
    gate.add_argument("--cooldown-seconds", type=float, default=10.0)
    gate.add_argument("--fee-rate", type=float, default=0.07)
    gate.add_argument("--slippage-per-contract", type=float, default=0.0015)
    gate.add_argument("--use-recorded-volatility", action="store_true")
    gate.add_argument("--min-fill-cost", type=float, default=0.01)
    gate.add_argument("--max-files", type=int, default=80)
    gate.add_argument("--oldest-first", action="store_true")
    gate.add_argument("--top", type=int, default=5)
    gate.add_argument("--min-trades", type=int, default=30)
    gate.add_argument("--min-win-rate", type=float, default=0.55)
    gate.add_argument("--min-pnl", type=float, default=0.0)
    gate.add_argument("--max-open-positions", type=int, default=0)
    gate.add_argument("--min-cf-messages", type=int, default=0)
    gate.add_argument("--require-cf-final-window", action="store_true")

    cf_report = subparsers.add_parser(
        "cf-final-window-report",
        help="Replay CF-backed final-window decision logic over recorded research logs.",
    )
    cf_report.add_argument("path")
    cf_report.add_argument("--max-order-cost", type=float, default=5.0)
    cf_report.add_argument("--max-trades", type=int, default=1)
    cf_report.add_argument("--max-entry-price", type=float, default=0.98)
    cf_report.add_argument("--max-seconds-remaining", type=float, default=60.0)
    cf_report.add_argument("--min-observed-count", type=int, default=55)
    cf_report.add_argument("--max-remaining-count", type=int, default=5)
    cf_report.add_argument("--min-required-gap", type=float, default=0.0)
    cf_report.add_argument("--min-projected-gap", type=float, default=0.0)
    cf_report.add_argument("--max-threshold-latest-ratio", type=float, default=1.25)
    cf_report.add_argument("--require-locked-side", action="store_true")
    cf_report.add_argument("--side", choices=("YES", "NO"))
    cf_report.add_argument("--cooldown-seconds", type=float, default=5.0)
    cf_report.add_argument("--fee-rate", type=float, default=0.07)
    cf_report.add_argument("--liquidity-fraction", type=float, default=1.0)
    cf_report.add_argument("--queue-ahead-contracts", type=float, default=0.0)
    cf_report.add_argument("--min-fill-cost", type=float, default=0.01)
    cf_report.add_argument("--allow-multiple-per-market", action="store_true")
    cf_report.add_argument("--max-files", type=int, default=80)
    cf_report.add_argument("--oldest-first", action="store_true")
    cf_report.add_argument("--details", action="store_true")

    cf_watch = subparsers.add_parser(
        "watch-cf-opportunities",
        help="Watch final-window CF decisions, optionally armed for IOC-only live execution.",
    )
    cf_watch.add_argument(
        "--targets",
        default="KXBTC15M:BTC-USD,KXETH15M:ETH-USD,KXSOL15M:SOL-USD,KXDOGE15M:DOGE-USD",
        help="Comma-separated SERIES:SYMBOL targets.",
    )
    cf_watch.add_argument("--seconds", type=float, default=60.0 * 60.0)
    cf_watch.add_argument("--watch-seconds", type=float, default=120.0)
    cf_watch.add_argument("--eval-seconds", type=float, default=1.0)
    cf_watch.add_argument("--out", default="logs/cf-opportunity-watcher.jsonl")
    cf_watch.add_argument("--print-all", action="store_true")
    cf_watch.add_argument("--max-entry-price", type=float, default=0.98)
    cf_watch.add_argument("--max-seconds-remaining", type=float, default=60.0)
    cf_watch.add_argument("--min-observed-count", type=int, default=55)
    cf_watch.add_argument("--max-remaining-count", type=int, default=5)
    cf_watch.add_argument("--min-required-gap", type=float, default=0.0)
    cf_watch.add_argument("--min-projected-gap", type=float, default=0.0)
    cf_watch.add_argument("--max-threshold-latest-ratio", type=float, default=1.25)
    cf_watch.add_argument("--require-locked-side", action="store_true")
    cf_watch.add_argument("--side", choices=("YES", "NO"))
    cf_watch.add_argument("--min-size", type=float, default=1.0)
    cf_watch.add_argument("--live", action="store_true", help="Arm IOC-only live execution for executable CF candidates.")
    cf_watch.add_argument(
        "--confirm-live",
        default="",
        help="Required literal confirmation when --live is set: LIVE_CF_IOC",
    )
    cf_watch.add_argument("--max-live-order-cost", type=float, default=5.0)
    cf_watch.add_argument("--max-live-run-cost", type=float, default=20.0)
    cf_watch.add_argument("--max-live-orders", type=int, default=1)
    cf_watch.add_argument("--min-live-fill-cost", type=float, default=0.01)
    cf_watch.add_argument("--live-fee-buffer", type=float, default=0.0)
    cf_watch.add_argument("--allow-multiple-live-per-market", action="store_true")
    cf_watch.add_argument("--allow-live-with-portfolio-value", action="store_true")
    cf_watch.add_argument("--tiered-live-sizing", action="store_true")
    cf_watch.add_argument("--micro-live-order-cost", type=float, default=2.0)
    cf_watch.add_argument("--weak-live-order-cost", type=float, default=5.0)
    cf_watch.add_argument("--strong-live-order-cost", type=float, default=20.0)
    cf_watch.add_argument("--very-strong-live-order-cost", type=float, default=50.0)
    cf_watch.add_argument("--weak-live-max-price", type=float, default=1.0)
    cf_watch.add_argument("--strong-live-max-price", type=float, default=0.95)
    cf_watch.add_argument("--very-strong-live-max-price", type=float, default=0.90)

    cf_score = subparsers.add_parser(
        "score-cf-opportunities",
        help="Score watcher CF candidates against finalized Kalshi settlements.",
    )
    cf_score.add_argument("path")
    cf_score.add_argument("--max-entry-price", type=float, default=0.98)
    cf_score.add_argument("--max-cost", type=float, default=2.0)
    cf_score.add_argument("--fee-rate", type=float, default=0.07)
    cf_score.add_argument("--include-duplicates", action="store_true")
    cf_score.add_argument("--details", action="store_true")

    reconcile = subparsers.add_parser("reconcile-market", help="Show Kalshi market settlement status.")
    reconcile.add_argument("ticker")

    cf_observe = subparsers.add_parser(
        "observe-cf-settlement",
        help="Observe Kalshi CF Benchmarks values and compute the final-window settlement state.",
    )
    cf_observe.add_argument("ticker")
    cf_observe.add_argument("--symbol", default="BTC-USD")
    cf_observe.add_argument("--index")
    cf_observe.add_argument("--seconds", type=float, default=10.0)
    cf_observe.add_argument("--window-seconds", type=int, default=60)
    cf_observe.add_argument("--target-count", type=int, default=60)
    cf_observe.add_argument("--details", action="store_true")

    settle = subparsers.add_parser("settle-recording", help="Replay entries and settle against final market result.")
    settle.add_argument("path")
    settle.add_argument("--ticker", help="Override ticker to fetch for settlement.")
    settle.add_argument("--annual-vol", type=float, default=0.70)
    settle.add_argument("--min-edge", type=float, default=0.03)
    settle.add_argument("--min-size", type=float, default=1.0)
    settle.add_argument("--max-order-cost", type=float, default=2.0)
    settle.add_argument("--max-trades", type=int, default=10)
    settle.add_argument("--cooldown-seconds", type=float, default=10.0)
    settle.add_argument("--fee-rate", type=float, default=0.07)
    settle.add_argument("--slippage-per-contract", type=float, default=0.0)
    settle.add_argument("--use-recorded-volatility", action="store_true")
    settle.add_argument("--liquidity-fraction", type=float, default=1.0)
    settle.add_argument("--queue-ahead-contracts", type=float, default=0.0)
    settle.add_argument("--min-fill-cost", type=float, default=0.01)
    settle.add_argument("--details", action="store_true")

    settle_log = subparsers.add_parser("settle-bot-log", help="Settle paper orders in a bot JSONL log.")
    settle_log.add_argument("path")
    settle_log.add_argument("--ticker", required=True)
    settle_log.add_argument("--fee-rate", type=float, default=0.07)
    settle_log.add_argument("--details", action="store_true")

    audit_log = subparsers.add_parser("audit-bot-log", help="Summarize paper/live orders in a bot JSONL log.")
    audit_log.add_argument("path")

    analyze_skips = subparsers.add_parser(
        "analyze-skips",
        help="Analyze skipped bot signals against final market settlement.",
    )
    analyze_skips.add_argument("path")
    analyze_skips.add_argument("--max-hypothetical-cost", type=float, default=2.0)
    analyze_skips.add_argument("--fee-rate", type=float, default=0.07)
    analyze_skips.add_argument("--dedupe-seconds", type=float, default=15.0)
    analyze_skips.add_argument("--top", type=int, default=20)
    analyze_skips.add_argument("--shadow-asset", default="ETH-USD")
    analyze_skips.add_argument("--shadow-side", default="NO")
    analyze_skips.add_argument("--shadow-max-entry-price", type=float, default=0.50)
    analyze_skips.add_argument("--shadow-max-seconds-to-close", type=float, default=180.0)
    analyze_skips.add_argument("--shadow-min-net-edge", type=float, default=0.06)
    analyze_skips.add_argument("--shadow-min-settled-for-promotion", type=int, default=10)
    analyze_skips.add_argument("--shadow-min-win-rate-for-promotion", type=float, default=0.65)
    analyze_skips.add_argument("--shadow-min-pnl-for-promotion", type=float, default=0.0)
    analyze_skips.add_argument("--details", action="store_true")

    place = subparsers.add_parser("place-order", help="Create a Kalshi limit order.")
    place.add_argument("ticker")
    place.add_argument("--side", choices=("yes", "no"), required=True)
    place.add_argument("--action", choices=("buy", "sell"), default="buy")
    place.add_argument("--price", type=float, required=True)
    place.add_argument("--count", type=float, required=True)
    place.add_argument("--time-in-force", default="good_till_canceled")
    place.add_argument("--max-buy-cost", type=float, default=5.0)
    place.add_argument("--allow-taker", action="store_true", help="Disable post-only protection.")
    place.add_argument("--reduce-only", action="store_true")
    place.add_argument("--client-order-id")
    place.add_argument("--live", action="store_true", help="Actually submit the order.")

    preflight = subparsers.add_parser("preflight-order", help="Inspect cost, fees, depth, and resting orders.")
    preflight.add_argument("ticker")
    preflight.add_argument("--side", choices=("yes", "no"), required=True)
    preflight.add_argument("--action", choices=("buy", "sell"), default="buy")
    preflight.add_argument("--price", type=float, required=True)
    preflight.add_argument("--count", type=float, required=True)
    preflight.add_argument("--max-cost", type=float, default=None)
    preflight.add_argument("--fee-rate", type=float, default=0.07)

    cancel = subparsers.add_parser("cancel-order", help="Cancel a Kalshi order by order id.")
    cancel.add_argument("order_id")
    cancel.add_argument("--live", action="store_true", help="Actually cancel the order.")

    open_orders = subparsers.add_parser("open-orders", help="List resting Kalshi orders.")
    open_orders.add_argument("--ticker")
    open_orders.add_argument("--limit", type=int, default=100)

    cancel_resting = subparsers.add_parser("cancel-resting", help="Cancel resting Kalshi orders.")
    cancel_resting.add_argument("--ticker", required=True, help="Only cancel resting orders for this ticker.")
    cancel_resting.add_argument("--limit", type=int, default=100)
    cancel_resting.add_argument("--live", action="store_true", help="Actually cancel resting orders.")

    bot = subparsers.add_parser("run-bot", help="Run the BTC microstructure bot.")
    bot.add_argument("--series", default="KXBTC15M")
    bot.add_argument("--symbol", default="BTC-USD")
    bot.add_argument("--poll-seconds", type=float, default=2.0)
    bot.add_argument("--run-seconds", type=float, default=60.0)
    bot.add_argument("--annual-vol", type=float, default=0.70)
    bot.add_argument("--dynamic-volatility", action="store_true")
    bot.add_argument("--vol-lookback-seconds", type=float, default=60.0)
    bot.add_argument("--min-annual-vol", type=float, default=0.20)
    bot.add_argument("--max-annual-vol", type=float, default=2.50)
    bot.add_argument("--min-edge", type=float, default=0.03)
    bot.add_argument("--min-size", type=float, default=1.0)
    bot.add_argument("--max-order-cost", type=float, default=2.0)
    bot.add_argument("--max-run-cost", type=float, default=10.0)
    bot.add_argument("--max-market-cost", type=float, default=None)
    bot.add_argument("--max-trades-per-market", type=int, default=1)
    bot.add_argument("--cooldown-seconds", type=float, default=10.0)
    bot.add_argument("--max-entry-seconds-to-close", type=float, default=None)
    bot.add_argument("--max-spot-age-seconds", type=float, default=2.0)
    bot.add_argument("--settlement-average-seconds", type=float, default=0.0)
    bot.add_argument("--min-settlement-proxy-coverage-seconds", type=float, default=0.0)
    bot.add_argument("--require-settlement-proxy", action="store_true")
    bot.add_argument("--fee-rate", type=float, default=0.07)
    bot.add_argument("--slippage-per-contract", type=float, default=0.0)
    bot.add_argument("--log-path", default="logs/bot.jsonl")
    bot.add_argument("--max-live-order-total", type=float, default=None)
    bot.add_argument("--allow-live-preflight-warnings", action="store_true")
    bot.add_argument("--disable-live-preflight", action="store_true")
    bot.add_argument("--disable-live-requote", action="store_true")
    bot.add_argument("--live", action="store_true", help="Allow live immediate-or-cancel orders.")

    ws_bot = subparsers.add_parser("run-ws-bot", help="Run the BTC bot from a live WebSocket book cache.")
    ws_bot.add_argument("--series", default="KXBTC15M")
    ws_bot.add_argument("--symbol", default="BTC-USD")
    ws_bot.add_argument("--poll-seconds", type=float, default=1.0)
    ws_bot.add_argument("--run-seconds", type=float, default=60.0)
    ws_bot.add_argument("--annual-vol", type=float, default=0.70)
    ws_bot.add_argument("--dynamic-volatility", action="store_true")
    ws_bot.add_argument("--vol-lookback-seconds", type=float, default=60.0)
    ws_bot.add_argument("--min-annual-vol", type=float, default=0.20)
    ws_bot.add_argument("--max-annual-vol", type=float, default=2.50)
    ws_bot.add_argument("--min-edge", type=float, default=0.03)
    ws_bot.add_argument("--min-size", type=float, default=1.0)
    ws_bot.add_argument("--max-order-cost", type=float, default=2.0)
    ws_bot.add_argument("--max-run-cost", type=float, default=10.0)
    ws_bot.add_argument("--max-market-cost", type=float, default=None)
    ws_bot.add_argument("--max-trades-per-market", type=int, default=1)
    ws_bot.add_argument("--cooldown-seconds", type=float, default=10.0)
    ws_bot.add_argument("--max-spot-age-seconds", type=float, default=2.0)
    ws_bot.add_argument("--settlement-average-seconds", type=float, default=0.0)
    ws_bot.add_argument("--min-settlement-proxy-coverage-seconds", type=float, default=0.0)
    ws_bot.add_argument("--fee-rate", type=float, default=0.07)
    ws_bot.add_argument("--slippage-per-contract", type=float, default=0.0)
    ws_bot.add_argument("--log-path", default="logs/ws-bot.jsonl")
    ws_bot.add_argument("--max-live-order-total", type=float, default=None)
    ws_bot.add_argument("--allow-live-preflight-warnings", action="store_true")
    ws_bot.add_argument("--disable-live-preflight", action="store_true")
    ws_bot.add_argument("--disable-live-requote", action="store_true")
    ws_bot.add_argument("--live", action="store_true", help="Allow live immediate-or-cancel orders.")

    live_session = subparsers.add_parser(
        "run-live-session",
        help="Run the WebSocket bot while recording market data.",
    )
    live_session.add_argument("--series", default="KXBTC15M")
    live_session.add_argument("--symbol", default="BTC-USD")
    live_session.add_argument("--run-seconds", type=float, default=60.0)
    live_session.add_argument("--poll-seconds", type=float, default=1.0)
    live_session.add_argument("--annual-vol", type=float, default=0.70)
    live_session.add_argument("--dynamic-volatility", action="store_true", default=True)
    live_session.add_argument("--vol-lookback-seconds", type=float, default=30.0)
    live_session.add_argument("--min-annual-vol", type=float, default=0.20)
    live_session.add_argument("--max-annual-vol", type=float, default=2.50)
    live_session.add_argument("--min-edge", type=float, default=0.08)
    live_session.add_argument("--min-size", type=float, default=1.0)
    live_session.add_argument("--max-order-cost", type=float, default=1.0)
    live_session.add_argument("--max-run-cost", type=float, default=3.0)
    live_session.add_argument("--max-market-cost", type=float, default=None)
    live_session.add_argument("--max-trades-per-market", type=int, default=1)
    live_session.add_argument("--cooldown-seconds", type=float, default=30.0)
    live_session.add_argument("--max-spot-age-seconds", type=float, default=1.5)
    live_session.add_argument("--settlement-average-seconds", type=float, default=0.0)
    live_session.add_argument("--min-settlement-proxy-coverage-seconds", type=float, default=0.0)
    live_session.add_argument("--fee-rate", type=float, default=0.07)
    live_session.add_argument("--slippage-per-contract", type=float, default=0.001)
    live_session.add_argument("--log-path", default="logs/live-session-bot.jsonl")
    live_session.add_argument("--record-out-dir", default="logs/live-session-recordings")
    live_session.add_argument("--record-spot-seconds", type=float, default=1.0)
    live_session.add_argument("--max-market-seconds", type=float, default=15.0 * 60.0)
    live_session.add_argument("--max-live-order-total", type=float, default=1.25)
    live_session.add_argument("--allow-live-preflight-warnings", action="store_true")
    live_session.add_argument("--disable-live-preflight", action="store_true")
    live_session.add_argument("--disable-live-requote", action="store_true")
    live_session.add_argument("--live", action="store_true", help="Actually allow live IOC orders.")

    portfolio = subparsers.add_parser(
        "run-live-portfolio",
        help="Run one shared-risk live/paper bot across multiple crypto series.",
    )
    portfolio.add_argument(
        "--watchlist",
        default="KXBTC15M:BTC-USD,KXETH15M:ETH-USD,KXSOL15M:SOL-USD,KXXRP15M:XRP-USD,KXDOGE15M:DOGE-USD",
        help="Comma-separated series:symbol pairs.",
    )
    portfolio.add_argument("--run-seconds", type=float, default=60.0)
    portfolio.add_argument("--poll-seconds", type=float, default=2.0)
    portfolio.add_argument("--annual-vol", type=float, default=0.70)
    portfolio.add_argument("--dynamic-volatility", action="store_true", default=True)
    portfolio.add_argument("--vol-lookback-seconds", type=float, default=30.0)
    portfolio.add_argument("--min-annual-vol", type=float, default=0.20)
    portfolio.add_argument("--max-annual-vol", type=float, default=2.50)
    portfolio.add_argument("--min-edge", type=float, default=0.08)
    portfolio.add_argument("--min-size", type=float, default=1.0)
    portfolio.add_argument("--max-order-cost", type=float, default=1.0)
    portfolio.add_argument("--max-run-cost", type=float, default=3.0)
    portfolio.add_argument("--max-market-cost", type=float, default=None)
    portfolio.add_argument("--max-trades-per-market", type=int, default=1)
    portfolio.add_argument("--cooldown-seconds", type=float, default=30.0)
    portfolio.add_argument("--max-entry-seconds-to-close", type=float, default=None)
    portfolio.add_argument("--max-spot-age-seconds", type=float, default=1.5)
    portfolio.add_argument("--settlement-average-seconds", type=float, default=0.0)
    portfolio.add_argument("--min-settlement-proxy-coverage-seconds", type=float, default=0.0)
    portfolio.add_argument("--require-settlement-proxy", action="store_true")
    portfolio.add_argument("--fee-rate", type=float, default=0.07)
    portfolio.add_argument("--slippage-per-contract", type=float, default=0.001)
    portfolio.add_argument("--log-path", default="logs/live-portfolio-bot.jsonl")
    portfolio.add_argument("--max-live-order-total", type=float, default=1.25)
    portfolio.add_argument("--allow-live-preflight-warnings", action="store_true")
    portfolio.add_argument("--disable-live-preflight", action="store_true")
    portfolio.add_argument("--disable-live-requote", action="store_true")
    portfolio.add_argument("--live", action="store_true", help="Actually allow live IOC orders.")

    autopilot = subparsers.add_parser(
        "autopilot",
        help="Run supervised portfolio cycles with audit, settlement, and circuit breakers.",
    )
    autopilot.add_argument("--config", default="config/autopilot.json")
    autopilot.add_argument("--max-cycles", type=int, default=None)
    autopilot.add_argument("--run-seconds", type=float, default=None)
    autopilot.add_argument("--live", action="store_true", help="Allow guarded live IOC portfolio cycles.")

    dashboard = subparsers.add_parser("dashboard", help="Run the local monitoring dashboard.")
    dashboard.add_argument("--host", default="127.0.0.1")
    dashboard.add_argument("--port", type=int, default=8765)
    dashboard.add_argument("--series", default="KXBTC15M")
    dashboard.add_argument("--symbol", default="BTC-USD")
    dashboard.add_argument(
        "--watchlist",
        default="KXBTC15M:BTC-USD,KXETH15M:ETH-USD,KXSOL15M:SOL-USD,KXDOGE15M:DOGE-USD",
        help="Comma-separated series:symbol pairs to show in the dashboard scout.",
    )
    dashboard.add_argument("--poll-seconds", type=float, default=1.0)
    dashboard.add_argument("--annual-vol", type=float, default=0.70)
    dashboard.add_argument("--dynamic-volatility", action="store_true", default=True)
    dashboard.add_argument("--vol-lookback-seconds", type=float, default=30.0)
    dashboard.add_argument("--min-annual-vol", type=float, default=0.20)
    dashboard.add_argument("--max-annual-vol", type=float, default=2.50)
    dashboard.add_argument("--min-edge", type=float, default=0.03)
    dashboard.add_argument("--min-size", type=float, default=1.0)
    dashboard.add_argument("--fee-rate", type=float, default=0.07)
    dashboard.add_argument("--slippage-per-contract", type=float, default=0.001)
    dashboard.add_argument("--max-spot-age-seconds", type=float, default=2.0)
    dashboard.add_argument("--disable-paper", action="store_true", help="Turn off dashboard paper fills.")
    dashboard.add_argument("--max-paper-order-cost", type=float, default=2.0)
    dashboard.add_argument("--max-paper-run-cost", type=float, default=10.0)
    dashboard.add_argument("--max-paper-trades-per-market", type=int, default=1)
    dashboard.add_argument("--paper-cooldown-seconds", type=float, default=10.0)
    dashboard.add_argument("--paper-liquidity-fraction", type=float, default=0.25)
    dashboard.add_argument("--paper-queue-ahead-contracts", type=float, default=0.0)
    dashboard.add_argument("--min-paper-fill-cost", type=float, default=0.01)
    dashboard.add_argument("--live-log-glob", default="logs/live-session-bot*.jsonl")
    dashboard.add_argument("--autopilot-log-path", default="logs/autopilot-supervisor.jsonl")
    dashboard.add_argument("--autopilot-config-path", default="config/autopilot.json")
    dashboard.add_argument("--autopilot-output-path", default="logs/dashboard-autopilot-control.log")
    dashboard.add_argument("--allow-live-cancel", action="store_true")

    args = parser.parse_args(argv)
    needs_credentials = args.command in {
        "auth-check",
        "watch",
        "watch-book",
        "record-book",
        "record-session",
        "record-series",
        "observe-cf-settlement",
        "watch-cf-opportunities",
        "score-cf-opportunities",
        "preflight-order",
        "place-order",
        "cancel-order",
        "open-orders",
        "cancel-resting",
        "run-ws-bot",
        "run-live-session",
        "run-live-portfolio",
        "autopilot",
        "analyze-skips",
        "dashboard",
    } or (
        args.command == "run-bot" and args.live
    )
    credentials = load_credentials(args.keys) if needs_credentials else None
    client = KalshiRestClient(env=args.env, credentials=credentials)

    if args.command == "auth-check":
        return _auth_check(client)
    if args.command == "watch":
        return _watch(args, credentials)
    if args.command == "watch-book":
        return _watch_book(args, credentials)
    if args.command == "record-book":
        return _record_book(args, credentials)
    if args.command == "record-session":
        return _record_session(client, credentials, args)
    if args.command == "record-series":
        return _record_series(client, credentials, args)
    if args.command == "backtest-recording":
        return _backtest_recording(args)
    if args.command == "sweep-recording":
        return _sweep_recording(args)
    if args.command == "strategy-report":
        return _strategy_report(args)
    if args.command == "strategy-gate":
        return _strategy_gate(args)
    if args.command == "cf-final-window-report":
        return _cf_final_window_report(args)
    if args.command == "watch-cf-opportunities":
        return _watch_cf_opportunities(client, credentials, args)
    if args.command == "score-cf-opportunities":
        return _score_cf_opportunities(client, args)
    if args.command == "reconcile-market":
        return _reconcile_market(client, args)
    if args.command == "observe-cf-settlement":
        return _observe_cf_settlement(client, credentials, args)
    if args.command == "settle-recording":
        return _settle_recording(client, args)
    if args.command == "settle-bot-log":
        return _settle_bot_log(client, args)
    if args.command == "audit-bot-log":
        return _audit_bot_log(args)
    if args.command == "analyze-skips":
        return _analyze_skips(client, args)
    if args.command == "place-order":
        return _place_order(client, args)
    if args.command == "preflight-order":
        return _preflight_order(client, args)
    if args.command == "cancel-order":
        return _cancel_order(client, args)
    if args.command == "open-orders":
        return _open_orders(client, args)
    if args.command == "cancel-resting":
        return _cancel_resting(client, args)
    if args.command == "run-bot":
        return _run_bot(client, args)
    if args.command == "run-ws-bot":
        return _run_ws_bot(client, credentials, args)
    if args.command == "run-live-session":
        return _run_live_session(client, credentials, args)
    if args.command == "run-live-portfolio":
        return _run_live_portfolio(client, args)
    if args.command == "autopilot":
        return _autopilot(client, args)
    if args.command == "dashboard":
        return _dashboard(client, credentials, args)
    if args.command == "discover":
        return _discover(client, args)
    if args.command == "range-scan":
        return _range_scan(client, args)
    if args.command == "observe-ranges":
        return _observe_ranges(client, args)
    if args.command == "score-range-observer":
        return _score_range_observer(args)
    if args.command == "scan":
        return _scan(client, args)
    return 1


def _auth_check(client: KalshiRestClient) -> int:
    try:
        client.auth_check()
    except HTTPError as exc:
        print(f"auth-check failed: HTTP {exc.code}", file=sys.stderr)
        return 1
    print("auth-check succeeded")
    return 0


def _watch(args: argparse.Namespace, credentials: object) -> int:
    channels = [channel.strip() for channel in args.channels.split(",") if channel.strip()]
    ws_client = KalshiWebSocketClient(credentials=credentials, env=args.env)
    messages = asyncio.run(
        ws_client.watch(
            market_tickers=[args.ticker],
            channels=channels,
            seconds=args.seconds,
        )
    )
    for message in messages:
        msg = message.get("msg") or {}
        if not isinstance(msg, dict):
            print(message.get("type", "unknown"))
            continue
        market = msg.get("market_ticker") or msg.get("ticker") or args.ticker
        summary = {
            "type": message.get("type"),
            "market": market,
            "yes_bid": msg.get("yes_bid_dollars") or msg.get("yes_bid"),
            "yes_ask": msg.get("yes_ask_dollars") or msg.get("yes_ask"),
            "price": msg.get("price") or msg.get("price_dollars"),
            "side": msg.get("side"),
            "delta": msg.get("delta"),
        }
        print({key: value for key, value in summary.items() if value is not None})
    print(f"received {len(messages)} websocket messages")
    return 0


def _watch_book(args: argparse.Namespace, credentials: object) -> int:
    ws_client = KalshiWebSocketClient(credentials=credentials, env=args.env)
    messages = asyncio.run(
        ws_client.watch(
            market_tickers=[args.ticker],
            channels=["orderbook_delta"],
            seconds=args.seconds,
        )
    )
    cache = KalshiBookCache(args.ticker)
    updates = 0
    for message in messages:
        cache.apply(message)
        if message.get("type") in {"orderbook_snapshot", "orderbook_delta"}:
            updates += 1
    book = cache.to_book()
    print(f"{args.ticker}: applied {updates} book messages seq={cache.seq}")
    _print_level("best YES bid", book.best_yes_bid)
    _print_level("best YES ask", book.best_yes_ask)
    _print_level("best NO bid ", book.best_no_bid)
    _print_level("best NO ask ", book.best_no_ask)
    if book.yes_spread is not None:
        print(f"yes spread={book.yes_spread:.4f}")
    return 0


def _record_book(args: argparse.Namespace, credentials: object) -> int:
    ws_client = KalshiWebSocketClient(credentials=credentials, env=args.env)
    messages = asyncio.run(
        ws_client.watch(
            market_tickers=[args.ticker],
            channels=["orderbook_delta"],
            seconds=args.seconds,
        )
    )
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("a", encoding="utf-8") as handle:
        for message in messages:
            handle.write(json.dumps(message, sort_keys=True) + "\n")
    print(f"recorded {len(messages)} messages to {out}")
    return 0


def _record_session(client: KalshiRestClient, credentials: object, args: argparse.Namespace) -> int:
    config = SessionRecordConfig(
        ticker=args.ticker,
        symbol=args.symbol,
        seconds=args.seconds,
        spot_seconds=args.spot_seconds,
        vol_lookback_seconds=args.vol_lookback_seconds,
        min_annual_volatility=args.min_annual_vol,
        max_annual_volatility=args.max_annual_vol,
        cf_enabled=not args.disable_cf,
        cf_seconds=args.cf_seconds,
        out=Path(args.out),
    )
    count = asyncio.run(record_session(client, credentials, args.env, config))
    print(f"recorded {count} session rows to {config.out}")
    return 0


def _record_series(client: KalshiRestClient, credentials: object, args: argparse.Namespace) -> int:
    config = SeriesRecordConfig(
        series=args.series,
        symbol=args.symbol,
        run_seconds=args.run_seconds,
        max_market_seconds=args.max_market_seconds,
        spot_seconds=args.spot_seconds,
        vol_lookback_seconds=args.vol_lookback_seconds,
        min_annual_volatility=args.min_annual_vol,
        max_annual_volatility=args.max_annual_vol,
        cf_enabled=not args.disable_cf,
        cf_seconds=args.cf_seconds,
        out_dir=Path(args.out_dir),
        idle_seconds=args.idle_seconds,
    )
    paths = asyncio.run(record_series(client, credentials, args.env, config))
    print(f"recorded {len(paths)} market sessions to {config.out_dir}")
    for path in paths:
        print(path)
    return 0


def _backtest_recording(args: argparse.Namespace) -> int:
    config = BacktestConfig(
        path=Path(args.path),
        annual_volatility=args.annual_vol,
        min_edge=args.min_edge,
        min_size=args.min_size,
        max_order_cost=args.max_order_cost,
        max_trades=args.max_trades,
        hold_seconds=args.hold_seconds,
        cooldown_seconds=args.cooldown_seconds,
        fee_rate=args.fee_rate,
        slippage_per_contract=args.slippage_per_contract,
        use_recorded_volatility=args.use_recorded_volatility,
        liquidity_fraction=args.liquidity_fraction,
        queue_ahead_contracts=args.queue_ahead_contracts,
        min_fill_cost=args.min_fill_cost,
    )
    payload = result_payload(replay_recording(config))
    details = payload.pop("trade_details")
    print(json.dumps(payload, indent=2, sort_keys=True))
    if args.details:
        print(json.dumps(details, indent=2, sort_keys=True))
    return 0


def _sweep_recording(args: argparse.Namespace) -> int:
    edges = [float(value.strip()) for value in args.edges.split(",") if value.strip()]
    rows = []
    for edge in edges:
        result = replay_recording(
            BacktestConfig(
                path=Path(args.path),
                annual_volatility=args.annual_vol,
                min_edge=edge,
                min_size=args.min_size,
                max_order_cost=args.max_order_cost,
                max_trades=args.max_trades,
                hold_seconds=args.hold_seconds,
                cooldown_seconds=args.cooldown_seconds,
                fee_rate=args.fee_rate,
                slippage_per_contract=args.slippage_per_contract,
                use_recorded_volatility=args.use_recorded_volatility,
                liquidity_fraction=args.liquidity_fraction,
                queue_ahead_contracts=args.queue_ahead_contracts,
                min_fill_cost=args.min_fill_cost,
            )
        )
        rows.append(
            {
                "min_edge": edge,
                "trades": len(result.trades),
                "pnl": result.realized_pnl,
                "wins": sum(1 for trade in result.trades if trade.pnl > 0),
                "losses": sum(1 for trade in result.trades if trade.pnl < 0),
                "scans": result.scans,
            }
        )
    print(json.dumps(rows, indent=2, sort_keys=True))
    return 0


def _strategy_report(args: argparse.Namespace) -> int:
    config = StrategyReportConfig(
        path=Path(args.path),
        edges=_float_tuple(args.edges),
        liquidity_fractions=_float_tuple(args.liquidity_fractions),
        queue_ahead_contracts=_float_tuple(args.queue_ahead_contracts),
        annual_volatility=args.annual_vol,
        min_size=args.min_size,
        max_order_cost=args.max_order_cost,
        max_trades=args.max_trades,
        hold_seconds=args.hold_seconds,
        cooldown_seconds=args.cooldown_seconds,
        fee_rate=args.fee_rate,
        slippage_per_contract=args.slippage_per_contract,
        use_recorded_volatility=args.use_recorded_volatility,
        min_fill_cost=args.min_fill_cost,
        max_files=args.max_files,
        latest_first=args.latest_first,
    )
    rows = report_payload(build_strategy_report(config))
    if args.top > 0:
        rows = rows[: args.top]
    print(json.dumps(rows, indent=2, sort_keys=True))
    return 0


def _strategy_gate(args: argparse.Namespace) -> int:
    report_config = StrategyReportConfig(
        path=Path(args.path),
        edges=_float_tuple(args.edges),
        liquidity_fractions=_float_tuple(args.liquidity_fractions),
        queue_ahead_contracts=_float_tuple(args.queue_ahead_contracts),
        annual_volatility=args.annual_vol,
        min_size=args.min_size,
        max_order_cost=args.max_order_cost,
        max_trades=args.max_trades,
        hold_seconds=args.hold_seconds,
        cooldown_seconds=args.cooldown_seconds,
        fee_rate=args.fee_rate,
        slippage_per_contract=args.slippage_per_contract,
        use_recorded_volatility=args.use_recorded_volatility,
        min_fill_cost=args.min_fill_cost,
        max_files=args.max_files,
        latest_first=not args.oldest_first,
    )
    gate_config = StrategyGateConfig(
        report=report_config,
        min_trades=args.min_trades,
        min_win_rate=args.min_win_rate,
        min_pnl=args.min_pnl,
        max_open_positions=args.max_open_positions,
        min_cf_messages=args.min_cf_messages,
        require_cf_final_window=args.require_cf_final_window,
    )
    payload = gate_payload(build_strategy_gate(gate_config))
    if args.top > 0:
        payload["rows"] = payload["rows"][: args.top]  # type: ignore[index]
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


def _cf_final_window_report(args: argparse.Namespace) -> int:
    files = _jsonl_files(
        Path(args.path),
        max_files=args.max_files,
        latest_first=not args.oldest_first,
    )
    results = [
        replay_cf_final_window(
            CFDecisionConfig(
                path=path,
                max_order_cost=args.max_order_cost,
                max_trades=args.max_trades,
                max_entry_price=args.max_entry_price,
                max_seconds_remaining=args.max_seconds_remaining,
                min_observed_count=args.min_observed_count,
                max_remaining_count=args.max_remaining_count,
                min_required_gap=args.min_required_gap,
                min_projected_gap=args.min_projected_gap,
                max_threshold_latest_ratio=args.max_threshold_latest_ratio,
                require_locked_side=args.require_locked_side,
                allowed_side=args.side,
                cooldown_seconds=args.cooldown_seconds,
                fee_rate=args.fee_rate,
                liquidity_fraction=args.liquidity_fraction,
                queue_ahead_contracts=args.queue_ahead_contracts,
                min_fill_cost=args.min_fill_cost,
                one_trade_per_market=not args.allow_multiple_per_market,
            )
        )
        for path in files
    ]
    liquidity_config = CFLiquidityConfig(
        max_entry_price=args.max_entry_price,
        max_seconds_remaining=args.max_seconds_remaining,
        min_observed_count=args.min_observed_count,
        max_remaining_count=args.max_remaining_count,
        min_required_gap=args.min_required_gap,
        min_projected_gap=args.min_projected_gap,
        max_threshold_latest_ratio=args.max_threshold_latest_ratio,
        require_locked_side=args.require_locked_side,
        allowed_side=args.side,
    )
    liquidity_summaries = [summarize_cf_liquidity_recording(path, liquidity_config) for path in files]
    trades = [trade for result in results for trade in result.trades]
    payload: dict[str, object] = {
        "files": len(files),
        "messages": sum(result.messages for result in results),
        "scans": sum(result.scans for result in results),
        "trades": len(trades),
        "open_positions": sum(result.open_positions for result in results),
        "realized_pnl": sum(trade.pnl for trade in trades),
        "wins": sum(1 for trade in trades if trade.pnl > 0),
        "losses": sum(1 for trade in trades if trade.pnl < 0),
        "flat": sum(1 for trade in trades if trade.pnl == 0),
        "win_rate": (sum(1 for trade in trades if trade.pnl > 0) / len(trades)) if trades else None,
        "avg_entry_price": (sum(trade.entry_price for trade in trades) / len(trades)) if trades else None,
        "liquidity": aggregate_cf_liquidity_summaries(liquidity_summaries),
        "constraints": {
            "max_order_cost": args.max_order_cost,
            "max_trades": args.max_trades,
            "max_entry_price": args.max_entry_price,
            "max_seconds_remaining": args.max_seconds_remaining,
            "min_observed_count": args.min_observed_count,
            "max_remaining_count": args.max_remaining_count,
            "min_required_gap": args.min_required_gap,
            "min_projected_gap": args.min_projected_gap,
            "max_threshold_latest_ratio": args.max_threshold_latest_ratio,
            "require_locked_side": args.require_locked_side,
            "side": args.side,
            "liquidity_fraction": args.liquidity_fraction,
            "queue_ahead_contracts": args.queue_ahead_contracts,
        },
    }
    if args.details:
        payload["files_detail"] = [
            {
                "path": str(path),
                **cf_decision_payload(result),
                "liquidity": liquidity.payload(),
            }
            for path, result, liquidity in zip(files, results, liquidity_summaries)
        ]
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


def _watch_cf_opportunities(client: KalshiRestClient, credentials: object, args: argparse.Namespace) -> int:
    if args.live and args.confirm_live != "LIVE_CF_IOC":
        print("--live requires --confirm-live LIVE_CF_IOC", file=sys.stderr)
        return 2
    liquidity = CFLiquidityConfig(
        max_entry_price=args.max_entry_price,
        max_seconds_remaining=args.max_seconds_remaining,
        min_observed_count=args.min_observed_count,
        max_remaining_count=args.max_remaining_count,
        min_required_gap=args.min_required_gap,
        min_projected_gap=args.min_projected_gap,
        require_locked_side=args.require_locked_side,
        allowed_side=args.side,
        min_size=args.min_size,
        max_threshold_latest_ratio=args.max_threshold_latest_ratio,
    )
    config = OpportunityWatchConfig(
        targets=parse_watch_targets(args.targets),
        run_seconds=args.seconds,
        watch_seconds=args.watch_seconds,
        eval_seconds=args.eval_seconds,
        out=Path(args.out),
        print_all=args.print_all,
        liquidity=liquidity,
        live=OpportunityLiveConfig(
            enabled=args.live,
            max_order_cost=args.max_live_order_cost,
            max_run_cost=args.max_live_run_cost,
            max_orders=args.max_live_orders,
            min_fill_cost=args.min_live_fill_cost,
            fee_buffer=args.live_fee_buffer,
            require_empty_portfolio=not args.allow_live_with_portfolio_value,
            allow_multiple_per_market=args.allow_multiple_live_per_market,
            tiered_sizing=args.tiered_live_sizing,
            micro_order_cost=args.micro_live_order_cost,
            weak_order_cost=args.weak_live_order_cost,
            strong_order_cost=args.strong_live_order_cost,
            very_strong_order_cost=args.very_strong_live_order_cost,
            weak_max_entry_price=args.weak_live_max_price,
            strong_max_entry_price=args.strong_live_max_price,
            very_strong_max_entry_price=args.very_strong_live_max_price,
        ),
    )
    asyncio.run(run_opportunity_watcher(client, credentials, args.env, config))
    print(f"wrote opportunity rows to {config.out}")
    return 0


def _score_cf_opportunities(client: KalshiRestClient, args: argparse.Namespace) -> int:
    path = Path(args.path)
    tickers = _cf_opportunity_tickers(path)
    settlements = {}
    for ticker in tickers:
        try:
            settlement = settlement_from_market(client.get_market(ticker))
        except Exception as exc:  # noqa: BLE001 - report partial scoring instead of failing all.
            print(f"warning: could not fetch settlement for {ticker}: {exc}", file=sys.stderr)
            continue
        if settlement.finalized:
            settlements[ticker] = settlement
    scored = score_cf_opportunity_log(
        path,
        settlements,
        max_entry_price=args.max_entry_price,
        max_cost=args.max_cost,
        fee_rate=args.fee_rate,
        dedupe_per_market=not args.include_duplicates,
    )
    payload = scored_cf_payload(scored)
    payload["candidate_tickers"] = len(tickers)
    payload["finalized_tickers"] = len(settlements)
    payload["max_entry_price"] = args.max_entry_price
    payload["max_cost"] = args.max_cost
    if not args.details:
        payload.pop("opportunities", None)
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


def _cf_opportunity_tickers(path: Path) -> set[str]:
    tickers: set[str] = set()
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            if not line.strip():
                continue
            row = json.loads(line)
            if row.get("event") != "cf_opportunity":
                continue
            ticker = str((row.get("market") or {}).get("ticker") or "")
            if ticker:
                tickers.add(ticker)
    return tickers


def _reconcile_market(client: KalshiRestClient, args: argparse.Namespace) -> int:
    market = client.get_market(args.ticker)
    settlement = settlement_from_market(market)
    print(
        json.dumps(
            {
                "ticker": settlement.ticker,
                "status": settlement.status,
                "finalized": settlement.finalized,
                "result_side": settlement.result_side,
                "expiration_value": settlement.expiration_value,
                "threshold": settlement.threshold,
            },
            indent=2,
            sort_keys=True,
        )
    )
    return 0


def _observe_cf_settlement(
    client: KalshiRestClient,
    credentials: object,
    args: argparse.Namespace,
) -> int:
    market = client.get_market(args.ticker)
    threshold = market_threshold(market)
    if threshold is None:
        print(f"Could not infer threshold for {market.ticker}", file=sys.stderr)
        return 1
    if market.close_time is None:
        print(f"Could not infer close time for {market.ticker}", file=sys.stderr)
        return 1
    index = args.index or CF_INDEX_BY_SYMBOL.get(args.symbol)
    if index is None:
        print(f"Could not infer CF index for {args.symbol}; pass --index.", file=sys.stderr)
        return 1

    ticks = asyncio.run(
        sample_kalshi_cf_values(
            credentials,  # type: ignore[arg-type]
            env=args.env,
            indices=[index],
            seconds=args.seconds,
        )
    )
    cache = CFValueCache(index)
    for tick in ticks:
        cache.update(tick)
    state = cache.settlement_state(
        threshold=threshold,
        window_end=market.close_time,
        window_seconds=args.window_seconds,
        target_count=args.target_count,
    )
    payload: dict[str, object] = {
        "market": market.ticker,
        "symbol": args.symbol,
        "index": index,
        "threshold": threshold,
        "ticks": len(ticks),
        "settlement_state": settlement_state_payload(state),
    }
    if args.details:
        payload["tick_details"] = [tick_payload(tick) for tick in ticks[-20:]]
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


def _settle_recording(client: KalshiRestClient, args: argparse.Namespace) -> int:
    ticker = args.ticker or _recording_ticker(Path(args.path))
    market = client.get_market(ticker)
    settlement = settlement_from_market(market)
    if not settlement.finalized:
        print(
            f"{ticker} is not finalized yet: status={settlement.status} result={settlement.result_side}",
            file=sys.stderr,
        )
        return 1
    config = BacktestConfig(
        path=Path(args.path),
        annual_volatility=args.annual_vol,
        min_edge=args.min_edge,
        min_size=args.min_size,
        max_order_cost=args.max_order_cost,
        max_trades=args.max_trades,
        cooldown_seconds=args.cooldown_seconds,
        fee_rate=args.fee_rate,
        slippage_per_contract=args.slippage_per_contract,
        use_recorded_volatility=args.use_recorded_volatility,
        liquidity_fraction=args.liquidity_fraction,
        queue_ahead_contracts=args.queue_ahead_contracts,
        min_fill_cost=args.min_fill_cost,
    )
    payload = result_payload(replay_recording_to_settlement(config, settlement))
    details = payload.pop("trade_details")
    payload["settlement"] = {
        "ticker": settlement.ticker,
        "result_side": settlement.result_side,
        "expiration_value": settlement.expiration_value,
        "threshold": settlement.threshold,
    }
    print(json.dumps(payload, indent=2, sort_keys=True))
    if args.details:
        print(json.dumps(details, indent=2, sort_keys=True))
    return 0


def _settle_bot_log(client: KalshiRestClient, args: argparse.Namespace) -> int:
    settlement = settlement_from_market(client.get_market(args.ticker))
    if not settlement.finalized:
        print(
            f"{args.ticker} is not finalized yet: status={settlement.status} result={settlement.result_side}",
            file=sys.stderr,
        )
        return 1
    payload = settled_payload(settle_bot_log(Path(args.path), settlement, fee_rate=args.fee_rate))
    details = payload.pop("order_details")
    payload["settlement"] = {
        "ticker": settlement.ticker,
        "result_side": settlement.result_side,
        "expiration_value": settlement.expiration_value,
        "threshold": settlement.threshold,
    }
    print(json.dumps(payload, indent=2, sort_keys=True))
    if args.details:
        print(json.dumps(details, indent=2, sort_keys=True))
    return 0


def _audit_bot_log(args: argparse.Namespace) -> int:
    print(json.dumps(audit_payload(audit_bot_log(Path(args.path))), indent=2, sort_keys=True))
    return 0


def _analyze_skips(client: KalshiRestClient, args: argparse.Namespace) -> int:
    path = Path(args.path)
    audit = audit_bot_log(path)
    settlements = {}
    for ticker in audit.tickers:
        try:
            settlements[ticker] = settlement_from_market(client.get_market(ticker))
        except Exception as exc:  # noqa: BLE001 - report partial data if one market lookup flakes.
            print(f"warning: could not fetch settlement for {ticker}: {exc}", file=sys.stderr)
    analysis = analyze_skipped_signals(
        path,
        settlements,
        max_hypothetical_cost=args.max_hypothetical_cost,
        fee_rate=args.fee_rate,
        dedupe_seconds=args.dedupe_seconds,
    )
    payload = skip_analysis_payload(analysis, details=args.details, top=args.top)
    payload["focused_shadow"] = focused_shadow_payload(
        analysis,
        asset=args.shadow_asset,
        side=args.shadow_side,
        max_entry_price=args.shadow_max_entry_price,
        max_seconds_to_close=args.shadow_max_seconds_to_close,
        min_net_edge=args.shadow_min_net_edge,
        min_settled_for_promotion=args.shadow_min_settled_for_promotion,
        min_win_rate_for_promotion=args.shadow_min_win_rate_for_promotion,
        min_pnl_for_promotion=args.shadow_min_pnl_for_promotion,
        top=args.top,
    )
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


def _recording_ticker(path: Path) -> str:
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            if not line.strip():
                continue
            row = json.loads(line)
            if row.get("event") == "session_started":
                return str(row["market"]["ticker"])
    raise ValueError(f"No session_started market row found in {path}")


def _float_tuple(value: str) -> tuple[float, ...]:
    return tuple(float(item.strip()) for item in value.split(",") if item.strip())


def _jsonl_files(path: Path, *, max_files: int | None = None, latest_first: bool = False) -> list[Path]:
    if path.is_file():
        return [path]
    files = [candidate for candidate in path.rglob("*.jsonl") if candidate.is_file()]
    files = sorted(files, key=lambda candidate: candidate.stat().st_mtime, reverse=latest_first)
    if max_files is not None and max_files > 0:
        files = files[:max_files]
    return files


def _place_order(client: KalshiRestClient, args: argparse.Namespace) -> int:
    order = LimitOrder(
        ticker=args.ticker,
        side=args.side,
        action=args.action,
        price=args.price,
        count=args.count,
        time_in_force=args.time_in_force,
        post_only=not args.allow_taker,
        reduce_only=args.reduce_only,
        client_order_id=args.client_order_id,
    )
    order.validate(max_buy_cost=args.max_buy_cost)
    payload = order.to_api()

    if not args.live:
        print("dry run: order not submitted")
        print(json.dumps(_redact_order_payload(payload), indent=2))
        print("add --live to submit")
        return 0

    response = client.create_order(payload)
    created = response.get("order", response)
    print(
        "order submitted: "
        f"id={created.get('order_id')} status={created.get('status')} "
        f"client_order_id={created.get('client_order_id')}"
    )
    return 0


def _preflight_order(client: KalshiRestClient, args: argparse.Namespace) -> int:
    book = client.get_orderbook(args.ticker)
    resting_orders = None
    try:
        orders, _ = client.get_orders(ticker=args.ticker, status="resting", limit=100)
        resting_orders = len(orders)
    except Exception:
        resting_orders = None
    payload = preflight_payload(
        preflight_limit_order(
            book=book,
            ticker=args.ticker,
            side=args.side,
            action=args.action,
            price=args.price,
            count=args.count,
            fee_rate=args.fee_rate,
            resting_orders=resting_orders,
            max_cost=args.max_cost,
        )
    )
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


def _cancel_order(client: KalshiRestClient, args: argparse.Namespace) -> int:
    if not args.live:
        print(f"dry run: would cancel order {args.order_id}")
        print("add --live to cancel")
        return 0
    response = client.cancel_order(args.order_id)
    order = response.get("order", {})
    print(
        "order canceled: "
        f"id={order.get('order_id', args.order_id)} "
        f"status={order.get('status', 'unknown')} "
        f"reduced_by={response.get('reduced_by_fp')}"
    )
    return 0


def _open_orders(client: KalshiRestClient, args: argparse.Namespace) -> int:
    orders, cursor = client.get_orders(ticker=args.ticker, status="resting", limit=args.limit)
    for order in orders:
        price = order.get("yes_price_dollars") or order.get("no_price_dollars")
        print(
            f"{order.get('order_id')} {order.get('ticker')} "
            f"{order.get('action')} {order.get('side')} price={price} "
            f"remaining={order.get('remaining_count_fp')}"
        )
    print(f"resting orders: {len(orders)}")
    if cursor:
        print(f"next cursor: {cursor}", file=sys.stderr)
    return 0


def _cancel_resting(client: KalshiRestClient, args: argparse.Namespace) -> int:
    orders, _ = client.get_orders(ticker=args.ticker, status="resting", limit=args.limit)
    order_ids = [str(order["order_id"]) for order in orders if order.get("order_id")]
    if not order_ids:
        print(f"no resting orders for {args.ticker}")
        return 0
    if not args.live:
        print(f"dry run: would cancel {len(order_ids)} resting orders for {args.ticker}")
        for order_id in order_ids:
            print(order_id)
        print("add --live to cancel")
        return 0
    response = client.batch_cancel_orders(order_ids)
    canceled = response.get("orders", [])
    print(f"cancel request sent for {len(order_ids)} orders; response items={len(canceled)}")
    return 0


def _run_bot(client: KalshiRestClient, args: argparse.Namespace) -> int:
    config = BotConfig(
        series=args.series,
        symbol=args.symbol,
        poll_seconds=args.poll_seconds,
        run_seconds=args.run_seconds,
        annual_volatility=args.annual_vol,
        dynamic_volatility=args.dynamic_volatility,
        vol_lookback_seconds=args.vol_lookback_seconds,
        min_annual_volatility=args.min_annual_vol,
        max_annual_volatility=args.max_annual_vol,
        min_edge=args.min_edge,
        min_size=args.min_size,
        max_order_cost=args.max_order_cost,
        max_run_cost=args.max_run_cost,
        max_market_cost=args.max_market_cost,
        max_trades_per_market=args.max_trades_per_market,
        cooldown_seconds=args.cooldown_seconds,
        max_entry_seconds_to_close=args.max_entry_seconds_to_close,
        max_spot_age_seconds=args.max_spot_age_seconds,
        settlement_average_seconds=args.settlement_average_seconds,
        min_settlement_proxy_coverage_seconds=args.min_settlement_proxy_coverage_seconds,
        require_settlement_proxy=args.require_settlement_proxy,
        fee_rate=args.fee_rate,
        slippage_per_contract=args.slippage_per_contract,
        log_path=Path(args.log_path),
        live=args.live,
        require_live_preflight=not args.disable_live_preflight,
        allow_live_preflight_warnings=args.allow_live_preflight_warnings,
        max_live_order_total=args.max_live_order_total,
        allow_live_requote=not args.disable_live_requote,
    )
    if config.live:
        print("LIVE MODE ENABLED: immediate-or-cancel orders may be submitted.", file=sys.stderr)
    else:
        print("paper mode: no live orders will be submitted")
    state = run_bot(client, config)
    print(
        f"bot stopped: cash={state.ledger.cash:.2f} "
        f"spent={state.spent_this_run:.2f} positions={len(state.ledger.positions)}"
    )
    print(f"log: {config.log_path}")
    return 0


def _run_ws_bot(client: KalshiRestClient, credentials: object, args: argparse.Namespace) -> int:
    config = BotConfig(
        series=args.series,
        symbol=args.symbol,
        poll_seconds=args.poll_seconds,
        run_seconds=args.run_seconds,
        annual_volatility=args.annual_vol,
        dynamic_volatility=args.dynamic_volatility,
        vol_lookback_seconds=args.vol_lookback_seconds,
        min_annual_volatility=args.min_annual_vol,
        max_annual_volatility=args.max_annual_vol,
        min_edge=args.min_edge,
        min_size=args.min_size,
        max_order_cost=args.max_order_cost,
        max_run_cost=args.max_run_cost,
        max_market_cost=args.max_market_cost,
        max_trades_per_market=args.max_trades_per_market,
        cooldown_seconds=args.cooldown_seconds,
        max_spot_age_seconds=args.max_spot_age_seconds,
        settlement_average_seconds=args.settlement_average_seconds,
        min_settlement_proxy_coverage_seconds=args.min_settlement_proxy_coverage_seconds,
        fee_rate=args.fee_rate,
        slippage_per_contract=args.slippage_per_contract,
        log_path=Path(args.log_path),
        live=args.live,
        require_live_preflight=not args.disable_live_preflight,
        allow_live_preflight_warnings=args.allow_live_preflight_warnings,
        max_live_order_total=args.max_live_order_total,
        allow_live_requote=not args.disable_live_requote,
    )
    if config.live:
        print("LIVE WS MODE ENABLED: immediate-or-cancel orders may be submitted.", file=sys.stderr)
    else:
        print("websocket paper mode: no live orders will be submitted")
    state = asyncio.run(run_websocket_bot(client, credentials, args.env, config))
    print(
        f"websocket bot stopped: cash={state.ledger.cash:.2f} "
        f"spent={state.spent_this_run:.2f} positions={len(state.ledger.positions)}"
    )
    print(f"log: {config.log_path}")
    return 0


def _run_live_session(client: KalshiRestClient, credentials: object, args: argparse.Namespace) -> int:
    bot_config = BotConfig(
        series=args.series,
        symbol=args.symbol,
        poll_seconds=args.poll_seconds,
        run_seconds=args.run_seconds,
        annual_volatility=args.annual_vol,
        dynamic_volatility=args.dynamic_volatility,
        vol_lookback_seconds=args.vol_lookback_seconds,
        min_annual_volatility=args.min_annual_vol,
        max_annual_volatility=args.max_annual_vol,
        min_edge=args.min_edge,
        min_size=args.min_size,
        max_order_cost=args.max_order_cost,
        max_run_cost=args.max_run_cost,
        max_market_cost=args.max_market_cost,
        max_trades_per_market=args.max_trades_per_market,
        cooldown_seconds=args.cooldown_seconds,
        max_spot_age_seconds=args.max_spot_age_seconds,
        settlement_average_seconds=args.settlement_average_seconds,
        min_settlement_proxy_coverage_seconds=args.min_settlement_proxy_coverage_seconds,
        fee_rate=args.fee_rate,
        slippage_per_contract=args.slippage_per_contract,
        log_path=Path(args.log_path),
        live=args.live,
        require_live_preflight=not args.disable_live_preflight,
        allow_live_preflight_warnings=args.allow_live_preflight_warnings,
        max_live_order_total=args.max_live_order_total,
        allow_live_requote=not args.disable_live_requote,
    )
    record_config = SeriesRecordConfig(
        series=args.series,
        symbol=args.symbol,
        run_seconds=args.run_seconds,
        max_market_seconds=args.max_market_seconds,
        spot_seconds=args.record_spot_seconds,
        vol_lookback_seconds=args.vol_lookback_seconds,
        min_annual_volatility=args.min_annual_vol,
        max_annual_volatility=args.max_annual_vol,
        out_dir=Path(args.record_out_dir),
    )
    if bot_config.live:
        print(
            "LIVE SESSION ENABLED: tiny immediate-or-cancel orders may be submitted after preflight.",
            file=sys.stderr,
        )
    else:
        print("live-session paper mode: no live orders will be submitted")
    state, paths = asyncio.run(_run_live_session_async(client, credentials, args.env, bot_config, record_config))
    print(
        f"live session stopped: cash={state.ledger.cash:.2f} "
        f"spent={state.spent_this_run:.2f} positions={len(state.ledger.positions)}"
    )
    print(f"bot log: {bot_config.log_path}")
    print(f"recordings: {record_config.out_dir} ({len(paths)} files)")
    for path in paths:
        print(path)
    return 0


def _run_live_portfolio(client: KalshiRestClient, args: argparse.Namespace) -> int:
    assets = tuple(PortfolioAsset(series=series, symbol=symbol) for series, symbol in _parse_watchlist(args.watchlist))
    config = BotConfig(
        poll_seconds=args.poll_seconds,
        run_seconds=args.run_seconds,
        annual_volatility=args.annual_vol,
        dynamic_volatility=args.dynamic_volatility,
        vol_lookback_seconds=args.vol_lookback_seconds,
        min_annual_volatility=args.min_annual_vol,
        max_annual_volatility=args.max_annual_vol,
        min_edge=args.min_edge,
        min_size=args.min_size,
        max_order_cost=args.max_order_cost,
        max_run_cost=args.max_run_cost,
        max_market_cost=args.max_market_cost,
        max_trades_per_market=args.max_trades_per_market,
        cooldown_seconds=args.cooldown_seconds,
        max_entry_seconds_to_close=args.max_entry_seconds_to_close,
        max_spot_age_seconds=args.max_spot_age_seconds,
        settlement_average_seconds=args.settlement_average_seconds,
        min_settlement_proxy_coverage_seconds=args.min_settlement_proxy_coverage_seconds,
        require_settlement_proxy=args.require_settlement_proxy,
        fee_rate=args.fee_rate,
        slippage_per_contract=args.slippage_per_contract,
        log_path=Path(args.log_path),
        live=args.live,
        require_live_preflight=not args.disable_live_preflight,
        allow_live_preflight_warnings=args.allow_live_preflight_warnings,
        max_live_order_total=args.max_live_order_total,
        allow_live_requote=not args.disable_live_requote,
    )
    if config.live:
        print(
            "LIVE PORTFOLIO ENABLED: shared-risk immediate-or-cancel orders may be submitted.",
            file=sys.stderr,
        )
    else:
        print("portfolio paper mode: no live orders will be submitted")
    state = asyncio.run(run_portfolio_bot(client, config, assets))
    print(
        f"live portfolio stopped: cash={state.ledger.cash:.2f} "
        f"spent={state.spent_this_run:.2f} positions={len(state.ledger.positions)}"
    )
    print(f"bot log: {config.log_path}")
    return 0


def _autopilot(client: KalshiRestClient, args: argparse.Namespace) -> int:
    path = Path(args.config)
    config = load_autopilot_config(path if path.exists() else None)
    if args.max_cycles is not None:
        config = dataclass_replace(config, max_cycles=args.max_cycles)
    if args.run_seconds is not None:
        config = dataclass_replace(config, run_seconds=args.run_seconds)
    if args.live:
        config = dataclass_replace(config, live=True)

    if config.live:
        print("AUTOPILOT LIVE ENABLED: supervised IOC orders may be submitted.", file=sys.stderr)
    else:
        print("autopilot paper mode: no live orders will be submitted")
    results = asyncio.run(run_autopilot(client, config))
    for result in results:
        print(
            f"autopilot cycle {result.cycle}: fills={len(result.fills)} "
            f"pnl={result.realized_pnl:.4f} resting={result.resting_orders} "
            f"stop={result.stopped_reason or 'none'} log={result.log_path}"
        )
    print(f"supervisor log: {config.supervisor_log_path}")
    return 0


async def _run_live_session_async(
    client: KalshiRestClient,
    credentials: object,
    env: str,
    bot_config: BotConfig,
    record_config: SeriesRecordConfig,
):
    bot_task = asyncio.create_task(run_websocket_bot(client, credentials, env, bot_config))
    record_task = asyncio.create_task(record_series(client, credentials, env, record_config))
    state, paths = await asyncio.gather(bot_task, record_task)
    return state, paths


def _dashboard(client: KalshiRestClient, credentials: object, args: argparse.Namespace) -> int:
    config = DashboardConfig(
        env=args.env,
        series=args.series,
        symbol=args.symbol,
        watchlist=_parse_watchlist(args.watchlist, fallback=(args.series, args.symbol)),
        poll_seconds=args.poll_seconds,
        annual_volatility=args.annual_vol,
        dynamic_volatility=args.dynamic_volatility,
        vol_lookback_seconds=args.vol_lookback_seconds,
        min_annual_volatility=args.min_annual_vol,
        max_annual_volatility=args.max_annual_vol,
        min_edge=args.min_edge,
        min_size=args.min_size,
        fee_rate=args.fee_rate,
        slippage_per_contract=args.slippage_per_contract,
        max_spot_age_seconds=args.max_spot_age_seconds,
        allow_live_cancel=args.allow_live_cancel,
        paper_trading=not args.disable_paper,
        max_paper_order_cost=args.max_paper_order_cost,
        max_paper_run_cost=args.max_paper_run_cost,
        max_paper_trades_per_market=args.max_paper_trades_per_market,
        paper_cooldown_seconds=args.paper_cooldown_seconds,
        paper_liquidity_fraction=args.paper_liquidity_fraction,
        paper_queue_ahead_contracts=args.paper_queue_ahead_contracts,
        min_paper_fill_cost=args.min_paper_fill_cost,
        live_log_glob=args.live_log_glob,
        autopilot_log_path=args.autopilot_log_path,
        autopilot_config_path=args.autopilot_config_path,
        autopilot_output_path=args.autopilot_output_path,
    )
    server = serve_dashboard(
        client=client,
        credentials=credentials,
        config=config,
        host=args.host,
        port=args.port,
    )
    print(f"dashboard: http://{args.host}:{args.port}")
    print("press Ctrl+C to stop")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.engine.stop_event.set()  # type: ignore[attr-defined]
        server.shutdown()
        server.server_close()
    return 0


def _parse_watchlist(
    raw: str,
    *,
    fallback: tuple[str, str] | None = None,
) -> tuple[tuple[str, str], ...]:
    pairs: list[tuple[str, str]] = []
    for item in (raw or "").split(","):
        item = item.strip()
        if not item:
            continue
        if ":" not in item:
            raise SystemExit(f"Invalid --watchlist item {item!r}; expected SERIES:SYMBOL")
        series, symbol = [part.strip() for part in item.split(":", 1)]
        if not series or not symbol:
            raise SystemExit(f"Invalid --watchlist item {item!r}; expected SERIES:SYMBOL")
        pair = (series, symbol)
        if pair not in pairs:
            pairs.append(pair)
    if fallback is not None and fallback not in pairs:
        pairs.insert(0, fallback)
    return tuple(pairs)


def _redact_order_payload(payload: dict[str, object]) -> dict[str, object]:
    return {key: value for key, value in payload.items() if key != "client_order_id"}


def _discover(client: KalshiRestClient, args: argparse.Namespace) -> int:
    markets = []
    cursor = None
    for _ in range(max(1, args.pages)):
        page, cursor = client.get_markets(
            series_ticker=args.series,
            limit=args.limit,
            cursor=cursor,
        )
        markets.extend(page)
        if not cursor:
            break
    query = args.query.lower().strip()
    if query:
        markets = [market for market in markets if query in market.text.lower()]
    for market in markets:
        close = market.close_time.astimezone(timezone.utc).isoformat() if market.close_time else "unknown"
        print(f"{market.ticker:32} {market.status:8} close={close}{_strike_text(market)}")
        print(f"  {market.title}")
        if market.subtitle:
            print(f"  {market.subtitle}")
    if cursor:
        print(f"\nNext cursor: {cursor}", file=sys.stderr)
    return 0


def _range_scan(client: KalshiRestClient, args: argparse.Namespace) -> int:
    spot = args.spot if args.spot is not None else reference_spot(args.symbol)
    candidates = fetch_range_candidates(
        client,
        series=args.series,
        spot=spot,
        limit=args.limit,
        pages=args.pages,
        max_close_seconds=args.max_close_seconds,
    )
    if not candidates:
        print(f"no active range buckets found for {args.series}")
        return 0

    print(f"{args.series} range scan spot={spot:g} symbol={args.symbol}")
    for candidate in candidates[: max(1, args.near_count)]:
        market = candidate.market
        try:
            book = client.get_orderbook(market.ticker)
        except HTTPError as exc:
            print(f"{market.ticker:32} close_in={candidate.seconds_to_close:7.0f}s {strike_text(market)}")
            print(f"  book unavailable: HTTP {exc.code}")
            continue
        strike_type, floor, cap = market_strike_bounds(market)
        print(
            f"{market.ticker:32} close_in={candidate.seconds_to_close:7.0f}s "
            f"distance={candidate.distance:g} {strike_text(market)} "
            f"spot={range_relation(spot, strike_type, floor, cap)}"
        )
        _print_level("  YES bid", book.best_yes_bid)
        _print_level("  YES ask", book.best_yes_ask)
        _print_level("  NO bid ", book.best_no_bid)
        _print_level("  NO ask ", book.best_no_ask)
    return 0


def _observe_ranges(client: KalshiRestClient, args: argparse.Namespace) -> int:
    config = RangeObserverConfig(
        targets=parse_range_targets(args.watchlist),
        seconds=args.seconds,
        eval_seconds=args.eval_seconds,
        limit=args.limit,
        pages=args.pages,
        near_count=args.near_count,
        max_close_seconds=args.max_close_seconds,
        out=Path(args.out),
    )
    rows = run_range_observer(client, config)
    print(f"range observer wrote {rows} rows to {config.out}")
    return 0


def _score_range_observer(args: argparse.Namespace) -> int:
    scored = score_range_observer_log(
        Path(args.path),
        max_entry_price=args.max_entry_price,
        max_cost=args.max_cost,
        fee_rate=args.fee_rate,
        max_seconds_to_close=args.max_seconds_to_close,
        min_seconds_to_close=args.min_seconds_to_close,
        max_final_spot_age_seconds=args.max_final_spot_age_seconds,
        dedupe_per_market=not args.include_duplicates,
    )
    payload = scored_range_payload(scored)
    payload["max_entry_price"] = args.max_entry_price
    payload["max_cost"] = args.max_cost
    payload["max_seconds_to_close"] = args.max_seconds_to_close
    payload["min_seconds_to_close"] = args.min_seconds_to_close
    payload["max_final_spot_age_seconds"] = args.max_final_spot_age_seconds
    if not args.details:
        payload.pop("opportunities", None)
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


def _strike_text(market: Market) -> str:
    text = strike_text(market)
    if text:
        return f" {text}"
    threshold = market_threshold(market)
    return f" threshold~{threshold:g}" if threshold else ""


def _scan(client: KalshiRestClient, args: argparse.Namespace) -> int:
    market = client.get_market(args.ticker)
    book = client.get_orderbook(args.ticker)
    spot = args.spot if args.spot is not None else reference_spot(args.symbol)
    signal = find_edge(
        market=market,
        book=book,
        spot=spot,
        annual_volatility=args.annual_vol,
        min_edge=args.min_edge,
        min_size=args.min_size,
    )

    print(f"{market.ticker}: {market.title}")
    print(f"spot={spot:.2f}")
    _print_level("best YES bid", book.best_yes_bid)
    _print_level("best YES ask", book.best_yes_ask)
    _print_level("best NO bid ", book.best_no_bid)
    _print_level("best NO ask ", book.best_no_ask)
    if book.yes_spread is not None:
        print(f"yes spread={book.yes_spread:.4f}")

    if signal is None:
        print("paper signal: none")
        return 0

    close = f"{signal.seconds_to_close:.0f}s" if signal.seconds_to_close is not None else "unknown"
    print(
        "paper signal: "
        f"BUY {signal.side} ask={signal.ask_price:.4f} "
        f"fair={signal.fair_price:.4f} edge={signal.edge:.4f} "
        f"size={signal.size:.2f} ev={signal.expected_value:.2f} close_in={close}"
    )
    print(f"reason: {signal.reason}")
    return 0


def _print_level(label: str, level: object) -> None:
    if level is None:
        print(f"{label}: none")
        return
    print(f"{label}: {level.price:.4f} x {level.size:.2f}")


if __name__ == "__main__":
    raise SystemExit(main())
