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

def find_entry(path, lo, hi, close_ms):
    for i, (ts, _bid, ask) in enumerate(path):
        if close_ms is not None and (close_ms - ts) / 1000.0 < MIN_ENTRY_S_TO_CLOSE:
            continue
        if lo <= ask < hi:
            return i, ask
    return None

def gate_ok(gate, ticker, ts_ms, side, signals):
    if gate == "mechanical":
        return True
    if gate == "physics":
        return signals.physics_ok(ticker, ts_ms, side)
    return signals.calibrator_ok(ticker, ts_ms, side)   # None when un-gateable

def clustered_mean(values, clusters):
    n = len(values)
    if n == 0:
        return {"n": 0, "effective_n": 0.0, "mean": None, "se": None, "t": None, "ci95": [None, None]}
    mean = sum(values) / n
    sizes = collections.Counter(clusters)
    eff = (n * n) / sum(s * s for s in sizes.values())
    groups = collections.defaultdict(float)
    for v, c in zip(values, clusters):
        groups[c] += (v - mean)
    cr_var = sum(g * g for g in groups.values()) / (n * n)
    se = math.sqrt(cr_var)
    t = mean / se if se > 0 else None
    half = 1.96 * se
    return {"n": n, "effective_n": eff, "mean": mean, "se": se, "t": t,
            "ci95": [mean - half, mean + half]}

def _normal_sf(z):
    return 0.5 * math.erfc(z / math.sqrt(2.0))

def _two_sided_p(t):
    return 2.0 * _normal_sf(abs(t)) if t is not None else 1.0

def benjamini_hochberg(pvals, alpha=0.05):
    m = len(pvals)
    order = sorted(range(m), key=lambda i: pvals[i])
    rejected = [False] * m
    max_k = -1
    for rank, idx in enumerate(order, start=1):
        if pvals[idx] <= alpha * (rank + 1) / m:
            max_k = rank
    for rank, idx in enumerate(order, start=1):
        if rank <= max_k:
            rejected[idx] = True
    return rejected

def _fold_bounds(sorted_close, folds=5):
    m = len(sorted_close)
    return [sorted_close[min(m - 1, (m * k) // folds)] for k in range(1, folds + 1)]

def walkforward_delta(records, key):
    if not records:
        return None
    closes = sorted(r["close_ms"] for r in records)
    last_train_close = _fold_bounds(closes)[-2] if len(closes) >= 5 else closes[0]
    test = [r for r in records if r["close_ms"] > last_train_close]
    if not test:
        return None
    return sum(r["pnl"] - r[key] for r in test) / len(test)

def score_variant(records):
    if not records:
        return {"n": 0}
    dh = [r["pnl"] - r["hold_pnl"] for r in records]
    dm = [r["pnl"] - r["market_pnl"] for r in records]
    clusters = [r["cluster"] for r in records]
    ch = clustered_mean(dh, clusters)
    return {
        "n": len(records),
        "delta_vs_hold": sum(dh) / len(dh),
        "delta_vs_market": sum(dm) / len(dm),
        "clustered": ch,
        "p_value": _two_sided_p(ch["t"]),
        "walkforward_delta": walkforward_delta(records, "hold_pnl"),
        "win_rate": sum(1 for r in records if r["won"]) / len(records),
    }
