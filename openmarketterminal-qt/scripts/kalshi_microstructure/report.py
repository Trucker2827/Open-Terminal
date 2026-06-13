from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from pathlib import Path
from statistics import mean

from .backtest import BacktestConfig, BacktestResult, ReplayTrade, replay_recording


@dataclass(frozen=True)
class StrategyReportConfig:
    path: Path
    edges: tuple[float, ...] = (0.02, 0.03, 0.04, 0.05)
    liquidity_fractions: tuple[float, ...] = (1.0,)
    queue_ahead_contracts: tuple[float, ...] = (0.0,)
    annual_volatility: float = 0.70
    min_size: float = 1.0
    max_order_cost: float = 2.0
    max_trades: int = 10
    hold_seconds: float = 5.0
    cooldown_seconds: float = 10.0
    fee_rate: float = 0.07
    slippage_per_contract: float = 0.0
    use_recorded_volatility: bool = False
    min_fill_cost: float = 0.01
    max_files: int | None = None
    latest_first: bool = False


@dataclass(frozen=True)
class StrategyGateConfig:
    report: StrategyReportConfig
    min_trades: int = 30
    min_win_rate: float = 0.55
    min_pnl: float = 0.0
    max_open_positions: int = 0
    min_cf_messages: int = 0
    require_cf_final_window: bool = False


@dataclass(frozen=True)
class StrategyGateResult:
    passed: bool
    reason: str
    best_row: StrategyReportRow | None
    rows: list[StrategyReportRow]
    min_trades: int
    min_win_rate: float
    min_pnl: float
    max_open_positions: int
    min_cf_messages: int
    require_cf_final_window: bool


@dataclass(frozen=True)
class StrategyReportRow:
    min_edge: float
    liquidity_fraction: float
    queue_ahead_contracts: float
    files: int
    messages: int
    scans: int
    trades: int
    wins: int
    losses: int
    flat: int
    pnl: float
    avg_pnl: float | None
    median_pnl: float | None
    win_rate: float | None
    avg_entry_price: float | None
    avg_exit_price: float | None
    open_positions: int
    cf_messages: int = 0
    cf_final_window_messages: int = 0


def build_strategy_report(config: StrategyReportConfig) -> list[StrategyReportRow]:
    files = _recording_files(
        config.path,
        max_files=config.max_files,
        latest_first=config.latest_first,
    )
    cf_coverage = _cf_coverage(files)
    rows: list[StrategyReportRow] = []
    for edge in config.edges:
        for liquidity_fraction in config.liquidity_fractions:
            for queue_ahead in config.queue_ahead_contracts:
                results = [
                    replay_recording(
                        BacktestConfig(
                            path=path,
                            annual_volatility=config.annual_volatility,
                            min_edge=edge,
                            min_size=config.min_size,
                            max_order_cost=config.max_order_cost,
                            max_trades=config.max_trades,
                            hold_seconds=config.hold_seconds,
                            cooldown_seconds=config.cooldown_seconds,
                            fee_rate=config.fee_rate,
                            slippage_per_contract=config.slippage_per_contract,
                            use_recorded_volatility=config.use_recorded_volatility,
                            liquidity_fraction=liquidity_fraction,
                            queue_ahead_contracts=queue_ahead,
                            min_fill_cost=config.min_fill_cost,
                        )
                    )
                    for path in files
                ]
                rows.append(_report_row(edge, liquidity_fraction, queue_ahead, files, results, cf_coverage))
    return sorted(rows, key=lambda row: (row.pnl, row.trades), reverse=True)


def report_payload(rows: list[StrategyReportRow]) -> list[dict[str, object]]:
    return [asdict(row) for row in rows]


def build_strategy_gate(config: StrategyGateConfig) -> StrategyGateResult:
    rows = build_strategy_report(config.report)
    for row in rows:
        if _row_passes_gate(row, config):
            return StrategyGateResult(
                passed=True,
                reason="candidate_passed",
                best_row=row,
                rows=rows,
                min_trades=config.min_trades,
                min_win_rate=config.min_win_rate,
                min_pnl=config.min_pnl,
                max_open_positions=config.max_open_positions,
                min_cf_messages=config.min_cf_messages,
                require_cf_final_window=config.require_cf_final_window,
            )
    return StrategyGateResult(
        passed=False,
        reason=_gate_block_reason(rows, config),
        best_row=rows[0] if rows else None,
        rows=rows,
        min_trades=config.min_trades,
        min_win_rate=config.min_win_rate,
        min_pnl=config.min_pnl,
        max_open_positions=config.max_open_positions,
        min_cf_messages=config.min_cf_messages,
        require_cf_final_window=config.require_cf_final_window,
    )


