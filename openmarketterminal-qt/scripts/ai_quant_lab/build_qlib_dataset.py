"""Build a fresh, CURRENT qlib US dataset from live Yahoo Finance data.

The free qlib bundle is frozen at 2020-11-10. This rebuilds the qlib binary
dataset (calendars / instruments / features) from yfinance so every qlib-native
tool (factor discovery, model library, live signals, backtesting) sees data
through today — internally consistent (single adjustment basis), no boundary
discontinuity.

qlib .day.bin format (verified): little-endian float32, first value = the
calendar index of the instrument's first row, followed by one value per GLOBAL
calendar day from that index. Missing interior days are written as NaN.

Usage:
    python build_qlib_dataset.py --tickers AAPL,MSFT,... --target_dir /path --start 2005-01-01
    python build_qlib_dataset.py --universe sp500 --target_dir ~/.qlib/qlib_data/us_data_live
"""
from __future__ import annotations

import argparse
import os
import sys
from datetime import datetime

import numpy as np
import pandas as pd

FIELDS = ["open", "high", "low", "close", "volume", "factor", "change"]


def _download(tickers, start, end):
    import yfinance as yf
    frames = {}
    # batch in chunks to be gentle on the API
    CHUNK = 60
    for i in range(0, len(tickers), CHUNK):
        chunk = tickers[i:i + CHUNK]
        df = yf.download(chunk, start=start, end=end, interval="1d",
                         auto_adjust=True, progress=False, threads=True, group_by="ticker")
        for t in chunk:
            try:
                sub = df[t] if len(chunk) > 1 else df
            except Exception:
                continue
            if sub is None or sub.empty:
                continue
            sub = sub.rename(columns={"Open": "open", "High": "high", "Low": "low",
                                      "Close": "close", "Volume": "volume"})
            need = ["open", "high", "low", "close", "volume"]
            if not all(c in sub.columns for c in need):
                continue
            sub = sub[need].dropna(how="all")
            if not sub.empty:
                frames[t.upper()] = sub
        print(f"  downloaded {min(i+CHUNK, len(tickers))}/{len(tickers)}", file=sys.stderr)
    return frames


def _write_bin(path, start_index, values):
    arr = np.empty(len(values) + 1, dtype="<f4")
    arr[0] = np.float32(start_index)
    arr[1:] = np.asarray(values, dtype="<f4")
    arr.tofile(path)


def build(tickers, target_dir, start, end):
    target_dir = os.path.expanduser(target_dir)
    frames = _download(sorted(set(t.upper() for t in tickers)), start, end)
    if not frames:
        print("ERROR: no data downloaded", file=sys.stderr)
        return 1

    # Global calendar = sorted union of all dates across instruments.
    all_dates = sorted(set().union(*[set(df.index.normalize()) for df in frames.values()]))
    cal = [d.strftime("%Y-%m-%d") for d in all_dates]
    cal_pos = {d: i for i, d in enumerate(all_dates)}  # key by Timestamp (matches df.index)

    os.makedirs(os.path.join(target_dir, "calendars"), exist_ok=True)
    os.makedirs(os.path.join(target_dir, "instruments"), exist_ok=True)
    feat_root = os.path.join(target_dir, "features")
    os.makedirs(feat_root, exist_ok=True)

    with open(os.path.join(target_dir, "calendars", "day.txt"), "w") as f:
        f.write("\n".join(cal) + "\n")

    inst_lines = []
    for tkr, df in sorted(frames.items()):
        df = df.copy()
        df.index = df.index.normalize()
        df = df[~df.index.duplicated(keep="last")].sort_index()
        df["factor"] = 1.0  # auto_adjust already applied; stored prices are adjusted
        df["change"] = df["close"].pct_change()

        first_pos = cal_pos[df.index[0]]
        last_pos = cal_pos[df.index[-1]]
        # reindex to the contiguous global-calendar slice [first..last]; gaps -> NaN
        slice_dates = pd.to_datetime(cal[first_pos:last_pos + 1])
        df = df.reindex(slice_dates)

        fdir = os.path.join(feat_root, tkr.lower())
        os.makedirs(fdir, exist_ok=True)
        for field in FIELDS:
            _write_bin(os.path.join(fdir, f"{field}.day.bin"), first_pos, df[field].values)

        inst_lines.append(f"{tkr}\t{cal[first_pos]}\t{cal[last_pos]}")

    with open(os.path.join(target_dir, "instruments", "all.txt"), "w") as f:
        f.write("\n".join(inst_lines) + "\n")

    print(f"OK: {len(frames)} instruments, {len(cal)} calendar days "
          f"({cal[0]} .. {cal[-1]}) -> {target_dir}")
    return 0


SP500_FALLBACK = ("AAPL MSFT NVDA AMZN GOOGL GOOG META BRK-B TSLA JPM V WMT MA "
                  "JNJ PG HD CVX MRK ABBV COST PEP KO ADBE BAC CRM AVGO MCD ACN "
                  "TMO LIN ABT CSCO DHR WFC TXN DIS VZ NKE PM NEE INTC AMD QCOM "
                  "INTU UNH XOM ORCL IBM GE CAT").split()


def _universe(name):
    if name == "sp500":
        try:
            tables = pd.read_html("https://en.wikipedia.org/wiki/List_of_S%26P_500_companies")
            syms = tables[0]["Symbol"].astype(str).str.replace(".", "-", regex=False).tolist()
            if syms:
                return syms
        except Exception:
            pass
        return SP500_FALLBACK
    return SP500_FALLBACK


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tickers", default="")
    ap.add_argument("--universe", default="")
    ap.add_argument("--target_dir", required=True)
    ap.add_argument("--start", default="2005-01-01")
    ap.add_argument("--end", default=datetime.now().strftime("%Y-%m-%d"))
    a = ap.parse_args()
    if a.tickers:
        tickers = [t.strip() for t in a.tickers.split(",") if t.strip()]
    elif a.universe:
        tickers = _universe(a.universe)
    else:
        print("ERROR: pass --tickers or --universe", file=sys.stderr)
        return 2
    return build(tickers, a.target_dir, a.start, a.end)


if __name__ == "__main__":
    sys.exit(main())
