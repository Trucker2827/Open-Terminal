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
