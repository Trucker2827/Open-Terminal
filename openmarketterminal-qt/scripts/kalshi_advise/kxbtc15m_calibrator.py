#!/usr/bin/env python3
"""Directional probability calibrator for Kalshi KXBTC15M (BTC up/down 15m).

Advisory-only. Learns nothing via gradient descent: the "full" model is a
deterministic remaining-time Gaussian P(close > open) from the daemon's own
open reference (horizon.floor_strike / reference_strike), live spot, and
realized per-minute vol. The market mid is scored beside it, untrained.

Why this exists separately from spot_calibrator.py
-------------------------------------------------
spot_calibrator is the threshold / hourly ensemble. Its Brier mixes families,
and its online logit is the wrong instrument for the 15-minute race. This
module scores ONLY KXBTC15M contracts, one number per settled contract
(schema-2 honesty from issue #171), and writes kxbtc15m-calibrator.json so
the paper bot can trust that family only when its own Brier beats the raw mid.

Settlement rule (stated, not guessed)
-------------------------------------
YES means the settlement reference at close is ABOVE the window open. The
daemon publishes the open as floor_strike / reference_strike on each
KXBTC15M snapshot ("BTC price up in next 15 mins?"). Outcomes are taken from
Kalshi's recorded result when available; otherwise derived from the BRTI
60-second average at close versus that open (same derivation honesty pattern
as the edge autopsy — refuse when the feed has a hole).

Report shape matches what KalshiBotDecision already reads: predictions keyed
by ticker with p_yes_full, market_yes_mid, features.sqrt_minutes_left, and
optional book passthrough keys.

Commands:  once | run [--interval 60] | report
"""
from __future__ import annotations

import datetime
import json
import math
import os
import sys
import time
import urllib.request
import zoneinfo

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from openterminal_paths import evidence_file
import outside_info_features as oif

EVIDENCE_PATH = evidence_file("kalshi-ws-books.json")
STATE_PATH = evidence_file("kxbtc15m-calibrator-state.json")
OUTPUT_PATH = evidence_file("kxbtc15m-calibrator.json")
BRTI_PATH = evidence_file("kalshi-cf-benchmarks.jsonl")
BRTI_PATH_ROTATION = evidence_file("kalshi-cf-benchmarks.jsonl.1")
SETTLEMENTS_PATH = evidence_file("kalshi-settlements.jsonl")
KALSHI_MARKET_URL = "https://api.elections.kalshi.com/trade-api/v2/markets/{ticker}"
KALSHI_MARKETS_URL = (
    "https://api.elections.kalshi.com/trade-api/v2/markets"
    "?series_ticker={series}&status=open&limit=50"
)
UA = "OpenTerminal/kxbtc15m-calibrator"

FAMILY = "KXBTC15M"
STATE_SCHEMA = 4
MAX_OBS_PER_TICKER = 60
SCORED_CONTRACT_WINDOW = 500
# CONTRACTS, never observations — same floor as spot_calibrator schema 2.
MIN_SCORED_CONTRACTS = 100
WINDOW_SECONDS = 15 * 60
SPOT_MOVED_BPS = 0.5
MID_MOVED_CENTS = 0.1
# Prefer BRTI avg60 for the settlement-aligned ablation in the final minutes;
# still score it whenever avg60 is available earlier in the window.
BRTI_AVG60_NEAR_CLOSE_S = 180
EASTERN = zoneinfo.ZoneInfo("America/New_York")
CLOSE_FOLD = 0
MONTHS = {"JAN": 1, "FEB": 2, "MAR": 3, "APR": 4, "MAY": 5, "JUN": 6,
          "JUL": 7, "AUG": 8, "SEP": 9, "OCT": 10, "NOV": 11, "DEC": 12}
ABLATION_KEYS = (
    "physics",
    "physics_veto_on_conflict",
    "physics_confirm_only",
    "physics_brti_avg60",
    "physics_vol_regime_confirm",
)


def clamp_probability(value):
    return max(0.001, min(0.999, float(value)))


def normal_cdf(z):
    """Standard normal CDF via erfc — same construction KalshiAutoEngine uses."""
    return 0.5 * math.erfc(-z / math.sqrt(2.0))


def directional_probability(open_price, spot, seconds_left, per_min_vol_bps):
    """P(close > open) under a remaining-time normal around the current spot.

    close ~ N(spot, sigma^2) with
        sigma = spot * (per_min_vol_bps / 1e4) * sqrt(minutes_left)

    When seconds_left <= 0 the outcome is decided by the spot vs open
    comparison (no fabricated residual vol). Missing/non-positive inputs
    refuse with None rather than inventing a coin flip.
    """
    try:
        open_price = float(open_price)
        spot = float(spot)
        seconds_left = int(seconds_left)
        per_min_vol_bps = float(per_min_vol_bps or 0.0)
    except (TypeError, ValueError):
        return None
    if open_price <= 0.0 or spot <= 0.0:
        return None
    if seconds_left <= 0:
        return 0.999 if spot > open_price else 0.001
    minutes_left = seconds_left / 60.0
    if per_min_vol_bps <= 0.0:
        return 0.999 if spot > open_price else 0.001
    sigma = spot * (per_min_vol_bps / 10000.0) * math.sqrt(minutes_left)
    if sigma <= 1e-12:
        return 0.999 if spot > open_price else 0.001
    return clamp_probability(normal_cdf((spot - open_price) / sigma))


