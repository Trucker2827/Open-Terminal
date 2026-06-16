"""Tests for the pure thesis/regime/scoring math (no network, no LLM).

Focus: the arbitrary thresholds most likely to drift (break buffer, trend slope) and
the symmetric weekly scorer (wins, losses, false breakouts, expectancy sign). Run:
    ./.venv311/bin/python3 tests_thesis.py
"""
from trading_mcp import thesis as T


def approx(a, b, eps=1e-9):
    return abs(a - b) < eps


def test_regime_defaults_to_chop():
    # Flat series: MAs flat, no break -> must NOT invent a direction.
    flat = [100.0] * 60
    assert T.classify_regime(flat)["regime"] == "chop"


def test_regime_unclear_on_short_history():
    assert T.classify_regime([100.0] * 10)["regime"] == "unclear"


def test_breakout_buffer_boundary():
    # prior 20d high is 100; break requires > 100 * (1 + 0.005) = 100.5
    over = [100.0] * 50 + [100.6]
    under = [100.0] * 50 + [100.4]
    assert T.classify_regime(over)["regime"] == "breakout"
    assert T.classify_regime(under)["regime"] == "chop"   # a small poke is NOT a breakout


def test_breakdown_buffer_boundary():
    under = [100.0] * 50 + [99.4]   # below 100 * 0.995 = 99.5
    near = [100.0] * 50 + [99.6]
    assert T.classify_regime(under)["regime"] == "breakdown"
    assert T.classify_regime(near)["regime"] == "chop"


def test_trend_up_requires_slope_and_alignment():
    # Rising series that paused today (no new high) -> trend_up, not breakout.
    closes = [100.0 + i for i in range(59)] + [158.0]
    r = T.classify_regime(closes)
    assert r["regime"] == "trend_up", r["regime"]
    assert r["ma20"] > r["ma50"]


def test_vol_state_flags_elevation():
    # Calm then a burst -> latest vol should rank as elevated.
    calm = [100.0 + 0.1 * (i % 2) for i in range(70)]
    burst = calm + [100, 130, 95, 140, 90, 135, 92, 138]
    vs = T.vol_state(burst)
    assert vs is not None and vs["label"] == "elevated", vs

    steady = [100.0 * (1.001 ** i) for i in range(90)]
    assert T.vol_state(steady) is not None  # does not crash on a smooth series


def test_watch_levels():
    closes = [90.0, 95, 100, 105, 110] * 5  # 25 points
    lv = T.watch_levels(closes)
    assert lv["resistance"] == 110 and lv["support"] == 90
    assert lv["price"] == closes[-1]


def _hist(prices, regimes, resistance=1.0):
    return [{"date": f"2026-06-{i + 1:02d}",
             "btc": {"price": p, "regime": rg, "resistance": resistance, "support": 0.0}}
            for i, (p, rg) in enumerate(zip(prices, regimes))]


def test_score_winning_breakout():
    h = _hist([100, 103, 106, 110, 112],
              ["breakout", "chop", "chop", "chop", "chop"], resistance=99)
    s = T.score_history(h, "btc", horizon=3, fee_bps=20.0)
    assert s["n_signals"] == 1
    assert s["expectancy"] > 0           # +10% over 3d, minus 0.4% fees
    assert s["false_breakouts"] == 0
    assert approx(s["hit_rate"], 1.0)


def test_score_false_breakout_and_loss():
    h = _hist([100, 98, 97, 95],
              ["breakout", "chop", "chop", "chop"], resistance=99)
    s = T.score_history(h, "btc", horizon=3, fee_bps=20.0)
    assert s["n_signals"] == 1
    assert s["expectancy"] < 0
    assert s["false_breakouts"] == 1     # fell back below 99 within the horizon
    assert approx(s["hit_rate"], 0.0)


def test_score_no_edge_verdict_when_enough_signals():
    prices = [100 - i for i in range(12)]          # strictly falling
    regimes = ["breakout"] * 12                     # all long signals into a downtrend -> all lose
    h = _hist(prices, regimes, resistance=1.0)      # resistance below price -> no false-breakout flag
    s = T.score_history(h, "btc", horizon=3, fee_bps=20.0)
    assert s["n_signals"] >= T.MIN_SIGNALS_FOR_VERDICT
    assert s["expectancy"] < 0
    assert "no demonstrated edge" in s["verdict"]


def test_score_empty_and_starved_degrade():
    assert T.score_history([], "btc")["n_signals"] == 0
    assert "insufficient" in T.score_history([], "btc")["verdict"]
    starved = _hist([100, 101], ["breakout", "chop"])
    s = T.score_history(starved, "btc", horizon=3)
    assert s["n_signals"] == 0           # not enough forward bars to score the signal
    assert "insufficient" in s["verdict"]


def main():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    for fn in fns:
        fn()
        print(f"ok  {fn.__name__}")
    print(f"\nthesis tests passed ({len(fns)})")


if __name__ == "__main__":
    main()
