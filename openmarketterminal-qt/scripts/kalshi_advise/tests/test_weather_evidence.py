#!/usr/bin/env python3
"""Regression test for weather_producer.bracket_record and its wiring into
decisions_for_series (Task 1: all-evaluated-bracket evidence for the
WeatherScreen UI, see
docs/superpowers/plans/2026-08-03-per-category-kalshi-views.md).

Coverage:
  - bracket_record() shape/values when a book WAS fetched (market_*/edge
    populated, sign-consistent with the decision rule).
  - bracket_record() when the book was NOT fetched (yes_bid/yes_ask=None):
    forecast fields (cheap pure-math) stay populated; market_*/edge are
    JSON null.
  - decisions_for_series() appends a record for EVERY bracket it iterates,
    including out-of-window ones (in_window=False, book never fetched, so
    book_bid_ask must NOT be called for them — network-cheap).

Runnable either under pytest:
  <pybin> -m pytest scripts/kalshi_advise/tests/test_weather_evidence.py -q

or as a plain script (exits non-zero on failure, no pytest required):
  <pybin> scripts/kalshi_advise/tests/test_weather_evidence.py
"""
import datetime
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
KALSHI_ADVISE_DIR = os.path.dirname(HERE)          # .../scripts/kalshi_advise
SCRIPTS_DIR = os.path.dirname(KALSHI_ADVISE_DIR)   # .../scripts
for p in (KALSHI_ADVISE_DIR, SCRIPTS_DIR):
    if p not in sys.path:
        sys.path.insert(0, p)

import weather_producer as wp  # noqa: E402


class _Patch:
    """Minimal setattr/restore helper so tests run identically under pytest
    or as a plain script, without depending on pytest's monkeypatch fixture."""

    def __init__(self):
        self._saved = []

    def setattr(self, obj, name, value):
        self._saved.append((obj, name, getattr(obj, name)))
        setattr(obj, name, value)

    def undo(self):
        for obj, name, value in reversed(self._saved):
            setattr(obj, name, value)


def test_bracket_record_shape():
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


def test_bracket_record_no_book():
    """Book not fetched (e.g. an out-of-window bracket): forecast fields stay
    populated (cheap pure-math off the already-fetched daily forecast), but
    market_bid/market_ask/market_mid/edge are JSON null rather than 0/fabricated."""
    rec = wp.bracket_record(series="KXHIGHNY", cfg=("NYC", 40.7, -74.0, 1.5, 3.0),
                            ticker="KXHIGHNY-26AUG03-T90", strike_type="greater",
                            floor=90.0, cap=None, fc=82.0, std=3.0,
                            yes_bid=None, yes_ask=None, now_ms=0, close_ts=0)
    assert rec["forecast_high_f"] is not None
    assert rec["forecast_p"] is not None
    assert 0.0 <= rec["forecast_p"] <= 1.0
    assert rec["market_bid"] is None
    assert rec["market_ask"] is None
    assert rec["market_mid"] is None
    assert rec["edge"] is None
    assert rec["in_window"] is False  # seconds_left = close_ts - now_ms//1000 = 0


def test_decisions_for_series_includes_all_brackets():
    """Integration: decisions_for_series must append a bracket_record for
    EVERY strike it iterates (in-window and out-of-window alike), fetching
    the book (and filling market_*/edge) only for the in-window one — no
    network-call explosion for the rest of the bracket browser."""
    patch = _Patch()
    try:
        now_ms = 0
        in_window_close = datetime.datetime.utcfromtimestamp(40000).isoformat() + "Z"
        out_window_close = datetime.datetime.utcfromtimestamp(200000).isoformat() + "Z"
        fake_markets = {
            "markets": [
                {"ticker": "KXHIGHNY-26AUG03-T80", "strike_type": "greater",
                 "floor_strike": 80.0, "cap_strike": None,
                 "close_time": in_window_close, "title": "t1"},
                {"ticker": "KXHIGHNY-26AUG03-T90", "strike_type": "greater",
                 "floor_strike": 90.0, "cap_strike": None,
                 "close_time": out_window_close, "title": "t2"},
            ]
        }
        book_calls = []

        def fake_http_json(url, tries=3):
            return fake_markets

        def fake_forecast_high(lat, lon, date):
            return 82.0

        def fake_book_bid_ask(ticker):
            book_calls.append(ticker)
            return 0.50, 0.55, 10.0, 12.0

        patch.setattr(wp, "http_json", fake_http_json)
        patch.setattr(wp, "forecast_high", fake_forecast_high)
        patch.setattr(wp, "book_bid_ask", fake_book_bid_ask)
        wp.BRACKETS.clear()

        cfg = ("NYC-HIGH", 40.78, -73.97, 1.2, 2.1)
        wp.decisions_for_series("KXHIGHNY", cfg, now_ms)

        by_ticker = {r["ticker"]: r for r in wp.BRACKETS}
        assert set(by_ticker) == {"KXHIGHNY-26AUG03-T80", "KXHIGHNY-26AUG03-T90"}, by_ticker

        in_rec = by_ticker["KXHIGHNY-26AUG03-T80"]
        out_rec = by_ticker["KXHIGHNY-26AUG03-T90"]

        assert in_rec["in_window"] is True
        assert in_rec["forecast_high_f"] is not None
        assert in_rec["forecast_p"] is not None
        assert in_rec["market_bid"] == 0.50
        assert in_rec["market_ask"] == 0.55
        assert in_rec["market_mid"] == 0.525
        assert in_rec["edge"] is not None

        # out-of-window bracket IS present, with forecast filled and book null.
        assert out_rec["in_window"] is False
        assert out_rec["forecast_high_f"] is not None
        assert out_rec["forecast_p"] is not None
        assert out_rec["market_bid"] is None
        assert out_rec["market_ask"] is None
        assert out_rec["market_mid"] is None
        assert out_rec["edge"] is None

        # network-cheap: book_bid_ask called only for the in-window bracket.
        assert book_calls == ["KXHIGHNY-26AUG03-T80"], book_calls
    finally:
        patch.undo()
        wp.BRACKETS.clear()


ALL_TESTS = [
    test_bracket_record_shape,
    test_bracket_record_no_book,
    test_decisions_for_series_includes_all_brackets,
]

if __name__ == "__main__":
    failures = []
    for t in ALL_TESTS:
        try:
            t()
        except Exception as exc:  # noqa: BLE001 - deliberate: report and fail loudly
            failures.append(f"{t.__name__}: {exc}")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.exit(1)
    print(f"PASS ({len(ALL_TESTS)} tests)")
    sys.exit(0)
