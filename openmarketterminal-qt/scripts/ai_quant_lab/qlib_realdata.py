"""Real market-data loader for the RL / Advanced-Models / Meta-Learning tools.

Primary source is LIVE Yahoo Finance (yfinance) — current data, through today.
Fallback is the on-disk qlib US dataset (a frozen 1999..2020-11-10 snapshot) for
offline / rate-limited situations. Either way the data is REAL; if neither source
yields data the loader returns None and callers must fail honestly (never fabricate).

`last_source()` reports which source produced the most recent load ('yahoo' or
'qlib'), so results can be tagged truthfully.

Shapes (e.g. AAPL, 500-day lookback):
    load_ohlcv      -> DataFrame (~N, 10): open/high/low/close/volume +
                       returns/volatility/rsi/macd/signal, date-indexed.
    load_sequences  -> (X[n, seq, n_features], y[n, 1]) forward-return label.
    load_matrix     -> (X[n, n_features], y[n]).
"""
from __future__ import annotations

import os
from datetime import datetime, timedelta
from typing import Optional, Tuple

import numpy as np
import pandas as pd

QLIB_PROVIDER_URI = os.path.expanduser("~/.qlib/qlib_data/us_data")

_FEATURE_COLS = ["open", "high", "low", "close", "volume",
                 "returns", "volatility", "rsi", "macd", "signal"]

_qlib_ready = False
LAST_SOURCE: Optional[str] = None  # 'yahoo' | 'qlib' | None


def last_source() -> Optional[str]:
    """Which source produced the most recent successful load_ohlcv()."""
    return LAST_SOURCE


def _add_indicators(df: pd.DataFrame) -> Optional[pd.DataFrame]:
    """Given a date-indexed OHLCV frame (open/high/low/close/volume), append
    returns/volatility/rsi/macd/signal and drop warm-up NaNs. Shared by both
    sources so the feature definition is identical."""
    if df is None or df.empty:
        return None
    df = df.copy()
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
    return df[_FEATURE_COLS] if not df.empty else None


def _load_ohlcv_yahoo(ticker: str, lookback_days: int = 500) -> Optional[pd.DataFrame]:
    """LIVE current OHLCV from Yahoo Finance. None if unavailable/rate-limited."""
    try:
        import yfinance as yf
    except Exception:
        return None
    sym = (ticker or "").strip().upper()
    if not sym:
        return None
    # ~1.5x calendar days to cover weekends/holidays + indicator warm-up.
    period_days = int(max(lookback_days, 60) * 1.5) + 60
    try:
        df = yf.download(sym, period=f"{period_days}d", interval="1d",
                         progress=False, auto_adjust=True, threads=False)
    except Exception:
        return None
    if df is None or df.empty:
        return None
    if getattr(df.columns, "nlevels", 1) > 1:  # flatten yfinance multiindex columns
        df.columns = df.columns.get_level_values(0)
    rename = {"Open": "open", "High": "high", "Low": "low", "Close": "close", "Volume": "volume"}
    df = df.rename(columns=rename)
    need = ["open", "high", "low", "close", "volume"]
    if not all(c in df.columns for c in need):
        return None
    df = df[need].astype(float)
    return _add_indicators(df)


def qlib_available() -> bool:
    return os.path.isdir(os.path.join(QLIB_PROVIDER_URI, "instruments"))


def _ensure_qlib() -> bool:
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


def _load_ohlcv_qlib(ticker: str, lookback_days: int = 500) -> Optional[pd.DataFrame]:
    """OHLCV from the frozen on-disk qlib dataset (offline fallback)."""
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
        n = min(max(int(lookback_days), 60) + 40, len(cal))
        start, end = str(cal[-n])[:10], str(cal[-1])[:10]
        df = D.features([sym], ["$open", "$high", "$low", "$close", "$volume"],
                        start_time=start, end_time=end, freq="day")
    except Exception:
        return None
    if df is None or df.empty:
        return None
    df = df.droplevel(0)
    df.columns = ["open", "high", "low", "close", "volume"]
    return _add_indicators(df)


def load_ohlcv(ticker: str, lookback_days: int = 500) -> Optional[pd.DataFrame]:
    """Real OHLCV + indicators. Prefers LIVE Yahoo (current data); falls back to
    the frozen qlib dataset if Yahoo is unavailable. None if neither has data.
    Sets last_source() to 'yahoo' or 'qlib'."""
    global LAST_SOURCE
    df = _load_ohlcv_yahoo(ticker, lookback_days)
    if df is not None and not df.empty:
        LAST_SOURCE = "yahoo"
        return df
    df = _load_ohlcv_qlib(ticker, lookback_days)
    if df is not None and not df.empty:
        LAST_SOURCE = "qlib"
        return df
    LAST_SOURCE = None
    return None


def _forward_return(close: pd.Series) -> np.ndarray:
    return (close.shift(-1) / close - 1.0).values


def load_sequences(ticker: str, seq_length: int = 20, n_features: int = 10,
                   lookback_days: int = 500) -> Optional[Tuple[np.ndarray, np.ndarray]]:
    df = load_ohlcv(ticker, lookback_days)
    if df is None or len(df) <= seq_length + 1:
        return None
    cols = _FEATURE_COLS[:max(1, int(n_features))]
    if len(cols) < n_features:
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
    df = load_ohlcv(ticker, lookback_days)
    if df is None or len(df) < 30:
        return None
    feats = df[_FEATURE_COLS].values.astype(np.float64)
    fwd = _forward_return(df["close"])
    X = feats[:-1]
    y = fwd[:-1]
    mask = ~np.isnan(y)
    X, y = X[mask], y[mask]
    if len(X) < 30:
        return None
    if classification:
        y = (y > 0).astype(int)
    return X, y
