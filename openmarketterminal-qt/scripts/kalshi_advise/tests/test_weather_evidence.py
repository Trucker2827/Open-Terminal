#!/usr/bin/env python3
"""Regression test for weather_producer.bracket_record (Task 1: all-evaluated-
bracket evidence for the WeatherScreen UI, see
docs/superpowers/plans/2026-08-03-per-category-kalshi-views.md).

Runnable either under pytest:
  <pybin> -m pytest scripts/kalshi_advise/tests/test_weather_evidence.py -q

or as a plain script (exits non-zero on failure, no pytest required):
  <pybin> scripts/kalshi_advise/tests/test_weather_evidence.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
KALSHI_ADVISE_DIR = os.path.dirname(HERE)          # .../scripts/kalshi_advise
SCRIPTS_DIR = os.path.dirname(KALSHI_ADVISE_DIR)   # .../scripts
for p in (KALSHI_ADVISE_DIR, SCRIPTS_DIR):
    if p not in sys.path:
        sys.path.insert(0, p)

import weather_producer as wp  # noqa: E402


def test_bracket_record_shape(monkeypatch=None):
    rec = wp.bracket_record(series="KXHIGHNY", cfg=("NYC", 40.7, -74.0, 1.5, 3.0),
                            ticker="KXHIGHNY-26AUG03-T80", strike_type="greater",
                            floor=80.0, cap=None, fc=82.0, std=3.0,
                            yes_bid=0.20, yes_ask=0.22, now_ms=0, close_ts=0)
    for k in ("series", "city", "ticker", "floor", "cap", "strike_type", "forecast_high_f",
              "forecast_p", "market_bid", "market_ask", "market_mid", "edge", "in_window",
              "seconds_left", "generated_at_ms"):
        assert k in rec, f"missing key: {k}"
    assert 0.0 <= rec["forecast_p"] <= 1.0

    # edge must be sign-consistent with the decision rule in decisions_for_series:
    # max(yes-side edge, no-side edge), where no_ask = 1 - yes_bid.
    no_ask = 1.0 - 0.20
    expected_edge = max(rec["forecast_p"] - 0.22, (1.0 - rec["forecast_p"]) - no_ask)
    assert abs(rec["edge"] - round(expected_edge, 4)) < 1e-6

    assert rec["series"] == "KXHIGHNY"
    assert rec["city"] == "NYC"
    assert rec["ticker"] == "KXHIGHNY-26AUG03-T80"
    assert rec["floor"] == 80.0
    assert rec["cap"] is None
    assert rec["strike_type"] == "greater"
    assert rec["market_bid"] == 0.20
    assert rec["market_ask"] == 0.22
    assert rec["generated_at_ms"] == 0


if __name__ == "__main__":
    try:
        test_bracket_record_shape()
    except Exception as exc:  # noqa: BLE001 - deliberate: report and fail loudly
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
    print("PASS")
    sys.exit(0)
