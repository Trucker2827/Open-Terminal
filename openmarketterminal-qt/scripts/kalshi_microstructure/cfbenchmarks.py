from __future__ import annotations

import asyncio
import json
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Iterable

from .auth import KalshiCredentials
from .ws import KalshiWebSocketClient


CF_INDEX_BY_SYMBOL = {
    "BTC-USD": "BRTI",
    "ETH-USD": "ETHUSD_RTI",
    "SOL-USD": "SOLUSD_RTI",
    "DOGE-USD": "DOGEUSD_RTI",
}


@dataclass(frozen=True)
class CFValueTick:
    index: str
    value: float
    ts_ms: int
    received_at: datetime
    source: str = "kalshi"
    avg_60s_value: float | None = None
    avg_60s_window_size: int | None = None
    final_window_value: float | None = None
    final_window_size: int | None = None

    @property
    def ts(self) -> datetime:
        return datetime.fromtimestamp(self.ts_ms / 1000.0, tz=timezone.utc)

    @property
    def second_bucket_ms(self) -> int:
        return self.ts_ms // 1000 * 1000


@dataclass(frozen=True)
class SettlementWindowState:
    index: str
    threshold: float
    window_start: datetime
    window_end: datetime
    observed_count: int
    target_count: int
    observed_average: float | None
    projected_average: float | None
    seconds_remaining: float
    required_remaining_average_for_yes: float | None
    required_remaining_average_for_no: float | None
    locked_side: str | None
    latest_value: float | None
    latest_ts: datetime | None

    @property
    def remaining_count(self) -> int:
        return max(0, self.target_count - self.observed_count)


@dataclass
class CFValueCache:
    index: str
    max_ticks: int = 600
    ticks_by_second: dict[int, CFValueTick] = field(default_factory=dict)

    def update(self, tick: CFValueTick) -> None:
        if tick.index != self.index:
            return
        self.ticks_by_second[tick.second_bucket_ms] = tick
        if len(self.ticks_by_second) > self.max_ticks:
            for key in sorted(self.ticks_by_second)[: len(self.ticks_by_second) - self.max_ticks]:
                self.ticks_by_second.pop(key, None)

    @property
    def latest(self) -> CFValueTick | None:
        if not self.ticks_by_second:
            return None
        return self.ticks_by_second[max(self.ticks_by_second)]

    @property
    def tick_count(self) -> int:
        return len(self.ticks_by_second)

    def ticks_between(self, start: datetime, end: datetime) -> list[CFValueTick]:
        start_ms = int(start.timestamp() * 1000)
        end_ms = int(end.timestamp() * 1000)
        return [
            tick
            for bucket, tick in sorted(self.ticks_by_second.items())
            if start_ms <= bucket < end_ms
        ]

    def settlement_state(
        self,
        *,
        threshold: float,
        window_end: datetime,
        now: datetime | None = None,
        window_seconds: int = 60,
        target_count: int = 60,
    ) -> SettlementWindowState:
        now = (now or datetime.now(timezone.utc)).astimezone(timezone.utc)
        window_end = window_end.astimezone(timezone.utc)
        window_start = datetime.fromtimestamp(window_end.timestamp() - window_seconds, tz=timezone.utc)
        latest = self.latest
        ticks = self.ticks_between(window_start, min(now, window_end))
        values = [tick.value for tick in ticks]
        if latest is not None and latest.final_window_value is not None and latest.final_window_size is not None:
            observed_count = min(target_count, latest.final_window_size)
            observed_sum = latest.final_window_value * observed_count
        else:
            observed_count = len(values)
            observed_sum = sum(values)
        observed_average = observed_sum / observed_count if observed_count else None
        remaining_count = max(0, target_count - observed_count)
        seconds_remaining = max(0.0, (window_end - now).total_seconds())

        required_yes = _required_remaining_average(
            threshold=threshold,
            observed_sum=observed_sum,
            target_count=target_count,
            remaining_count=remaining_count,
        )
        projected_average = None
        if latest is not None:
            projected_average = (observed_sum + latest.value * remaining_count) / target_count
        locked_side = _locked_side(required_yes, remaining_count)
        required_no = required_yes
        if observed_count >= target_count:
            final_average = observed_sum / target_count if target_count else None
            projected_average = final_average
            locked_side = _winner(final_average, threshold)
            required_yes = None
            required_no = None

        return SettlementWindowState(
            index=self.index,
            threshold=threshold,
            window_start=window_start,
            window_end=window_end,
            observed_count=observed_count,
            target_count=target_count,
            observed_average=observed_average,
            projected_average=projected_average,
            seconds_remaining=seconds_remaining,
            required_remaining_average_for_yes=required_yes,
            required_remaining_average_for_no=required_no,
            locked_side=locked_side,
            latest_value=latest.value if latest else None,
            latest_ts=latest.ts if latest else None,
        )