def is_kxbtc15m_ticker(ticker):
    if not isinstance(ticker, str) or not ticker:
        return False
    return ticker.split("-", 1)[0] == FAMILY


def parse_close_ms(ticker):
    """Close instant from the ticker date token, US/Eastern (issue #176)."""
    parts = ticker.split("-")
    if len(parts) < 2:
        return None
    token = parts[1]
    # KXBTC15M-26AUG061945-45 → YYMONDDHHMM
    if len(token) < 11:
        return None
    try:
        yy = int(token[0:2])
        mon = MONTHS[token[2:5]]
        dd = int(token[5:7])
        hh = int(token[7:9])
        mm = int(token[9:11])
        close = datetime.datetime(2000 + yy, mon, dd, hh, mm, tzinfo=EASTERN,
                                  fold=CLOSE_FOLD)
        return int(close.timestamp() * 1000)
    except (KeyError, ValueError):
        return None


def open_price_from_horizon(horizon):
    """Window open from the daemon snapshot. Prefer reference_strike, then floor."""
    if not isinstance(horizon, dict):
        return None
    for key in ("reference_strike", "floor_strike"):
        try:
            value = float(horizon.get(key) or 0.0)
        except (TypeError, ValueError):
            continue
        if value > 0.0:
            return value
    return None


def lag_flags_from_horizon(horizon, contract=None):
    """Venue-lead / sticky-mid flags from daemon horizon (+ contract fallback)."""
    horizon = horizon if isinstance(horizon, dict) else {}
    contract = contract if isinstance(contract, dict) else {}
    try:
        venue_lead = float(horizon.get("venue_lead_bps_30s",
                                       horizon.get("realized_move_30s_bps", 0.0)) or 0.0)
    except (TypeError, ValueError):
        venue_lead = 0.0
    # realized_move_30s_bps is abs in horizon — prefer signed venue_lead when present.
    if "venue_lead_bps_30s" not in horizon:
        # Fall back to unsigned realized move cannot establish direction; treat as 0.
        venue_lead = 0.0
    try:
        mid_lag = float(horizon.get("mid_lag_cents_30s",
                                    contract.get("yes_change_30s_cents", 0.0)) or 0.0)
    except (TypeError, ValueError):
        mid_lag = 0.0

    if "lead_confirms_direction" in horizon or "lead_conflicts" in horizon:
        confirms = bool(horizon.get("lead_confirms_direction"))
        conflicts = bool(horizon.get("lead_conflicts"))
    else:
        spot_up = venue_lead > SPOT_MOVED_BPS
        spot_down = venue_lead < -SPOT_MOVED_BPS
        mid_up = mid_lag > MID_MOVED_CENTS
        mid_down = mid_lag < -MID_MOVED_CENTS
        conflicts = (spot_up and mid_down) or (spot_down and mid_up)
        confirms = (spot_up or spot_down) and not conflicts and (
            (not mid_up and not mid_down) or (spot_up and mid_up) or (spot_down and mid_down)
        )
    return {
        "venue_lead_bps_30s": venue_lead,
        "mid_lag_cents_30s": mid_lag,
        "lead_confirms_direction": confirms,
        "lead_conflicts": conflicts,
    }


def ablation_probabilities(p_physics, yes_mid, lead_confirms, lead_conflicts,
                           p_brti_avg60=None, per_min_vol_bps=None, now_ms=None):
    """physics / veto / confirm / BRTI-avg60 / vol-regime probabilities."""
    p_physics = float(p_physics)
    yes_mid = float(yes_mid)
    p_veto = yes_mid if lead_conflicts else p_physics
    p_confirm = p_physics if lead_confirms else yes_mid
    p_avg = float(p_brti_avg60) if p_brti_avg60 is not None else p_physics
    session = oif.session_regime_at(now_ms) if now_ms is not None else None
    p_vol = oif.ablation_vol_regime_confirm(
        p_physics, yes_mid, per_min_vol_bps, session=session)
    return {
        "physics": p_physics,
        "physics_veto_on_conflict": p_veto,
        "physics_confirm_only": p_confirm,
        "physics_brti_avg60": p_avg,
        "physics_vol_regime_confirm": p_vol,
    }


