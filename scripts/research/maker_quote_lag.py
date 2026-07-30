#!/usr/bin/env python3
"""Kalshi maker quote-lag engine (read-only). Measures whether RESTING a quote to
capture the book-lag after a >=sigma BTC move is a real edge, with a fill model that
BRACKETS queue position between optimistic (front) and pessimistic (back) bounds
computed from real trade prints. Certifies an edge only under the pessimistic bound.
NO live execution, ever. See docs/design/2026-07-30-kalshi-maker-quote-lag-design.md.

    python3 scripts/research/maker_quote_lag.py
"""
import bisect, collections, json, math, sys
import kalshi_edge_common as common
import q1_quote_lag as q1

SCHEMA_VERSION = 1


def maker_fill(ahead_size, hits):
    """Bracket the fill of a resting order against the taker volume that would
    execute it. `hits` = sorted [(ts_ms, size)] of trades through our price on the
    taking side, after we joined. Optimistic = first hit (front of queue).
    Pessimistic = the hit at which cumulative size first exceeds `ahead_size`
    (back of queue), else None. Non-fill is a real outcome, not a free win.
    """
    if not hits:
        return {"optimistic": None, "pessimistic": None}
    optimistic = hits[0][0]
    cumulative = 0.0
    pessimistic = None
    for ts_ms, size in hits:
        cumulative += size
        if cumulative > ahead_size:
            pessimistic = ts_ms
            break
    return {"optimistic": optimistic, "pessimistic": pessimistic}
