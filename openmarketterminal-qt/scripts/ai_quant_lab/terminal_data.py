#!/usr/bin/env python3
"""Layer 1 of the terminal<->Quant Lab bridge: OFFLINE reads.

Read-only access to what the terminal already persists — no running app
required:
  - spot tick history (edge_prediction_raw_ticks: BTC/ETH/SOL/... , 1-min
    aggregate, coinbase+kraken sources, same series the vol estimator uses)
  - the resolved edge decision journal
  - kalshi evidence / calibrator JSON files

Also provides resolve_terminal_series(): the shared `{"source":"terminal"}`
hook the Analytics wrappers call so any series-taking operation can run on
the terminal's own data instead of hand-pasted arrays or yfinance.

Env overrides (tests): OPENTERMINAL_DATA_DB, OPENTERMINAL_EVIDENCE_DIR.
"""
import json
import math
import os
import sqlite3
import time

DEFAULT_DB = os.path.expanduser(
    "~/Library/Application Support/org.openterminal.OpenTerminal/data/openmarketterminal.db")
DEFAULT_EVIDENCE_DIR = os.path.expanduser(
    "~/Library/Application Support/Open Terminal/Open Terminal")


def _db_path():
    return os.environ.get("OPENTERMINAL_DATA_DB", DEFAULT_DB)


def _evidence_dir():
    return os.environ.get("OPENTERMINAL_EVIDENCE_DIR", DEFAULT_EVIDENCE_DIR)


def normalize_symbol(symbol):
    """Tick storage uses bare uppercase base symbols (BTC, not btc-usd)."""
    s = str(symbol or "").strip().upper()
    for suffix in ("-USD", "-USDT", "/USD", "/USDT", "USD"):
        if s.endswith(suffix) and len(s) > len(suffix):
            s = s[: -len(suffix)]
            break
    return s


def spot_series(symbol, window_hours=24, resample_sec=60, now_ms=None):
    """[(ts_ms, price)] resampled from the stored tick history, ascending.

    Mirrors EdgePredictionModelRepository::list_spot_price_series_since:
    average price per bucket, coinbase/kraken sources only.
    """
    now_ms = now_ms if now_ms is not None else int(time.time() * 1000)
    since_ms = now_ms - int(window_hours * 3600 * 1000)
    bucket_ms = max(1, int(resample_sec)) * 1000
    conn = sqlite3.connect(f"file:{_db_path()}?mode=ro", uri=True)
    try:
        rows = conn.execute(
            "SELECT (exchange_ts/?)*? AS bucket, AVG(price) FROM edge_prediction_raw_ticks"
            " WHERE symbol=? AND exchange_ts>=? AND exchange_ts>0 AND price>0"
            " AND (LOWER(source) LIKE '%coinbase%' OR LOWER(source) LIKE '%kraken%')"
            " GROUP BY bucket ORDER BY bucket ASC",
            (bucket_ms, bucket_ms, normalize_symbol(symbol), since_ms)).fetchall()
        return [(int(ts), float(price)) for ts, price in rows]
    finally:
        conn.close()


def spot_returns(symbol, window_hours=24, resample_sec=60, now_ms=None, log_returns=True):
    """Per-bucket returns from spot_series (log by default)."""
    series = spot_series(symbol, window_hours, resample_sec, now_ms)
    out = []
    for (_, prev), (ts, cur) in zip(series, series[1:]):
        if prev > 0 and cur > 0:
            out.append((ts, math.log(cur / prev) if log_returns else cur / prev - 1.0))
    return out


def available_symbols(min_ticks=1000):
    conn = sqlite3.connect(f"file:{_db_path()}?mode=ro", uri=True)
    try:
        rows = conn.execute(
            "SELECT symbol, COUNT(*) FROM edge_prediction_raw_ticks"
            " GROUP BY symbol HAVING COUNT(*) >= ? ORDER BY 2 DESC", (min_ticks,)).fetchall()
        return [{"symbol": s, "ticks": int(n)} for s, n in rows]
    finally:
        conn.close()


def kalshi_evidence():
    """The daemon's decision-snapshot file, or None when absent/unreadable."""
    try:
        with open(os.path.join(_evidence_dir(), "kalshi-ws-books.json"), encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def calibrator_report():
    try:
        with open(os.path.join(_evidence_dir(), "calibrator.json"), encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def resolve_terminal_series(data):
    """The shared `{"source":"terminal"}` hook for the Analytics wrappers.

    When `data` requests the terminal source and carries no explicit values,
    fill data["values"] from the stored tick history:
        {"source": "terminal", "symbol": "BTC-USD",
         "window_hours": 24, "resample_sec": 60, "series": "price"|"returns"}
    Mutates and returns `data`. Raises ValueError with an honest message when
    the terminal has no usable series — never fabricates.
    """
    if data.get("source") != "terminal" or data.get("values"):
        return data
    symbol = data.get("symbol")
    if not symbol:
        raise ValueError("source=terminal requires 'symbol' (e.g. BTC-USD)")
    window_hours = float(data.get("window_hours", 24))
    resample_sec = int(data.get("resample_sec", 60))
    kind = data.get("series", "price")
    if kind == "returns":
        points = spot_returns(symbol, window_hours, resample_sec)
    else:
        points = spot_series(symbol, window_hours, resample_sec)
    if len(points) < 8:
        raise ValueError(
            f"terminal has only {len(points)} points for {normalize_symbol(symbol)} "
            f"over {window_hours}h — is the daemon collecting ticks for this symbol?")
    data["values"] = [v for _, v in points]
    data["_terminal_series"] = {"symbol": normalize_symbol(symbol), "kind": kind,
                                "points": len(points), "resample_sec": resample_sec,
                                "window_hours": window_hours}
    return data