def brti_avg60_from_horizon(horizon):
    """Daemon-published BRTI 60s average, or None when unavailable."""
    if not isinstance(horizon, dict):
        return None
    try:
        value = float(horizon.get("brti_avg_60s") or 0.0)
    except (TypeError, ValueError):
        return None
    return value if value > 0.0 else None


def settlement_aligned_spot(spot, brti_avg60, seconds_left):
    """Prefer BRTI avg60 (payout underlier), especially near close.

    Returns (spot_used, source_tag). When avg60 is missing, fall back to the
    last-print/independent spot so the ablation can still score.
    """
    try:
        spot = float(spot or 0.0)
    except (TypeError, ValueError):
        spot = 0.0
    if brti_avg60 is not None and brti_avg60 > 0.0:
        # Always use avg60 for the settlement-aligned ablation when present —
        # that is what Kalshi pays against at the window close.
        return float(brti_avg60), "brti_avg_60s"
    if spot > 0.0:
        tag = "last_print_fallback"
        if seconds_left is not None and int(seconds_left) <= BRTI_AVG60_NEAR_CLOSE_S:
            tag = "last_print_near_close_no_avg60"
        return spot, tag
    return None, None


def extract_observation(ticker, snapshot, brti_avg60=None, now_ms=None):
    """One scored observation, or None when this snapshot is not modelable.

    `seconds_left` is derived from the ticker close (US/Eastern) vs `now_ms`
    whenever the close parses. Daemon snapshots freeze `contract.seconds_left`
    at last observation — trusting that alone keeps closed races in the report
    and the cockpit FLOW for hours after the window ended.
    """
    if not is_kxbtc15m_ticker(ticker):
        return None
    contract = snapshot.get("contract") or {}
    horizon = contract.get("horizon") or {}
    if now_ms is None:
        now_ms = int(time.time() * 1000)
    close_ms = parse_close_ms(ticker)
    if close_ms is not None:
        # Clock is the authority on expiry — not a frozen snapshot counter.
        if now_ms >= close_ms:
            return None
        seconds_left = int((close_ms - now_ms) / 1000)
    else:
        try:
            seconds_left = int(contract.get("seconds_left"))
        except (TypeError, ValueError):
            try:
                seconds_left = int(horizon.get("seconds_left"))
            except (TypeError, ValueError):
                return None
    try:
        yes_mid = float(contract.get("yes_mid") or 0.0)
    except (TypeError, ValueError):
        return None
    if not 0.0 < yes_mid < 1.0 or seconds_left <= 0:
        return None
    open_price = open_price_from_horizon(horizon)
    try:
        spot = float(horizon.get("spot") or 0.0)
    except (TypeError, ValueError):
        spot = 0.0
    vol = horizon.get("realized_volatility") or {}
    try:
        per_min_vol_bps = float(vol.get("per_min_bps") or 0.0)
    except (TypeError, ValueError):
        per_min_vol_bps = 0.0
    model_p = directional_probability(open_price, spot, seconds_left, per_min_vol_bps)
    if model_p is None or open_price is None:
        return None
    avg60 = brti_avg60_from_horizon(horizon)
    if avg60 is None and brti_avg60 is not None and brti_avg60 > 0.0:
        avg60 = float(brti_avg60)
    aligned_spot, aligned_source = settlement_aligned_spot(spot, avg60, seconds_left)
    p_brti = model_p
    if aligned_spot is not None and aligned_source and aligned_source.startswith("brti"):
        p_aligned = directional_probability(
            open_price, aligned_spot, seconds_left, per_min_vol_bps)
        if p_aligned is not None:
            p_brti = p_aligned
    lag = lag_flags_from_horizon(horizon, contract)
    if now_ms is None:
        try:
            now_ms = int(snapshot.get("observed_at_ms") or 0) or None
        except (TypeError, ValueError):
            now_ms = None
    ablations = ablation_probabilities(
        model_p, yes_mid, lag["lead_confirms_direction"], lag["lead_conflicts"],
        p_brti_avg60=p_brti, per_min_vol_bps=per_min_vol_bps, now_ms=now_ms)
    minutes_left = seconds_left / 60.0
    session = oif.session_regime_at(now_ms) if now_ms is not None else "unknown"
    features = {
        "open_price": open_price,
        "spot": spot,
        "per_min_vol_bps": per_min_vol_bps,
        "sqrt_minutes_left": math.sqrt(minutes_left),
        "signed_distance_bps": (spot - open_price) / open_price * 10000.0,
        "yes_mid": yes_mid,
        "model_p": model_p,
        "venue_lead_bps_30s": lag["venue_lead_bps_30s"],
        "mid_lag_cents_30s": lag["mid_lag_cents_30s"],
        "lead_confirms_direction": lag["lead_confirms_direction"],
        "lead_conflicts": lag["lead_conflicts"],
        "p_veto_on_conflict": ablations["physics_veto_on_conflict"],
        "p_confirm_only": ablations["physics_confirm_only"],
        "brti_avg_60s": avg60,
        "settlement_aligned_spot": aligned_spot,
        "settlement_aligned_source": aligned_source,
        "p_brti_avg60": ablations["physics_brti_avg60"],
        "session_regime": session,
        "vol_regime": oif.vol_regime(per_min_vol_bps),
        "p_vol_regime_confirm": ablations["physics_vol_regime_confirm"],
    }
    return {
        "p_model": model_p,
        "p_ablations": ablations,
        "yes_mid": yes_mid,
        "features": features,
        "open_price": open_price,
    }


