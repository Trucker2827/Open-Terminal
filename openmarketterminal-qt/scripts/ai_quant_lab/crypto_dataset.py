#!/usr/bin/env python3
"""Layer 3 of the terminal<->Quant Lab bridge: a qlib dataset from OWN ticks.

Converts the terminal's stored spot tick history (edge_prediction_raw_ticks,
coinbase+kraken sources — the exact venues the engine trades) into a
qlib-format bar dataset, so the whole ML stack (train / IC / screen) runs on
the terminal's own crypto data instead of the bundled US equities snapshot.

Format written (verified against qlib 0.9.7 file_storage.py):
  <out>/calendars/<freq>.txt              one timestamp per line
  <out>/instruments/all.txt               SYM<TAB>start<TAB>end
  <out>/features/<sym>/<field>.<freq>.bin float32 [start_index, v0, v1, ...]

Fields: open/high/low/close from real ticks, factor=1.0 (no splits in spot
crypto), change=close/prev-1. volume and vwap are written as ALL-NaN — the
tick store carries no size data and fabricating volume would poison
volume-based features, but a MISSING field file crashes qlib's expression
engine (empty array vs full-length broadcast), while a NaN-filled one
degrades those features to NaN, which tree models tolerate and the
dense-model Fillna pipeline absorbs.

Commands:
  build '{"days":14,"freq_sec":60,"symbols":["BTC","ETH"],"out":"~/.qlib/qlib_data/crypto_data"}'
  info  '{"out":"~/.qlib/qlib_data/crypto_data"}'
"""
import json
import math
import os
import sqlite3
import struct
import sys
import time
from datetime import datetime, timezone

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from openterminal_paths import journal_db as _db_path

DEFAULT_OUT = os.path.expanduser("~/.qlib/qlib_data/crypto_data")
FIELDS = ("open", "high", "low", "close", "factor", "change")
NAN_FIELDS = ("volume", "vwap")   # required by Alpha-handler expressions; see module docstring


def freq_name(freq_sec):
    if freq_sec % 86400 == 0:
        return "day"
    if freq_sec % 60 == 0:
        return f"{freq_sec // 60}min"
    return f"{freq_sec}s"


def ts_text(bucket_ms, freq_sec):
    dt = datetime.fromtimestamp(bucket_ms / 1000.0, tz=timezone.utc)
    if freq_sec % 86400 == 0:
        return dt.strftime("%Y-%m-%d")
    return dt.strftime("%Y-%m-%d %H:%M:%S")