def gate_payload(result: StrategyGateResult) -> dict[str, object]:
    return {
        "passed": result.passed,
        "reason": result.reason,
        "best_row": asdict(result.best_row) if result.best_row is not None else None,
        "rows": report_payload(result.rows),
        "constraints": {
            "min_trades": result.min_trades,
            "min_win_rate": result.min_win_rate,
            "min_pnl": result.min_pnl,
            "max_open_positions": result.max_open_positions,
            "min_cf_messages": result.min_cf_messages,
            "require_cf_final_window": result.require_cf_final_window,
        },
    }


def _recording_files(path: Path, *, max_files: int | None = None, latest_first: bool = False) -> list[Path]:
    if path.is_file():
        return [path]
    files = [candidate for candidate in path.rglob("*.jsonl") if candidate.is_file()]
    if latest_first:
        files = sorted(files, key=lambda candidate: candidate.stat().st_mtime, reverse=True)
    else:
        files = sorted(files)
    if max_files is not None and max_files > 0:
        files = files[:max_files]
    return files


def _row_passes_gate(row: StrategyReportRow, config: StrategyGateConfig) -> bool:
    return (
        row.trades >= config.min_trades
        and row.pnl >= config.min_pnl
        and row.open_positions <= config.max_open_positions
        and row.cf_messages >= config.min_cf_messages
        and (not config.require_cf_final_window or row.cf_final_window_messages > 0)
        and row.win_rate is not None
        and row.win_rate >= config.min_win_rate
    )


def _gate_block_reason(rows: list[StrategyReportRow], config: StrategyGateConfig) -> str:
    if not rows:
        return "no_recordings"
    best = rows[0]
    if best.open_positions > config.max_open_positions:
        return "open_positions"
    if best.cf_messages < config.min_cf_messages:
        return "insufficient_cf_coverage"
    if config.require_cf_final_window and best.cf_final_window_messages <= 0:
        return "missing_cf_final_window"
    if best.trades < config.min_trades:
        return "not_enough_trades"
    if best.pnl < config.min_pnl:
        return "negative_pnl"
    if best.win_rate is None or best.win_rate < config.min_win_rate:
        return "low_win_rate"
    return "no_candidate"


def _report_row(
    edge: float,
    liquidity_fraction: float,
    queue_ahead: float,
    files: list[Path],
    results: list[BacktestResult],
    cf_coverage: tuple[int, int],
) -> StrategyReportRow:
    trades = [trade for result in results for trade in result.trades]
    wins = sum(1 for trade in trades if trade.pnl > 0)
    losses = sum(1 for trade in trades if trade.pnl < 0)
    flat = sum(1 for trade in trades if trade.pnl == 0)
    pnl = sum(trade.pnl for trade in trades)
    return StrategyReportRow(
        min_edge=edge,
        liquidity_fraction=liquidity_fraction,
        queue_ahead_contracts=queue_ahead,
        files=len(files),
        messages=sum(result.messages for result in results),
        scans=sum(result.scans for result in results),
        trades=len(trades),
        wins=wins,
        losses=losses,
        flat=flat,
        pnl=pnl,
        avg_pnl=(pnl / len(trades)) if trades else None,
        median_pnl=_median([trade.pnl for trade in trades]) if trades else None,
        win_rate=(wins / len(trades)) if trades else None,
        avg_entry_price=_trade_mean(trades, "entry_price"),
        avg_exit_price=_trade_mean(trades, "exit_price"),
        open_positions=sum(result.open_positions for result in results),
        cf_messages=cf_coverage[0],
        cf_final_window_messages=cf_coverage[1],
    )


def _cf_coverage(files: list[Path]) -> tuple[int, int]:
    cf_messages = 0
    final_window_messages = 0
    for path in files:
        for line in path.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            try:
                row = json.loads(line)
            except ValueError:
                continue
            if row.get("event") != "cf":
                continue
            cf_messages += 1
            tick = row.get("tick") or {}
            state = row.get("settlement_state") or {}
            if tick.get("final_window_size") is not None or int(state.get("observed_count") or 0) > 0:
                final_window_messages += 1
    return cf_messages, final_window_messages


def _trade_mean(trades: list[ReplayTrade], field: str) -> float | None:
    if not trades:
        return None
    return mean(float(getattr(trade, field)) for trade in trades)


def _median(values: list[float]) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / 2.0