def extract_book(snapshot):
    """Passthrough of daemon top-of-book — same keys as spot_calibrator."""
    execution = snapshot.get("execution") or {}
    book = {}
    for side in ("yes", "no"):
        quote = execution.get(side) or {}
        for level in ("bid", "ask"):
            try:
                price = float(quote.get(level))
            except (TypeError, ValueError):
                continue
            if 0.0 < price < 1.0:
                book["market_%s_%s" % (side, level)] = price
    return book


def brier(pairs):
    if not pairs:
        return None
    return sum((p - (1.0 if y else 0.0)) ** 2 for p, y in pairs) / len(pairs)


def mean_or_none(values):
    if not values:
        return None
    return sum(values) / len(values)


def default_state():
    return {
        "schema": STATE_SCHEMA,
        "pending": {},
        "contract_scores_full": [],
        "contract_scores_market_mid_raw": [],
        "contract_scores_physics_veto_on_conflict": [],
        "contract_scores_physics_confirm_only": [],
        "contract_scores_physics_brti_avg60": [],
        "contract_scores_physics_vol_regime_confirm": [],
        "resolved": 0,
        "observation_count": 0,
    }


def load_state(path=STATE_PATH):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            state = json.load(fh)
    except (OSError, ValueError):
        return default_state()
    if not isinstance(state, dict):
        return default_state()
    schema = state.get("schema")
    if schema not in (1, 2, 3, STATE_SCHEMA):
        # Unknown / future schema: refuse to reinterpret; start clean.
        return default_state()
    state["schema"] = STATE_SCHEMA
    state.setdefault("pending", {})
    state.setdefault("contract_scores_full", [])
    state.setdefault("contract_scores_market_mid_raw", [])
    state.setdefault("contract_scores_physics_veto_on_conflict", [])
    state.setdefault("contract_scores_physics_confirm_only", [])
    state.setdefault("contract_scores_physics_brti_avg60", [])
    state.setdefault("contract_scores_physics_vol_regime_confirm", [])
    state.setdefault("resolved", 0)
    state.setdefault("observation_count", 0)
    return state


def save_json_atomic(payload, path):
    directory = os.path.dirname(path) or "."
    os.makedirs(directory, exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, separators=(",", ":"))
        fh.write("\n")
    os.replace(tmp, path)


def brti_avg60_at(samples, ts_ms, max_gap_ms=5000):
    """Nearest BRTI avg60 at ts_ms, or None when hole / missing avg."""
    sample = brti_nearest(samples, ts_ms, max_gap_ms=max_gap_ms)
    if sample is None:
        return None
    _ts, _spot, avg60 = sample
    return avg60 if avg60 is not None and avg60 > 0.0 else None


