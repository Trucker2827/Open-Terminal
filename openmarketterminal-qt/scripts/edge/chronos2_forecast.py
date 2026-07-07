#!/usr/bin/env python3
"""Chronos-2 forecasting bridge for OpenTerminal edge books.

Input is a local CSV of timestamped prices. Output is one JSON object designed
for the CLI/daemon to journal and score. This script never places trades.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True)
class HorizonSpec:
    horizon: str
    freq: str
    prediction_length: int


def horizon_spec(raw: str) -> HorizonSpec:
    h = (raw or "15m").strip().lower().replace("_", "-")
    if h in {"5", "5m", "5-min", "5min"}:
        return HorizonSpec("5m", "1min", 5)
    if h in {"15", "15m", "15-min", "15min"}:
        return HorizonSpec("15m", "1min", 15)
    if h in {"60", "60m", "1h", "hour", "hourly"}:
        return HorizonSpec("1h", "5min", 12)
    if h in {"1d", "d", "day", "daily", "24h"}:
        return HorizonSpec("1d", "30min", 48)
    raise ValueError("--horizon must be 5m, 15m, 1h, or 1d")


def load_prices(path: str, spec: HorizonSpec, context_limit: int) -> Any:
    import pandas as pd

    df = pd.read_csv(path)
    if df.empty:
        raise ValueError("input CSV is empty")

    if "timestamp_ms" in df.columns:
        ts = pd.to_datetime(df["timestamp_ms"], unit="ms", utc=True)
    elif "received_ts" in df.columns:
        ts = pd.to_datetime(df["received_ts"], unit="ms", utc=True)
    elif "timestamp" in df.columns:
        ts = pd.to_datetime(df["timestamp"], utc=True)
    else:
        raise ValueError("CSV needs timestamp_ms, received_ts, or timestamp")

    price_col = "price" if "price" in df.columns else "close" if "close" in df.columns else None
    if price_col is None:
        raise ValueError("CSV needs price or close column")

    s = (
        pd.DataFrame({"timestamp": ts, "price": pd.to_numeric(df[price_col], errors="coerce")})
        .dropna()
        .sort_values("timestamp")
    )
    s = s[s["price"] > 0]
    if s.empty:
        raise ValueError("CSV has no positive prices")

    # Multiple sources can write the same minute. Median reduces one-source noise,
    # then ffill keeps a regular grid for Chronos.
    regular = (
        s.set_index("timestamp")["price"]
        .resample(spec.freq)
        .median()
        .ffill()
        .dropna()
        .tail(context_limit)
    )
    if len(regular) < max(12, spec.prediction_length * 3):
        raise ValueError(
            f"not enough regular history for {spec.horizon}: have {len(regular)}, "
            f"need at least {max(12, spec.prediction_length * 3)}"
        )
    out = regular.reset_index()
    if getattr(out["timestamp"].dt, "tz", None) is not None:
        out["timestamp"] = out["timestamp"].dt.tz_convert(None)
    out["item_id"] = "series"
    return out[["item_id", "timestamp", "price"]]


def choose_device(requested: str) -> str:
    if requested != "auto":
        return requested
    try:
        import torch

        if torch.cuda.is_available():
            return "cuda"
        if hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
            return "mps"
    except Exception:
        pass
    return "cpu"


def confidence_label(abs_return_bps: float, width_bps: float, samples: int) -> str:
    if samples < 100:
        return "low"
    if not math.isfinite(width_bps) or width_bps <= 0:
        return "low"
    ratio = abs_return_bps / max(width_bps, 1.0)
    if ratio >= 0.35 and width_bps < 120:
        return "high"
    if ratio >= 0.18 and width_bps < 250:
        return "medium"
    return "low"


def mock_forecast(context: Any, spec: HorizonSpec, symbol: str, model_id: str) -> dict[str, Any]:
    prices = context["price"].astype(float)
    last_price = float(prices.iloc[-1])
    first_price = float(prices.iloc[max(0, len(prices) - spec.prediction_length * 2)])
    drift = ((last_price / first_price) - 1.0) if first_price > 0 else 0.0
    predicted_return_bps = max(-250.0, min(250.0, drift * 10000.0 * 0.35))
    width_bps = max(20.0, min(400.0, abs(predicted_return_bps) * 2.0 + 35.0))
    q10 = predicted_return_bps - width_bps / 2.0
    q90 = predicted_return_bps + width_bps / 2.0
    return build_output(
        symbol=symbol,
        spec=spec,
        model_id=model_id + ":mock",
        device="mock",
        context=context,
        q10_return_bps=q10,
        q50_return_bps=predicted_return_bps,
        q90_return_bps=q90,
    )


def build_output(
    *,
    symbol: str,
    spec: HorizonSpec,
    model_id: str,
    device: str,
    context: Any,
    q10_return_bps: float,
    q50_return_bps: float,
    q90_return_bps: float,
) -> dict[str, Any]:
    last_price = float(context["price"].iloc[-1])
    width_bps = float(q90_return_bps - q10_return_bps)
    confidence = confidence_label(abs(q50_return_bps), width_bps, len(context))
    confidence_score = max(0.0, min(1.0, abs(q50_return_bps) / max(width_bps, 1.0)))
    direction = "up" if q50_return_bps > 0 else "down" if q50_return_bps < 0 else "flat"
    probability_up = 0.5 + max(-0.35, min(0.35, q50_return_bps / max(width_bps * 2.0, 50.0)))
    return {
        "success": True,
        "paper_only": True,
        "symbol": symbol,
        "horizon": spec.horizon,
        "model_id": model_id,
        "device": device,
        "freq": spec.freq,
        "prediction_length": spec.prediction_length,
        "sample_count": int(len(context)),
        "last_timestamp": context["timestamp"].iloc[-1].isoformat(),
        "last_price": last_price,
        "predicted_return_bps": float(q50_return_bps),
        "q10_return_bps": float(q10_return_bps),
        "q50_return_bps": float(q50_return_bps),
        "q90_return_bps": float(q90_return_bps),
        "interval_width_bps": width_bps,
        "direction": direction,
        "probability_up": float(max(0.01, min(0.99, probability_up))),
        "confidence": confidence,
        "confidence_score": float(confidence_score),
        "readiness": "paper_forecast",
        "notes": [
            "Chronos-2 is a forecasting book, not an execution engine.",
            "Use fee/spread/risk gates before any order decision.",
        ],
    }


def run_forecast(args: argparse.Namespace) -> dict[str, Any]:
    spec = horizon_spec(args.horizon)
    context = load_prices(args.input, spec, args.context_limit)
    if args.mock:
        return mock_forecast(context, spec, args.symbol, args.model)

    try:
        from chronos import BaseChronosPipeline
    except Exception as exc:
        return {
            "success": False,
            "error": f"chronos-forecasting is not installed: {exc}",
            "install": "python3.11 -m venv .venv-chronos && .venv-chronos/bin/pip install chronos-forecasting pandas",
        }

    device = choose_device(args.device)
    try:
        pipeline = BaseChronosPipeline.from_pretrained(args.model, device_map=device)
        pred_df = pipeline.predict_df(
            context,
            prediction_length=spec.prediction_length,
            quantile_levels=[0.1, 0.5, 0.9],
            id_column="item_id",
            timestamp_column="timestamp",
            target="price",
        )
    except Exception as exc:
        return {"success": False, "error": str(exc), "model_id": args.model, "device": device}

    final = pred_df.tail(1).iloc[0]
    last_price = float(context["price"].iloc[-1])

    def ret_bps(column: str) -> float:
        return (float(final[column]) / last_price - 1.0) * 10000.0

    return build_output(
        symbol=args.symbol,
        spec=spec,
        model_id=args.model,
        device=device,
        context=context,
        q10_return_bps=ret_bps("0.1"),
        q50_return_bps=ret_bps("0.5"),
        q90_return_bps=ret_bps("0.9"),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="OpenTerminal Chronos-2 forecast bridge")
    parser.add_argument("--input", required=True, help="CSV with timestamp_ms/received_ts/timestamp and price")
    parser.add_argument("--symbol", default="BTC-USD")
    parser.add_argument("--horizon", default="15m")
    parser.add_argument("--model", default="amazon/chronos-2")
    parser.add_argument("--device", default="auto", choices=["auto", "cpu", "mps", "cuda"])
    parser.add_argument("--context-limit", type=int, default=2048)
    parser.add_argument("--mock", action="store_true", help="Use deterministic local baseline for tests")
    args = parser.parse_args()

    try:
        result = run_forecast(args)
    except Exception as exc:
        result = {"success": False, "error": str(exc)}
    print(json.dumps(result, separators=(",", ":"), sort_keys=True))
    return 0 if result.get("success") else 3


if __name__ == "__main__":
    sys.exit(main())