async def maintain_kalshi_cf_cache(
    cache: CFValueCache,
    credentials: KalshiCredentials,
    *,
    env: str = "prod",
) -> None:
    while True:
        try:
            async for tick in stream_kalshi_cf_values(credentials, env=env, indices=[cache.index]):
                cache.update(tick)
        except asyncio.CancelledError:
            raise
        except Exception:
            await asyncio.sleep(1.0)


def parse_cf_value_message(message: dict[str, Any], *, source: str = "kalshi") -> CFValueTick | None:
    msg_type = str(message.get("type") or "")
    if msg_type == "value":
        payload = message
        index = payload.get("id")
        avg_60s = {}
        final_window = {}
    elif msg_type == "cfbenchmarks_value":
        payload = message.get("msg") or {}
        raw_data = payload.get("data")
        if isinstance(raw_data, str):
            try:
                data = json.loads(raw_data)
            except json.JSONDecodeError:
                data = {}
            if isinstance(data, dict) and data.get("type") == "value":
                payload = {**data, **payload}
                index = data.get("id") or payload.get("index_id")
            else:
                index = payload.get("index_id") or payload.get("id") or payload.get("index") or payload.get("ticker")
        else:
            index = payload.get("index_id") or payload.get("id") or payload.get("index") or payload.get("ticker")
        avg_60s = payload.get("avg_60s_data") or {}
        final_window = payload.get("last_60s_windowed_average_15min") or {}
    else:
        return None

    value = payload.get("value") or payload.get("price")
    raw_time = payload.get("time") or payload.get("ts_ms") or payload.get("timestamp_ms")
    if index is None or value is None or raw_time is None:
        return None
    return CFValueTick(
        index=str(index),
        value=float(value),
        ts_ms=int(raw_time),
        received_at=datetime.now(timezone.utc),
        source=source,
        avg_60s_value=_optional_float(avg_60s.get("value")),
        avg_60s_window_size=_optional_int(avg_60s.get("window_size")),
        final_window_value=_optional_float(final_window.get("value")),
        final_window_size=_optional_int(final_window.get("window_size")),
    )


async def stream_kalshi_cf_values(
    credentials: KalshiCredentials,
    *,
    env: str = "prod",
    indices: Iterable[str],
):
    client = KalshiWebSocketClient(credentials=credentials, env=env)
    async for message in client.stream_raw(
        channels=["cfbenchmarks_value"],
        extra_params={"index_ids": list(indices)},
    ):
        tick = parse_cf_value_message(message, source="kalshi")
        if tick is not None:
            yield tick


async def sample_kalshi_cf_values(
    credentials: KalshiCredentials,
    *,
    env: str = "prod",
    indices: Iterable[str],
    seconds: float = 10.0,
) -> list[CFValueTick]:
    ticks: list[CFValueTick] = []
    try:
        async with asyncio.timeout(seconds):
            async for tick in stream_kalshi_cf_values(credentials, env=env, indices=indices):
                ticks.append(tick)
    except TimeoutError:
        pass
    return ticks


def settlement_state_payload(state: SettlementWindowState) -> dict[str, Any]:
    return {
        "index": state.index,
        "threshold": state.threshold,
        "window_start": state.window_start.isoformat(),
        "window_end": state.window_end.isoformat(),
        "observed_count": state.observed_count,
        "remaining_count": state.remaining_count,
        "target_count": state.target_count,
        "observed_average": state.observed_average,
        "projected_average": state.projected_average,
        "seconds_remaining": state.seconds_remaining,
        "required_remaining_average_for_yes": state.required_remaining_average_for_yes,
        "required_remaining_average_for_no": state.required_remaining_average_for_no,
        "locked_side": state.locked_side,
        "latest_value": state.latest_value,
        "latest_ts": state.latest_ts.isoformat() if state.latest_ts else None,
    }


def tick_payload(tick: CFValueTick) -> dict[str, Any]:
    return {
        "index": tick.index,
        "value": tick.value,
        "ts_ms": tick.ts_ms,
        "ts": tick.ts.isoformat(),
        "received_at": tick.received_at.isoformat(),
        "source": tick.source,
        "avg_60s_value": tick.avg_60s_value,
        "avg_60s_window_size": tick.avg_60s_window_size,
        "final_window_value": tick.final_window_value,
        "final_window_size": tick.final_window_size,
    }


def dumps_json(payload: Any) -> str:
    return json.dumps(payload, indent=2, sort_keys=True)


def _required_remaining_average(
    *,
    threshold: float,
    observed_sum: float,
    target_count: int,
    remaining_count: int,
) -> float | None:
    if remaining_count <= 0:
        return None
    return (threshold * target_count - observed_sum) / remaining_count


def _locked_side(required_yes: float | None, remaining_count: int) -> str | None:
    if required_yes is None or remaining_count <= 0:
        return None
    if required_yes <= 0:
        return "YES"
    return None


def _winner(average: float | None, threshold: float) -> str | None:
    if average is None:
        return None
    return "YES" if average > threshold else "NO"


def _optional_float(value: Any) -> float | None:
    if value is None:
        return None
    return float(value)


def _optional_int(value: Any) -> int | None:
    if value is None:
        return None
    return int(value)