def _http_json(url, timeout=12, fetcher=None):
    if fetcher is not None:
        return fetcher(url)
    req = urllib.request.Request(
        url, headers={"Accept": "application/json", "User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _dollars(market, *keys):
    for key in keys:
        raw = market.get(key)
        if raw is None:
            continue
        try:
            value = float(raw)
        except (TypeError, ValueError):
            continue
        if value > 0.0:
            return value
    return None


def yes_mid_from_market(market):
    bid = _dollars(market, "yes_bid_dollars", "yes_bid")
    ask = _dollars(market, "yes_ask_dollars", "yes_ask")
    if bid is not None and ask is not None and 0.0 < bid < 1.0 and 0.0 < ask < 1.0:
        return (bid + ask) / 2.0
    # Derive YES mid from NO book when the list endpoint omits YES quotes.
    no_bid = _dollars(market, "no_bid_dollars", "no_bid")
    no_ask = _dollars(market, "no_ask_dollars", "no_ask")
    if no_bid is not None and no_ask is not None and 0.0 < no_bid < 1.0 and 0.0 < no_ask < 1.0:
        yes_bid = 1.0 - no_ask
        yes_ask = 1.0 - no_bid
        if 0.0 < yes_bid < 1.0 and 0.0 < yes_ask < 1.0:
            return (yes_bid + yes_ask) / 2.0
    last = _dollars(market, "last_price_dollars", "last_price")
    if last is not None and 0.0 < last < 1.0:
        return last
    return None


def open_price_from_market(market):
    for key in ("floor_strike", "cap_strike"):
        try:
            value = float(market.get(key) or 0.0)
        except (TypeError, ValueError):
            continue
        if value > 0.0:
            return value
    return None


def per_min_vol_bps_from_brti(samples):
    """Realized per-minute vol (bps) from BRTI (ts, spot, avg60) samples."""
    if not samples:
        return None
    series = [(int(ts), float(spot)) for ts, spot, _avg in samples if spot and spot > 0.0]
    if len(series) < 6:
        return None
    cutoff = series[-1][0] - 60 * 60 * 1000
    window = [p for ts, p in series if ts >= cutoff]
    if len(window) < 8:
        window = [p for _, p in series[-30:]]
    rets = []
    for i in range(1, len(window)):
        a, b = window[i - 1], window[i]
        if a > 0.0 and b > 0.0:
            rets.append(math.log(b / a))
    if len(rets) < 5:
        return None
    mean = sum(rets) / len(rets)
    var = sum((r - mean) ** 2 for r in rets) / (len(rets) - 1)
    return math.sqrt(max(var, 0.0)) * 10000.0


def fetch_open_kxbtc15m_markets(fetcher=None):
    """Open KXBTC15M markets from Kalshi REST (fills WS universe gaps)."""
    try:
        payload = _http_json(
            KALSHI_MARKETS_URL.format(series=FAMILY), fetcher=fetcher)
    except (OSError, ValueError):
        return []
    markets = []
    for market in payload.get("markets") or []:
        ticker = (market.get("ticker") or "").upper()
        if not is_kxbtc15m_ticker(ticker):
            continue
        # List endpoint often omits quotes right after rollover — hydrate.
        if yes_mid_from_market(market) is None:
            try:
                detail = _http_json(
                    KALSHI_MARKET_URL.format(ticker=ticker), fetcher=fetcher)
                if isinstance(detail, dict) and isinstance(detail.get("market"), dict):
                    market = detail["market"]
            except (OSError, ValueError):
                pass
        markets.append(market)
    return markets


def observation_from_rest(market, brti_samples, now_ms):
    """Score an open race from REST + BRTI when the WS snapshot is missing."""
    ticker = (market.get("ticker") or "").upper()
    if not is_kxbtc15m_ticker(ticker):
        return None
    close_ms = parse_close_ms(ticker)
    if close_ms is None or now_ms >= close_ms:
        return None
    seconds_left = int((close_ms - now_ms) / 1000)
    if seconds_left <= 0:
        return None
    yes_mid = yes_mid_from_market(market)
    open_price = open_price_from_market(market)
    if yes_mid is None or open_price is None or not 0.0 < yes_mid < 1.0:
        return None
    sample = brti_nearest(brti_samples, now_ms, max_gap_ms=120_000)
    if sample is None:
        return None
    _ts, spot, avg60 = sample
    if spot is None or spot <= 0.0:
        return None
    per_min_vol_bps = per_min_vol_bps_from_brti(brti_samples)
    if per_min_vol_bps is None or per_min_vol_bps <= 0.0:
        return None
    model_p = directional_probability(open_price, spot, seconds_left, per_min_vol_bps)
    if model_p is None:
        return None
    aligned_spot, aligned_source = settlement_aligned_spot(spot, avg60, seconds_left)
    p_brti = model_p
    if aligned_spot is not None and aligned_source and aligned_source.startswith("brti"):
        p_aligned = directional_probability(
            open_price, aligned_spot, seconds_left, per_min_vol_bps)
        if p_aligned is not None:
            p_brti = p_aligned
    # REST has no venue-lead path — lag ablations collapse to physics/mid.
    ablations = ablation_probabilities(
        model_p, yes_mid, lead_confirms=False, lead_conflicts=False,
        p_brti_avg60=p_brti, per_min_vol_bps=per_min_vol_bps, now_ms=now_ms)
    minutes_left = seconds_left / 60.0
    features = {
        "open_price": open_price,
        "spot": spot,
        "per_min_vol_bps": per_min_vol_bps,
        "sqrt_minutes_left": math.sqrt(minutes_left),
        "signed_distance_bps": (spot - open_price) / open_price * 10000.0,
        "yes_mid": yes_mid,
        "model_p": model_p,
        "venue_lead_bps_30s": 0.0,
        "mid_lag_cents_30s": 0.0,
        "lead_confirms_direction": False,
        "lead_conflicts": False,
        "p_veto_on_conflict": ablations["physics_veto_on_conflict"],
        "p_confirm_only": ablations["physics_confirm_only"],
        "brti_avg_60s": avg60,
        "settlement_aligned_spot": aligned_spot,
        "settlement_aligned_source": aligned_source,
        "p_brti_avg60": ablations["physics_brti_avg60"],
        "session_regime": oif.session_regime_at(now_ms),
        "vol_regime": oif.vol_regime(per_min_vol_bps),
        "p_vol_regime_confirm": ablations["physics_vol_regime_confirm"],
        "source": "rest+brti",
    }
    return {
        "ticker": ticker,
        "p_model": model_p,
        "p_ablations": ablations,
        "yes_mid": yes_mid,
        "features": features,
        "open_price": open_price,
    }


def _record_live_prediction(state, predictions, ticker, obs, now_ms, book=None):
    close_ms = parse_close_ms(ticker)
    if close_ms is None:
        return
    entry = state["pending"].setdefault(
        ticker, {"close_ms": close_ms, "open_price": obs["open_price"], "obs": []})
    # Pin the open from the first observation; a later rewrite would let
    # settlement drift redefine the race mid-window.
    if not entry.get("open_price"):
        entry["open_price"] = obs["open_price"]
    entry["close_ms"] = close_ms
    if len(entry["obs"]) < MAX_OBS_PER_TICKER:
        ablations = obs.get("p_ablations") or {
            "physics": obs["p_model"],
            "physics_veto_on_conflict": obs["p_model"],
            "physics_confirm_only": obs["p_model"],
            "physics_brti_avg60": obs["p_model"],
            "physics_vol_regime_confirm": obs["p_model"],
        }
        entry["obs"].append({
            "p_model": obs["p_model"],
            "p_ablations": ablations,
            "yes_mid": obs["yes_mid"],
            "features": obs["features"],
            "ts_ms": now_ms,
        })
        state["observation_count"] = int(state.get("observation_count") or 0) + 1
    trusted_variant = select_trusted_variant(state)
    live_p = obs["p_model"]
    if trusted_variant and trusted_variant in (obs.get("p_ablations") or {}):
        live_p = obs["p_ablations"][trusted_variant]
    prediction = {
        "p_yes_full": live_p,
        "p_yes_market_baseline": obs["yes_mid"],
        "market_yes_mid": obs["yes_mid"],
        "edge": live_p - obs["yes_mid"],
        "features": obs["features"],
        "probability_source": "kxbtc15m-directional-gaussian",
        "ablation_variant": trusted_variant or "physics",
    }
    if book:
        prediction.update(book)
    predictions[ticker] = prediction


def observe_cycle(state, evidence, now_ms, brti_samples=None, rest_markets=None,
                  rest_fetcher=None):
    """Score every open KXBTC15M snapshot; return prediction map for the report.

    Pass `brti_samples=[]` to skip disk I/O (unit tests). Production `run_once`
    loads a trailing BRTI window once per cycle.

    When the daemon has not yet subscribed the next open window (common for a
    minute at each 15m rollover), fill from Kalshi REST + BRTI so FLOW does not
    go blank between races.
    """
    snapshots = (evidence or {}).get("snapshots") or {}
    predictions = {}
    if brti_samples is None:
        brti_samples = load_brti_samples()
    avg60_now = brti_avg60_at(brti_samples, now_ms)
    for ticker, snapshot in snapshots.items():
        obs = extract_observation(
            ticker, snapshot, brti_avg60=avg60_now, now_ms=now_ms)
        if obs is None:
            continue
        _record_live_prediction(
            state, predictions, ticker, obs, now_ms, book=extract_book(snapshot))
    if not predictions:
        if rest_markets is None:
            rest_markets = fetch_open_kxbtc15m_markets(fetcher=rest_fetcher)
        for market in rest_markets:
            obs = observation_from_rest(market, brti_samples, now_ms)
            if obs is None:
                continue
            _record_live_prediction(state, predictions, obs["ticker"], obs, now_ms)
    return predictions


def _iter_jsonl(path):
    if not os.path.exists(path):
        return
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                yield json.loads(line)
            except ValueError:
                continue


def _iter_jsonl_tail(path, max_lines=8000):
    """Yield up to the last max_lines non-empty JSONL records (cheap for 50MB+)."""
    if not os.path.exists(path) or max_lines <= 0:
        return
    try:
        with open(path, "rb") as fh:
            fh.seek(0, os.SEEK_END)
            size = fh.tell()
            # ~400 bytes/line typical for CF ticks; over-read then trim.
            chunk = min(size, max_lines * 512)
            fh.seek(max(0, size - chunk), os.SEEK_SET)
            data = fh.read().decode("utf-8", errors="replace")
    except OSError:
        return
    lines = [ln for ln in data.splitlines() if ln.strip()]
    if size > chunk and lines:
        lines = lines[1:]  # drop possibly truncated first line
    for line in lines[-max_lines:]:
        try:
            yield json.loads(line)
        except ValueError:
            continue


def load_brti_samples(max_lines=8000):
    """(ts_ms, spot, avg60) ascending, from live log + rotation.

    Reads only a trailing window — the live CF log is tens of MB / 100k+ lines
    and must not be fully scanned every 60s calibrator cycle.
    """
    samples = []
    for path in (BRTI_PATH_ROTATION, BRTI_PATH):
        for record in _iter_jsonl_tail(path, max_lines=max_lines):
            if record.get("id") != "BRTI":
                continue
            try:
                ts_ms = int(record["time"])
                spot = float(record["value"])
            except (KeyError, TypeError, ValueError):
                continue
            avg = (record.get("avg_60s_data") or {}).get("value")
            try:
                avg60 = float(avg) if avg is not None else None
            except (TypeError, ValueError):
                avg60 = None
            samples.append((ts_ms, spot, avg60))
    samples.sort()
    return samples


def brti_nearest(samples, ts_ms, max_gap_ms=5000):
    if not samples:
        return None
    # Binary search for nearest.
    lo, hi = 0, len(samples) - 1
    while lo < hi:
        mid = (lo + hi) // 2
        if samples[mid][0] < ts_ms:
            lo = mid + 1
        else:
            hi = mid
    candidates = [i for i in (lo - 1, lo) if 0 <= i < len(samples)]
    best = min(candidates, key=lambda i: abs(samples[i][0] - ts_ms))
    if abs(samples[best][0] - ts_ms) > max_gap_ms:
        return None
    return samples[best]


def recorded_result(ticker, fetcher=None):
    """YES/NO from public settlement feed, then REST. None when unknown."""
    for record in _iter_jsonl(SETTLEMENTS_PATH):
        if record.get("kalshi_market_id") != ticker:
            continue
        result = (record.get("result") or "").lower()
        if result == "yes":
            return True
        if result == "no":
            return False
    if fetcher is None:
        def fetcher(url):
            with urllib.request.urlopen(url, timeout=10) as resp:
                return json.loads(resp.read().decode("utf-8"))
    try:
        payload = fetcher(KALSHI_MARKET_URL.format(ticker=ticker))
        market = payload.get("market") or payload
        result = (market.get("result") or "").lower()
        if result == "yes":
            return True
        if result == "no":
            return False
    except (OSError, ValueError, KeyError):
        return None
    return None


def derive_outcome(ticker, open_price, brti_samples, close_ms=None):
    """True/False from BRTI 60s average at close vs window open. None if hole."""
    if open_price is None or open_price <= 0.0:
        return None
    close_ms = close_ms if close_ms is not None else parse_close_ms(ticker)
    if close_ms is None:
        return None
    sample = brti_nearest(brti_samples, close_ms)
    if sample is None:
        return None
    _, spot, avg60 = sample
    value = avg60 if avg60 is not None else spot
    return value > open_price


def resolve_outcome(ticker, open_price, brti_samples=None, fetcher=None):
    recorded = recorded_result(ticker, fetcher=fetcher)
    if recorded is not None:
        return recorded, "recorded"
    if brti_samples is None:
        brti_samples = load_brti_samples()
    derived = derive_outcome(ticker, open_price, brti_samples)
    if derived is None:
        return None, None
    return derived, "derived"


def settle_cycle(state, now_ms, resolver=None, brti_samples=None):
    """Score pending contracts whose close + grace has elapsed.

    One score per contract: mean squared error over that contract's own
    observations (physics p and raw mid), before any further use. Equal weight
    per contract.
    """
    if resolver is None:
        if brti_samples is None:
            brti_samples = load_brti_samples()

        def resolver(ticker, open_price):
            outcome, _source = resolve_outcome(ticker, open_price, brti_samples)
            return outcome

    for ticker in list(state["pending"].keys()):
        entry = state["pending"][ticker]
        close_ms = int(entry.get("close_ms") or 0)
        if now_ms < close_ms + 120_000:
            continue
        observations = entry.get("obs") or []
        if not observations:
            del state["pending"][ticker]
            continue
        outcome = resolver(ticker, entry.get("open_price"))
        if outcome is None:
            if now_ms > close_ms + 24 * 3600 * 1000:
                del state["pending"][ticker]
            continue
        state["contract_scores_full"].append(
            brier([(obs["p_model"], outcome) for obs in observations]))
        state["contract_scores_market_mid_raw"].append(
            brier([(obs["yes_mid"], outcome) for obs in observations]))
        veto_pairs = []
        confirm_pairs = []
        brti_pairs = []
        vol_pairs = []
        for obs in observations:
            ablations = obs.get("p_ablations") or {}
            veto_pairs.append((
                ablations.get("physics_veto_on_conflict", obs["p_model"]), outcome))
            confirm_pairs.append((
                ablations.get("physics_confirm_only", obs["p_model"]), outcome))
            brti_pairs.append((
                ablations.get("physics_brti_avg60", obs["p_model"]), outcome))
            vol_pairs.append((
                ablations.get("physics_vol_regime_confirm", obs["p_model"]), outcome))
        state["contract_scores_physics_veto_on_conflict"].append(brier(veto_pairs))
        state["contract_scores_physics_confirm_only"].append(brier(confirm_pairs))
        state["contract_scores_physics_brti_avg60"].append(brier(brti_pairs))
        state["contract_scores_physics_vol_regime_confirm"].append(brier(vol_pairs))
        state["resolved"] = int(state.get("resolved") or 0) + 1
        del state["pending"][ticker]
    for key in (
        "contract_scores_full",
        "contract_scores_market_mid_raw",
        "contract_scores_physics_veto_on_conflict",
        "contract_scores_physics_confirm_only",
        "contract_scores_physics_brti_avg60",
        "contract_scores_physics_vol_regime_confirm",
    ):
        state[key] = state[key][-SCORED_CONTRACT_WINDOW:]


def ablation_scoreboard(state):
    """Per-variant Brier vs mid, paired on the overlapping trailing window."""
    return oif.paired_ablation_scoreboard(
        {
            "physics": state.get("contract_scores_full") or [],
            "physics_veto_on_conflict":
                state.get("contract_scores_physics_veto_on_conflict") or [],
            "physics_confirm_only":
                state.get("contract_scores_physics_confirm_only") or [],
            "physics_brti_avg60":
                state.get("contract_scores_physics_brti_avg60") or [],
            "physics_vol_regime_confirm":
                state.get("contract_scores_physics_vol_regime_confirm") or [],
        },
        state.get("contract_scores_market_mid_raw") or [],
        MIN_SCORED_CONTRACTS,
    )


def select_trusted_variant(state):
    """Best ablation that beats mid at ≥100 contracts; else None (fail-closed)."""
    board, _b_mid = ablation_scoreboard(state)
    return oif.select_best_trusted(board, ABLATION_KEYS)


def build_report(state, predictions, now_ms):
    scored = state.get("contract_scores_full") or []
    b_full = mean_or_none(scored)
    ablations, b_mid = ablation_scoreboard(state)
    trusted_variant = select_trusted_variant(state)
    adds_value = trusted_variant is not None
    return {
        "schema": STATE_SCHEMA,
        "event": "kxbtc15m_calibrator",
        "advisory_only": True,
        "family": FAMILY,
        "generated_at_ms": now_ms,
        "resolved_contracts": int(state.get("resolved") or 0),
        "scored_contracts": len(scored),
        "training_observations": int(state.get("observation_count") or 0),
        "scoring_rule": ("one score per KXBTC15M contract: mean squared error "
                         "over that contract's observations; physics is "
                         "P(close>open) Gaussian on last-print/independent spot; "
                         "lag ablations veto/confirm vs sticky mid; "
                         "physics_brti_avg60 uses CF BRTI 60s average "
                         "(settlement underlier); physics_vol_regime_confirm "
                         "clamps quiet/weekend to mid; market is raw yes mid"),
        "min_scored_contracts": MIN_SCORED_CONTRACTS,
        "brier_full": b_full,
        "brier_market_mid_raw": b_mid,
        "ablations": ablations,
        "trusted_variant": trusted_variant,
        "adds_value_over_market": adds_value,
        "predictions": predictions,
    }


def run_once(now_ms=None):
    now_ms = now_ms if now_ms is not None else int(time.time() * 1000)
    try:
        with open(EVIDENCE_PATH, "r", encoding="utf-8") as fh:
            evidence = json.load(fh)
    except (OSError, ValueError):
        return {"error": "evidence file unavailable", "path": EVIDENCE_PATH}
    state = load_state()
    predictions = observe_cycle(state, evidence, now_ms)
    settle_cycle(state, now_ms)
    save_json_atomic(state, STATE_PATH)
    report = build_report(state, predictions, now_ms)
    save_json_atomic(report, OUTPUT_PATH)
    return report


def main(argv):
    command = argv[1] if len(argv) > 1 else "once"
    if command == "once":
        print(json.dumps(run_once()))
        return 0
    if command == "report":
        state = load_state()
        print(json.dumps(build_report(state, {}, int(time.time() * 1000))))
        return 0
    if command == "run":
        interval = 60
        if "--interval" in argv:
            interval = max(10, int(argv[argv.index("--interval") + 1]))
        while True:
            result = run_once()
            print(json.dumps({
                "cycle": result.get("generated_at_ms"),
                "resolved": result.get("resolved_contracts"),
                "scored": result.get("scored_contracts"),
                "adds_value_over_market": result.get("adds_value_over_market"),
                "error": result.get("error"),
            }), flush=True)
            time.sleep(interval)
    print("usage: kxbtc15m_calibrator.py [once|run [--interval N]|report]",
          file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
