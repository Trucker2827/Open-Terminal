#!/usr/bin/env python3
"""Kalshi paper strategy-grid: a reproducible sweep over a pre-registered grid of
mechanical + signal-gated paper strategies, marked to the retained real book and
settled on real outcomes net of fees. Read-only; emits versioned evidence artifacts.
See docs/design/2026-07-30-kalshi-strategy-grid-design.md.

    python3 scripts/research/strategy_grid.py
"""
import bisect, collections, json, math, sys
import kalshi_edge_common as common

SCHEMA_VERSION = 1
SIDES = ("YES", "NO")
ENTRY_BANDS = ((0.02, 0.10), (0.10, 0.25), (0.25, 0.50),
               (0.50, 0.75), (0.75, 0.90), (0.90, 1.01))
GATES = ("mechanical", "physics", "calibrator")
TPS = (0.05, 0.10, 0.15, 0.20, 0.30)
SLS = (0.05, 0.10, 0.15, 0.20)
TRAILS = (0.05, 0.10, 0.15)
MIN_ENTRY_S_TO_CLOSE = 120

def _band_tag(lo, hi):
    return "b%d-%d" % (round(lo * 100), round(min(hi, 1.0) * 100))

def _exit_tag(ex):
    return ex["kind"] if ex["amount"] is None else "%s%d" % (ex["kind"], round(ex["amount"] * 100))

def variant_id(v):
    return "|".join((v["side"], _band_tag(v["band"][0], v["band"][1]),
                     v["gate"], _exit_tag(v["exit"])))

def _exits():
    out = [{"kind": "hold", "amount": None}]
    out += [{"kind": "tp", "amount": a} for a in TPS]
    out += [{"kind": "sl", "amount": a} for a in SLS]
    out += [{"kind": "trail", "amount": a} for a in TRAILS]
    return out

def build_grid():
    grid = []
    for side in SIDES:
        for lo, hi in ENTRY_BANDS:
            for gate in GATES:
                for ex in _exits():
                    v = {"side": side, "band": [lo, hi], "gate": gate, "exit": ex}
                    v["variant_id"] = variant_id(v)
                    grid.append(v)
    return grid

FEE = common.fee_per_contract

def bet_side_quotes(rows, side):
    """(ts, yes_bid, yes_ask) -> (ts, bet_bid, bet_ask). NO buys/sells the complement."""
    if side == "YES":
        return [(ts, b, a) for ts, b, a in rows]
    return [(ts, 1.0 - a, 1.0 - b) for ts, b, a in rows]   # NO: bid/ask flip & complement

def simulate_exit(path, i0, entry_ask, exit_rule, close_ms):
    """First (sell_bid, exit_ts) after i0 (before close) where the rule fires; taker at bid."""
    kind, amt = exit_rule["kind"], exit_rule["amount"]
    if kind == "hold":
        return None, None
    running_max = None
    for ts, bid, _ask in path[i0 + 1:]:
        if close_ms is not None and ts >= close_ms:
            break
        if kind == "tp" and bid >= entry_ask + amt:
            return bid, ts
        if kind == "sl" and bid <= entry_ask - amt:
            return bid, ts
        if kind == "trail":
            running_max = bid if running_max is None else max(running_max, bid)
            if bid <= running_max - amt:
                return bid, ts
    return None, None

def contract_pnl(path, i0, entry_ask, exit_rule, won, close_ms=None):
    hold_pnl = ((1.0 if won else 0.0) - entry_ask) - FEE(entry_ask)
    sell_bid, _ts = simulate_exit(path, i0, entry_ask, exit_rule, close_ms)
    if sell_bid is None:
        return {"pnl": hold_pnl, "exited": False, "hold_pnl": hold_pnl}
    pnl = (sell_bid - entry_ask) - FEE(entry_ask) - FEE(sell_bid)
    return {"pnl": pnl, "exited": True, "hold_pnl": hold_pnl}
