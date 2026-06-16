"""Pure, deterministic market-read helpers for the daily observer and weekly review.

NO LLM, NO network, NO side effects — just math on a list of daily closes. The
design bias is HONESTY over confidence: the regime classifier defaults to
"chop / no edge" and only emits a directional call when the data clearly clears a
threshold. The weekly scorer is SYMMETRIC (it scores every signal the rule fired,
net of fees) so "no demonstrated edge" is a valid — and expected — result, not a
bug. This keeps the feedback loop (observe -> journal -> review) from turning into
a hindsight machine that rationalises arming live trading.
"""
import math

# --- thresholds (the arbitrary parts — pinned here and tested at the boundary) ---
BREAK_BUFFER = 0.005   # price must clear the prior 20d extreme by >0.5% to count as a break (not noise)
TREND_SLOPE = 0.01     # MA20 must move >1% over 5 sessions to call a trend
VOL_WINDOW = 20        # sessions in a realized-vol sample
VOL_LOOKBACK = 90      # window over which to rank current vol into a percentile
WEEKLY_HORIZON = 3     # forward sessions used to score a signal
WEEKLY_FEE_BPS = 20.0  # per-side cost assumption for paper P&L (round-trip = 2x)
MIN_SIGNALS_FOR_VERDICT = 5

DIRECTIONAL = {"breakout": 1, "trend_up": 1, "breakdown": -1, "trend_down": -1}


# ----------------------------- basic stats -----------------------------
def _sma(xs, n):
    if len(xs) < n:
        return None
    return sum(xs[-n:]) / n


def _sma_at(xs, n, offset):
    """SMA of the n closes ending `offset` bars before the last close."""
    end = len(xs) - offset
    if end < n:
        return None
    return sum(xs[end - n:end]) / n


def _log_returns(closes):
    return [math.log(closes[i] / closes[i - 1])
            for i in range(1, len(closes))
            if closes[i - 1] > 0 and closes[i] > 0]


def _stdev(xs):
    n = len(xs)
    if n < 2:
        return None
    m = sum(xs) / n
    return math.sqrt(sum((x - m) ** 2 for x in xs) / (n - 1))


# ----------------------------- volatility -----------------------------
def realized_vol(closes, window=VOL_WINDOW):
    """Daily realized vol (stdev of the last `window` log returns), as a fraction."""
    rets = _log_returns(closes)
    if len(rets) < window:
        return None
    return _stdev(rets[-window:])


def vol_state(closes, window=VOL_WINDOW, lookback=VOL_LOOKBACK):
    """Classify current vol vs its own recent distribution: compressed / normal / elevated."""
    rets = _log_returns(closes)
    if len(rets) < window:
        return None
    rets = rets[-lookback:]
    samples = [s for i in range(window, len(rets) + 1)
               if (s := _stdev(rets[i - window:i])) is not None]
    if not samples:
        return None
    cur = samples[-1]
    pct = 100.0 * sum(1 for s in samples if s <= cur) / len(samples)
    label = "compressed" if pct < 33 else ("elevated" if pct > 67 else "normal")
    return {"daily_pct": cur * 100, "pctile": pct, "label": label}


# ----------------------------- regime -----------------------------
def watch_levels(closes):
    """Support / resistance / MA pivot from the recent window. None if too little data."""
    if len(closes) < 21:
        return None
    return {
        "support": min(closes[-20:]),
        "resistance": max(closes[-20:]),
        "ma20": _sma(closes, 20),
        "ma50": _sma(closes, 50),
        "price": closes[-1],
    }


def classify_regime(closes):
    """Honest regime classifier. DEFAULTS to 'chop' (no edge). A directional label is
    only returned when the data clearly clears a threshold:
      - breakout/breakdown: close beyond the prior 20d extreme by > BREAK_BUFFER
      - trend_up/down:      MA20 vs MA50 aligned, price on-side, MA20 slope > TREND_SLOPE
    Returns a dict with 'regime' plus the levels behind the call.
    """
    if len(closes) < 51:
        return {"regime": "unclear", "detail": "insufficient history"}
    price = closes[-1]
    ma20 = _sma(closes, 20)
    ma50 = _sma(closes, 50)
    ma20_prev = _sma_at(closes, 20, 5)
    prior_hi20 = max(closes[-21:-1])   # excludes today, so today can break it
    prior_lo20 = min(closes[-21:-1])
    slope = (ma20 - ma20_prev) / ma20_prev if ma20_prev else 0.0
    base = {"price": price, "ma20": ma20, "ma50": ma50, "slope": slope,
            "prior_hi20": prior_hi20, "prior_lo20": prior_lo20}

    if price > prior_hi20 * (1 + BREAK_BUFFER):
        return {"regime": "breakout", **base}
    if price < prior_lo20 * (1 - BREAK_BUFFER):
        return {"regime": "breakdown", **base}
    if ma20 > ma50 and price > ma20 and slope > TREND_SLOPE:
        return {"regime": "trend_up", **base}
    if ma20 < ma50 and price < ma20 and slope < -TREND_SLOPE:
        return {"regime": "trend_down", **base}
    return {"regime": "chop", **base}


