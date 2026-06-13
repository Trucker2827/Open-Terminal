"""Real market-data loader backed by the downloaded qlib US dataset.

This replaces the np.random synthetic-data paths in the RL / Advanced-Models /
Meta-Learning tools with genuine OHLCV + indicators from qlib. If the dataset
is missing or a ticker has no data, the loader returns None / raises — callers
must then fail honestly (never fabricate).

Verified pipeline shapes (AAPL, 500-day lookback):
    load_ohlcv      -> DataFrame (~486, 10): open/high/low/close/volume +
                       returns/volatility/rsi/macd/signal, date-indexed.
    load_sequences  -> (X[n, seq, n_features], y[n]) forward-return label.
    load_matrix     -> (X[n, n_features], y[n]).

Dataset coverage is 1999-12-31 .. 2020-11-10, so dates are taken RELATIVE to the
qlib calendar (last N trading days), never hardcoded recent dates.
"""
from __future__ import annotations

import os
from typing import Optional, Tuple

import numpy as np
import pandas as pd

QLIB_PROVIDER_URI = os.path.expanduser("~/.qlib/qlib_data/us_data")

_FEATURE_COLS = ["open", "high", "low", "close", "volume",
                 "returns", "volatility", "rsi", "macd", "signal"]

_qlib_ready = False


def qlib_available() -> bool:
    """True if the qlib US dataset is present on disk."""
    return os.path.isdir(os.path.join(QLIB_PROVIDER_URI, "instruments"))


def _ensure_qlib() -> bool:
    """Idempotently initialise qlib against the US dataset. Returns False if the
    dataset is missing or qlib is not importable."""
    global _qlib_ready
    if _qlib_ready:
        return True
    if not qlib_available():
        return False
    try:
        import qlib
        from qlib.constant import REG_US
        qlib.init(provider_uri=QLIB_PROVIDER_URI, region=REG_US)
        _qlib_ready = True
        return True
    except Exception:
        return False


def load_ohlcv(ticker: str, lookback_days: int = 500) -> Optional[pd.DataFrame]:
    """Real OHLCV + indicators for one ticker over the last `lookback_days`
    trading days in the qlib calendar. Returns None if unavailable/empty."""
    if not _ensure_qlib():
        return None
    from qlib.data import D
    sym = (ticker or "").strip().upper()
    if not sym:
        return None
    try:
        cal = D.calendar(freq="day")
        if len(cal) == 0:
            return None
        n = min(max(int(lookback_days), 60) + 40, len(cal))  # +40 to absorb indicator warm-up
        start, end = str(cal[-n])[:10], str(cal[-1])[:10]
        df = D.features([sym], ["$open", "$high", "$low", "$close", "$volume"],
                        start_time=start, end_time=end, freq="day")
    except Exception:
        return None
    if df is None or df.empty:
        return None
    df = df.droplevel(0)
    df.columns = ["open", "high", "low", "close", "volume"]
    df["returns"] = df["close"].pct_change()
    df["volatility"] = df["returns"].rolling(10).std()
    delta = df["close"].diff()
    up = delta.clip(lower=0).rolling(14).mean()
    dn = (-delta.clip(upper=0)).rolling(14).mean()
    df["rsi"] = 100 - 100 / (1 + up / dn.replace(0, np.nan))
    ema12 = df["close"].ewm(span=12).mean()
    ema26 = df["close"].ewm(span=26).mean()
    df["macd"] = ema12 - ema26
    df["signal"] = df["macd"].ewm(span=9).mean()
    df = df.replace([np.inf, -np.inf], np.nan).dropna()
    return df if not df.empty else None


def _forward_return(close: pd.Series) -> np.ndarray:
    """Next-day return label aligned to each row (last row dropped)."""
    return (close.shift(-1) / close - 1.0).values


def load_sequences(ticker: str, seq_length: int = 20, n_features: int = 10,
                   lookback_days: int = 500) -> Optional[Tuple[np.ndarray, np.ndarray]]:
    """Sliding-window sequences for the Advanced-Models LSTM/Transformer path.
    X: (n, seq_length, n_features), y: (n,) next-day return. None if no data."""
    df = load_ohlcv(ticker, lookback_days)
    if df is None or len(df) <= seq_length + 1:
        return None
    cols = _FEATURE_COLS[:max(1, int(n_features))]
    if len(cols) < n_features:  # pad with close if caller wants more features than we have
        cols = (cols + ["close"] * n_features)[:n_features]
    feats = df[cols].values.astype(np.float32)
    fwd = _forward_return(df["close"]).astype(np.float32)
    X, y = [], []
    for i in range(len(feats) - seq_length - 1):
        X.append(feats[i:i + seq_length])
        y.append(fwd[i + seq_length - 1])
    if not X:
        return None
    return np.asarray(X, dtype=np.float32), np.asarray(y, dtype=np.float32).reshape(-1, 1)


def load_matrix(ticker: str, lookback_days: int = 500,
                classification: bool = False) -> Optional[Tuple[np.ndarray, np.ndarray]]:
    """Flat feature matrix for the Meta-Learning path.
    X: (n, n_features), y: (n,) next-day return (regression) or up/down (classification)."""
    df = load_ohlcv(ticker, lookback_days)
    if df is None or len(df) < 30:
        return None
    feats = df[_FEATURE_COLS].values.astype(np.float64)
    fwd = _forward_return(df["close"])
    # drop the last row (no forward label)
    X = feats[:-1]
    y = fwd[:-1]
    mask = ~np.isnan(y)
    X, y = X[mask], y[mask]
    if len(X) < 30:
        return None
    if classification:
        y = (y > 0).astype(int)
    return X, y