def stream_ohlc(symbol, since_ms, bucket_ms, db_path=None):
    """{bucket_ms: [open, high, low, close]} streamed from the tick store."""
    conn = sqlite3.connect(f"file:{db_path or _db_path()}?mode=ro", uri=True)
    try:
        cursor = conn.execute(
            "SELECT exchange_ts, price FROM edge_prediction_raw_ticks"
            " WHERE symbol=? AND exchange_ts>=? AND exchange_ts>0 AND price>0"
            " AND (LOWER(source) LIKE '%coinbase%' OR LOWER(source) LIKE '%kraken%')"
            " ORDER BY exchange_ts ASC", (symbol, since_ms))
        buckets = {}
        while True:
            rows = cursor.fetchmany(50_000)
            if not rows:
                break
            for ts, price in rows:
                b = (ts // bucket_ms) * bucket_ms
                bar = buckets.get(b)
                if bar is None:
                    buckets[b] = [price, price, price, price]
                else:
                    if price > bar[1]:
                        bar[1] = price
                    if price < bar[2]:
                        bar[2] = price
                    bar[3] = price
        return buckets
    finally:
        conn.close()


def tick_symbols(min_ticks, since_ms, db_path=None):
    conn = sqlite3.connect(f"file:{db_path or _db_path()}?mode=ro", uri=True)
    try:
        rows = conn.execute(
            "SELECT symbol, COUNT(*) FROM edge_prediction_raw_ticks"
            " WHERE exchange_ts>=? GROUP BY symbol HAVING COUNT(*) >= ? ORDER BY 2 DESC",
            (since_ms, min_ticks)).fetchall()
        return [s for s, _ in rows]
    finally:
        conn.close()


def write_bin(path, start_index, values):
    with open(path, "wb") as fh:
        fh.write(struct.pack("<f", float(start_index)))
        fh.write(struct.pack(f"<{len(values)}f", *values))


def build(params):
    days = float(params.get("days", 14))
    freq_sec = int(params.get("freq_sec", 60))
    out = os.path.expanduser(params.get("out", DEFAULT_OUT))
    min_ticks = int(params.get("min_ticks", 5000))
    now_ms = int(params.get("now_ms", time.time() * 1000))
    since_ms = now_ms - int(days * 86400 * 1000)
    bucket_ms = freq_sec * 1000
    freq = freq_name(freq_sec)

    symbols = params.get("symbols") or tick_symbols(min_ticks, since_ms)
    symbols = [str(s).upper() for s in symbols]
    if not symbols:
        return {"success": False,
                "error": f"no symbols with >= {min_ticks} ticks in the last {days} days"}

    per_symbol = {}
    for symbol in symbols:
        ohlc = stream_ohlc(symbol, since_ms, bucket_ms)
        if len(ohlc) >= 8:
            per_symbol[symbol] = ohlc
    if not per_symbol:
        return {"success": False, "error": "no symbol produced enough bars"}

    calendar = sorted(set().union(*per_symbol.values()))
    cal_index = {b: i for i, b in enumerate(calendar)}

    os.makedirs(os.path.join(out, "calendars"), exist_ok=True)
    os.makedirs(os.path.join(out, "instruments"), exist_ok=True)
    with open(os.path.join(out, "calendars", freq + ".txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(ts_text(b, freq_sec) for b in calendar) + "\n")

    instrument_rows = []
    report = {}
    nan = float("nan")
    for symbol, ohlc in sorted(per_symbol.items()):
        buckets = sorted(ohlc)
        first, last = cal_index[buckets[0]], cal_index[buckets[-1]]
        span = last - first + 1
        series = {f: [nan] * span for f in FIELDS}
        prev_close = None
        for i in range(span):
            bar = ohlc.get(calendar[first + i])
            if bar is None:
                continue
            o, h, l, c = bar
            series["open"][i] = o
            series["high"][i] = h
            series["low"][i] = l
            series["close"][i] = c
            series["factor"][i] = 1.0
            if prev_close and prev_close > 0:
                series["change"][i] = c / prev_close - 1.0
            prev_close = c
        sym_dir = os.path.join(out, "features", symbol.lower())
        os.makedirs(sym_dir, exist_ok=True)
        for field in FIELDS:
            write_bin(os.path.join(sym_dir, f"{field}.{freq}.bin"), first, series[field])
        for field in NAN_FIELDS:
            write_bin(os.path.join(sym_dir, f"{field}.{freq}.bin"), first, [nan] * span)
        instrument_rows.append(
            f"{symbol}\t{ts_text(calendar[first], freq_sec)}\t{ts_text(calendar[last], freq_sec)}")
        gap_pct = 100.0 * (1.0 - len(buckets) / span)
        report[symbol] = {"bars": len(buckets), "span": span,
                          "gap_pct": round(gap_pct, 2)}
    with open(os.path.join(out, "instruments", "all.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(instrument_rows) + "\n")

    return {"success": True, "out": out, "freq": freq, "calendar_bars": len(calendar),
            "range": {"start": ts_text(calendar[0], freq_sec),
                      "end": ts_text(calendar[-1], freq_sec)},
            "symbols": report,
            "note": ("volume/vwap written as NaN — the tick store carries no size data; "
                     "volume-based features degrade to NaN rather than being fabricated")}


def info(params):
    out = os.path.expanduser(params.get("out", DEFAULT_OUT))
    cal_dir = os.path.join(out, "calendars")
    if not os.path.isdir(cal_dir):
        return {"success": False, "error": f"no dataset at {out} — run build first"}
    freqs = {}
    for name in os.listdir(cal_dir):
        with open(os.path.join(cal_dir, name), encoding="utf-8") as fh:
            lines = [l for l in fh.read().splitlines() if l.strip()]
        freqs[name.replace(".txt", "")] = {
            "bars": len(lines),
            "start": lines[0] if lines else None, "end": lines[-1] if lines else None}
    try:
        with open(os.path.join(out, "instruments", "all.txt"), encoding="utf-8") as fh:
            instruments = [l.split("\t")[0] for l in fh.read().splitlines() if l.strip()]
    except OSError:
        instruments = []
    return {"success": True, "out": out, "freqs": freqs, "instruments": instruments}


def main(argv):
    if len(argv) < 2 or argv[1] not in ("build", "info"):
        print(json.dumps({"success": False,
                          "error": "usage: crypto_dataset.py build|info ['{json}']"}))
        return 2
    params = {}
    if len(argv) > 2:
        try:
            params = json.loads(argv[2])
        except ValueError as exc:
            print(json.dumps({"success": False, "error": f"invalid JSON: {exc}"}))
            return 2
    result = build(params) if argv[1] == "build" else info(params)
    print(json.dumps(result))
    return 0 if result.get("success") else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