REGIME_LABEL = {
    "breakout": "breakout", "breakdown": "breakdown",
    "trend_up": "trend (up)", "trend_down": "trend (down)",
    "chop": "chop (no edge)", "unclear": "unclear (low data)",
}


def thesis_text(regime, lv):
    """Paper-trade idea + honest reason-for-no-action for a regime. Always framed as
    PAPER, small, with a stop — never a live call. Default is 'stay flat'."""
    res = lv["resistance"]
    sup = lv["support"]
    ma20 = lv["ma20"]
    if regime == "breakout":
        idea = (f"Confirmed break above ${res:,.0f}. PAPER ONLY: a small long above "
                f"${res:,.0f} with a stop back under it tests continuation — not live, size tiny.")
        reason = ("Break is confirmed, but breakouts fail often in crypto; this is a paper "
                  "test of follow-through, not conviction.")
    elif regime == "breakdown":
        idea = (f"Confirmed break below ${sup:,.0f}. PAPER ONLY: don't catch the knife; a "
                f"reclaim back above ${sup:,.0f} would be the paper long trigger.")
        reason = "Falling through support; no edge buying weakness. Wait for a reclaim."
    elif regime == "trend_up":
        idea = (f"Uptrend (MA20>MA50, rising). PAPER ONLY: a pullback toward MA20 "
                f"${ma20:,.0f} is the lower-risk paper entry; chasing here is extended.")
        reason = "Trend up but price extended above MA20; better paper entry is a pullback, not a chase."
    elif regime == "trend_down":
        idea = (f"Downtrend (MA20<MA50, falling). PAPER ONLY: no long edge; a bounce into "
                f"MA20 ${ma20:,.0f} is where a paper short would be tested.")
        reason = "Trend down; longs fight the tape."
    else:  # chop / unclear
        idea = (f"No edge — stay flat. Only a confirmed break of ${res:,.0f} (long) or "
                f"${sup:,.0f} (short) is worth a small paper test with a stop on the other side.")
        reason = ("Mid-range, MAs flat, no confirmed break. Trend rules bleed in chop "
                  "(proven negative on 60d MA5/20); fees + whipsaw = negative expectancy.")
    return idea, reason


# ----------------------------- weekly symmetric scoring -----------------------------
def score_history(history, asset, horizon=WEEKLY_HORIZON, fee_bps=WEEKLY_FEE_BPS):
    """SYMMETRIC forward-test of the regime signals stored in `history` for one asset.

    history: list of daily snapshots (chronological), each a dict with date and per-asset
    sub-dicts {price, regime, support, resistance, ...}. For every day whose regime was
    directional, compute the net-of-fee P&L of the implied PAPER trade `horizon` days
    later (round-trip fee = 2x fee_bps), and tally hit rate / avg win / avg loss /
    false breakouts / net expectancy. 'no demonstrated edge' is a valid verdict.
    """
    rows = [(h.get("date"), h.get(asset, {})) for h in history]
    rows = [(d, a) for d, a in rows if a and a.get("price") is not None]
    n = len(rows)
    fee = fee_bps / 10000.0
    trades = []
    false_breakouts = 0
    for i, (date, a) in enumerate(rows):
        regime = a.get("regime")
        direction = DIRECTIONAL.get(regime)
        if direction is None:
            continue
        if i + horizon >= n:
            continue  # not enough forward data yet
        entry = rows[i][1]["price"]
        exit_ = rows[i + horizon][1]["price"]
        if not entry or not exit_:
            continue
        fwd = exit_ / entry - 1.0
        pnl = direction * fwd - 2 * fee  # paper P&L net of round-trip cost
        trades.append(pnl)
        # false breakout: a breakout that closed back below the broken level within the horizon
        if regime == "breakout":
            level = a.get("resistance")
            window = [rows[j][1]["price"] for j in range(i + 1, i + horizon + 1)]
            if level and any(p is not None and p < level for p in window):
                false_breakouts += 1
    return _summarize(trades, false_breakouts, n)


def _summarize(trades, false_breakouts, days):
    ns = len(trades)
    wins = [t for t in trades if t > 0]
    losses = [t for t in trades if t <= 0]
    expectancy = sum(trades) / ns if ns else None
    stats = {
        "days": days,
        "n_signals": ns,
        "hit_rate": (len(wins) / ns) if ns else None,
        "avg_win": (sum(wins) / len(wins)) if wins else None,
        "avg_loss": (sum(losses) / len(losses)) if losses else None,
        "expectancy": expectancy,
        "false_breakouts": false_breakouts,
    }
    stats["verdict"] = _verdict(ns, expectancy)
    return stats


def _verdict(ns, expectancy):
    if ns < MIN_SIGNALS_FOR_VERDICT:
        return f"insufficient signals ({ns}) for a verdict — keep observing"
    if expectancy is None or expectancy <= 0:
        e = 0.0 if expectancy is None else expectancy * 100
        return (f"no demonstrated edge (expectancy {e:+.2f}% net of fees over {ns} signals) "
                "— consistent with the backtests; stay flat / paper only")
    return (f"positive expectancy {expectancy * 100:+.2f}% over {ns} signals — still PAPER, "
            "needs many more samples before any trust")
