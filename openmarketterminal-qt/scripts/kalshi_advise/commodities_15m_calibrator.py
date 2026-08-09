#!/usr/bin/env python3
"""Directional probability calibrator for Kalshi commodities 15m races.

Advisory-only. Same physics as kxbtc15m_calibrator — remaining-time Gaussian
P(close > open) — applied to:

    KXGOLD15M  (Gold up/down 15m)
    KXSILVER15M
    KXWTI15M

Why a separate report
---------------------
Trust must not leak across families. Hourly threshold calibrator.json and
kxbtc15m-calibrator.json stay BTC-owned. This module writes
commodities-15m-calibrator.json and flips `adds_value_over_market` only when
its own per-contract Brier beats the raw mid over ≥100 settled contracts.

Spot / settlement honesty
-------------------------
Kalshi settles these on the 1-minute candlestick close of the underlier
(Pyth). Live spot + realized vol prefer Hermes Core metals:

    GOLD   → Metal.XAU/USD
    SILVER → Metal.XAG/USD
    WTI    → Metal.XTI/USD

Yahoo futures (GC=F / SI=F / CL=F) are a cold fallback when Pyth is down —
not a claim that Yahoo ≡ Pyth. Outcomes prefer Kalshi's recorded YES/NO;
derived close uses Pyth series first, then Yahoo. Report
`settlement_parity` measures Pyth-derived direction vs recorded results.

The daemon's kalshi-ws-books.json is used when it already carries a commodity
snapshot (horizon.spot / floor_strike); otherwise markets are pulled from the
public Kalshi REST API so the scoreboard can accumulate without waiting on
daemon universe expansion.

Commands:  once | run [--interval 60] | report
"""
from __future__ import annotations

import datetime
import json
import math
import os
import sys
import time
import urllib.parse
import urllib.request
import zoneinfo

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from openterminal_paths import evidence_file

# Reuse the BTC 15m race math — identical P(close>open) construction.
import kxbtc15m_calibrator as race
import calibrator_eligibility as ce
import outside_info_features as oif

EVIDENCE_PATH = evidence_file("kalshi-ws-books.json")
STATE_PATH = evidence_file("commodities-15m-calibrator-state.json")
OUTPUT_PATH = evidence_file("commodities-15m-calibrator.json")
SETTLEMENTS_PATH = evidence_file("kalshi-settlements.jsonl")
KALSHI_MARKETS_URL = (
    "https://api.elections.kalshi.com/trade-api/v2/markets"
    "?series_ticker={series}&status=open&limit=20"
)
KALSHI_MARKET_URL = "https://api.elections.kalshi.com/trade-api/v2/markets/{ticker}"
YAHOO_CHART_URL = (
    "https://query1.finance.yahoo.com/v8/finance/chart/{symbol}"
    "?interval=1m&range=1d"
)
PYTH_HERMES_LATEST = "https://hermes.pyth.network/v2/updates/price/latest"

FAMILIES = {
    "KXGOLD15M": {
        "yahoo": "GC=F",
        "label": "gold",
        "pyth_symbol": "Metal.XAU/USD",
        "pyth_id": "765d2ba906dbc32ca17cc11f5310a89e9ee1f6420508c63861f2f8ba4ee34bb2",
    },
    "KXSILVER15M": {
        "yahoo": "SI=F",
        "label": "silver",
        "pyth_symbol": "Metal.XAG/USD",
        "pyth_id": "f2fb02c32b055c805e7238d628e5e9dadef274376114eb1f012337cabe93871e",
    },
    "KXWTI15M": {
        "yahoo": "CL=F",
        "label": "wti",
        "pyth_symbol": "Metal.XTI/USD",
        "pyth_id": "a35b407f0fa4b027c2dfa8dff0b7b99b853fb4d326a9e9906271933237b90c1c",
    },
}
STATE_SCHEMA = 3
MAX_OBS_PER_TICKER = race.MAX_OBS_PER_TICKER
SCORED_CONTRACT_WINDOW = race.SCORED_CONTRACT_WINDOW
MIN_SCORED_CONTRACTS = race.MIN_SCORED_CONTRACTS
PYTH_SERIES_MAX_POINTS = 180  # ~3h at 1/min poll cadence
EASTERN = zoneinfo.ZoneInfo("America/New_York")
CLOSE_FOLD = 0
UA = "OpenTerminal/commodities-15m-calibrator"
ABLATION_KEYS = (
    "physics",
    "physics_tape_confirm_near_close",
    "physics_vol_regime_confirm",
)


def is_commodity_15m_ticker(ticker):
    if not isinstance(ticker, str) or not ticker:
        return False
    return ticker.split("-", 1)[0] in FAMILIES


def family_of(ticker):
    if not isinstance(ticker, str) or not ticker:
        return None
    family = ticker.split("-", 1)[0]
    return family if family in FAMILIES else None


def parse_close_ms(ticker):
    """Same YYMONDDHHMM Eastern close token as KXBTC15M."""
    return race.parse_close_ms(ticker)


def _http_json(url, timeout=12):
    req = urllib.request.Request(url, headers={"Accept": "application/json", "User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def parse_pyth_price(parsed_item):
    """Hermes parsed feed → (price, conf, publish_ms). None on refuse."""
    if not isinstance(parsed_item, dict):
        return None
    price_obj = parsed_item.get("price") or {}
    try:
        raw = float(price_obj["price"])
        expo = int(price_obj["expo"])
        publish_s = int(price_obj["publish_time"])
    except (KeyError, TypeError, ValueError):
        return None
    price = raw * (10.0 ** expo)
    if price <= 0.0:
        return None
    try:
        conf = float(price_obj.get("conf") or 0.0) * (10.0 ** expo)
    except (TypeError, ValueError):
        conf = 0.0
    return price, conf, publish_s * 1000


def fetch_pyth_latest(feed_ids, fetcher=None):
    """Map bare hex feed id → (price, conf, publish_ms). Empty on failure."""
    ids = [fid for fid in feed_ids if fid]
    if not ids:
        return {}
    query = "&".join("ids[]=%s" % urllib.parse.quote(fid, safe="") for fid in ids)
    url = "%s?%s" % (PYTH_HERMES_LATEST, query)
    try:
        payload = fetcher(url) if fetcher is not None else _http_json(url)
    except (OSError, ValueError, KeyError):
        return {}
    out = {}
    for item in payload.get("parsed") or []:
        parsed = parse_pyth_price(item)
        if parsed is None:
            continue
        feed_id = str(item.get("id") or "").lower().lstrip("0x")
        if feed_id:
            out[feed_id] = parsed
    return out


def append_pyth_tick(series_map, symbol, price, publish_ms):
    """Persist one Pyth tick into state-backed series (capped)."""
    if not symbol or price is None or price <= 0.0 or publish_ms is None:
        return
    series = series_map.setdefault(symbol, [])
    if series and int(series[-1][0]) == int(publish_ms):
        series[-1] = [int(publish_ms), float(price)]
    else:
        series.append([int(publish_ms), float(price)])
    if len(series) > PYTH_SERIES_MAX_POINTS:
        del series[:-PYTH_SERIES_MAX_POINTS]


def refresh_pyth_series(series_map, fetcher=None):
    """Poll Hermes for all families; mutate series_map; return id→tick map."""
    id_to_symbol = {}
    for meta in FAMILIES.values():
        fid = str(meta["pyth_id"]).lower().lstrip("0x")
        id_to_symbol[fid] = meta["pyth_symbol"]
    ticks = fetch_pyth_latest(list(id_to_symbol.keys()), fetcher=fetcher)
    for feed_id, (price, _conf, publish_ms) in ticks.items():
        symbol = id_to_symbol.get(feed_id) or id_to_symbol.get(feed_id.lstrip("0"))
        if symbol:
            append_pyth_tick(series_map, symbol, price, publish_ms)
    return ticks


def fetch_yahoo_series(symbol):
    """Ascending (ts_ms, close) from Yahoo 1m chart. Empty on failure."""
    try:
        payload = _http_json(YAHOO_CHART_URL.format(symbol=urllib.parse.quote(symbol, safe="")))
    except (OSError, ValueError, KeyError):
        return []
    result = ((payload.get("chart") or {}).get("result") or [None])[0] or {}
    timestamps = result.get("timestamp") or []
    closes = (((result.get("indicators") or {}).get("quote") or [{}])[0].get("close") or [])
    out = []
    for ts, close in zip(timestamps, closes):
        try:
            ts_ms = int(ts) * 1000
            price = float(close)
        except (TypeError, ValueError):
            continue
        if price > 0.0:
            out.append((ts_ms, price))
    out.sort()
    return out


def per_min_vol_bps_from_series(series):
    """Realized per-minute vol (bps) from a (ts, price) series. None if thin."""
    if not series:
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


def spot_and_vol_for_family(family, pyth_series, yahoo_cache):
    """Prefer Pyth spot/vol; Yahoo cold fallback. Returns (spot, vol, source)."""
    meta = FAMILIES.get(family) or {}
    pyth_symbol = meta.get("pyth_symbol")
    yahoo_symbol = meta.get("yahoo")
    pyth_pts = list(pyth_series.get(pyth_symbol) or []) if pyth_symbol else []
    spot = None
    source = None
    if pyth_pts:
        spot = float(pyth_pts[-1][1])
        source = "pyth:%s" % pyth_symbol
        vol = per_min_vol_bps_from_series([(int(t), float(p)) for t, p in pyth_pts])
        if vol is not None and spot > 0.0:
            return spot, vol, source
    if yahoo_symbol:
        if yahoo_symbol not in yahoo_cache:
            yahoo_cache[yahoo_symbol] = fetch_yahoo_series(yahoo_symbol)
        yahoo = yahoo_cache[yahoo_symbol]
        if yahoo:
            y_spot = yahoo[-1][1]
            y_vol = per_min_vol_bps_from_series(yahoo)
            if spot is None or spot <= 0.0:
                spot = y_spot
                source = "yahoo:%s" % yahoo_symbol
            if y_vol is not None:
                # Spot may still be Pyth with Yahoo vol when Pyth history is thin.
                if source and source.startswith("pyth:"):
                    source = "%s+yahoo_vol" % source
                elif not source:
                    source = "yahoo:%s" % yahoo_symbol
                return spot, y_vol, source
    if spot is not None and spot > 0.0:
        return spot, None, source or "unknown"
    return None, None, None


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
    last = _dollars(market, "last_price_dollars", "last_price")
    if last is not None and 0.0 < last < 1.0:
        return last
    return None


def seconds_left_from_market(market, now_ms):
    close = market.get("close_time") or market.get("expected_expiration_time")
    if not close:
        return None
    try:
        # Kalshi uses Zulu ISO.
        dt = datetime.datetime.fromisoformat(str(close).replace("Z", "+00:00"))
        return int((dt.timestamp() * 1000 - now_ms) / 1000)
    except (TypeError, ValueError):
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


def fetch_open_commodity_markets():
    markets = []
    for series in FAMILIES:
        try:
            payload = _http_json(KALSHI_MARKETS_URL.format(series=series))
        except (OSError, ValueError):
            continue
        for market in payload.get("markets") or []:
            ticker = (market.get("ticker") or "").upper()
            if is_commodity_15m_ticker(ticker):
                markets.append(market)
    return markets


def _enrich_phase3_features(features, open_price, spot, yes_mid, model_p,
                            seconds_left, vol, now_ms, yahoo_series=None):
    """Attach futures-tape + session/vol features and ablation probabilities."""
    session = oif.session_regime_at(now_ms)
    vreg = oif.vol_regime(vol)
    tape_bps = oif.series_change_bps(yahoo_series or [], now_ms)
    tape = oif.futures_tape_flags(spot, open_price, tape_bps)
    p_tape = oif.ablation_tape_confirm_near_close(
        model_p, yes_mid, seconds_left, tape["tape_confirms"])
    p_vol = oif.ablation_vol_regime_confirm(model_p, yes_mid, vol, session=session)
    ablations = {
        "physics": float(model_p),
        "physics_tape_confirm_near_close": float(p_tape),
        "physics_vol_regime_confirm": float(p_vol),
    }
    features.update({
        "session_regime": session,
        "vol_regime": vreg,
        "tape_change_bps": tape["tape_change_bps"],
        "tape_confirms": tape["tape_confirms"],
        "tape_conflicts": tape["tape_conflicts"],
        "tape_available": tape["tape_available"],
        "p_tape_confirm_near_close": p_tape,
        "p_vol_regime_confirm": p_vol,
    })
    return ablations


def observation_from_rest(market, yahoo_cache, now_ms, pyth_series=None):
    ticker = (market.get("ticker") or "").upper()
    family = family_of(ticker)
    if family is None:
        return None
    open_price = open_price_from_market(market)
    yes_mid = yes_mid_from_market(market)
    seconds_left = seconds_left_from_market(market, now_ms)
    if open_price is None or yes_mid is None or seconds_left is None or seconds_left <= 0:
        return None
    if not 0.0 < yes_mid < 1.0:
        return None
    if pyth_series is None:
        pyth_series = {}
    spot, vol, spot_source = spot_and_vol_for_family(family, pyth_series, yahoo_cache)
    if spot is None or spot <= 0.0:
        return None
    if vol is None or vol <= 0.0:
        # Refuse fabricated residual vol — ATM would become a coin flip.
        return None
    model_p = race.directional_probability(open_price, spot, seconds_left, vol)
    if model_p is None:
        return None
    minutes_left = seconds_left / 60.0
    yahoo_symbol = FAMILIES[family]["yahoo"]
    if yahoo_symbol not in yahoo_cache:
        yahoo_cache[yahoo_symbol] = fetch_yahoo_series(yahoo_symbol)
    features = {
        "open_price": open_price,
        "spot": spot,
        "per_min_vol_bps": vol,
        "sqrt_minutes_left": math.sqrt(minutes_left),
        "signed_distance_bps": (spot - open_price) / open_price * 10000.0,
        "yes_mid": yes_mid,
        "model_p": model_p,
        "underlier": FAMILIES[family]["label"],
        "spot_source": spot_source,
        "settlement_source": "pyth:%s" % FAMILIES[family]["pyth_symbol"],
    }
    ablations = _enrich_phase3_features(
        features, open_price, spot, yes_mid, model_p, seconds_left, vol, now_ms,
        yahoo_series=yahoo_cache.get(yahoo_symbol) or [])
    bid = _dollars(market, "yes_bid_dollars", "yes_bid")
    ask = _dollars(market, "yes_ask_dollars", "yes_ask")
    book = {}
    if bid is not None and 0.0 < bid < 1.0:
        book["market_yes_bid"] = bid
    if ask is not None and 0.0 < ask < 1.0:
        book["market_yes_ask"] = ask
    return {
        "ticker": ticker,
        "p_model": model_p,
        "p_ablations": ablations,
        "yes_mid": yes_mid,
        "features": features,
        "open_price": open_price,
        "book": book,
    }


def observation_from_evidence(ticker, snapshot, now_ms=None, yahoo_cache=None):
    """Daemon path — same fields as kxbtc15m when the universe carries the book."""
    if not is_commodity_15m_ticker(ticker):
        return None
    # kxbtc15m.extract_observation hard-filters KXBTC15M; pull fields locally.
    contract = snapshot.get("contract") or {}
    horizon = contract.get("horizon") or {}
    if now_ms is None:
        now_ms = int(time.time() * 1000)
    close_ms = parse_close_ms(ticker)
    if close_ms is not None:
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
    open_price = race.open_price_from_horizon(horizon)
    try:
        spot = float(horizon.get("spot") or 0.0)
    except (TypeError, ValueError):
        spot = 0.0
    vol = horizon.get("realized_volatility") or {}
    try:
        per_min_vol_bps = float(vol.get("per_min_bps") or 0.0)
    except (TypeError, ValueError):
        per_min_vol_bps = 0.0
    model_p = race.directional_probability(open_price, spot, seconds_left, per_min_vol_bps)
    if model_p is None or open_price is None:
        return None
    minutes_left = seconds_left / 60.0
    family = family_of(ticker)
    if yahoo_cache is None:
        yahoo_cache = {}
    yahoo_series = []
    if family in FAMILIES:
        ysym = FAMILIES[family]["yahoo"]
        if ysym not in yahoo_cache:
            yahoo_cache[ysym] = fetch_yahoo_series(ysym)
        yahoo_series = yahoo_cache.get(ysym) or []
    features = {
        "open_price": open_price,
        "spot": spot,
        "per_min_vol_bps": per_min_vol_bps,
        "sqrt_minutes_left": math.sqrt(minutes_left),
        "signed_distance_bps": (spot - open_price) / open_price * 10000.0,
        "yes_mid": yes_mid,
        "model_p": model_p,
        "underlier": FAMILIES.get(family, {}).get("label"),
        "spot_source": horizon.get("settlement_source") or "daemon",
        "settlement_source": "pyth:%s" % (FAMILIES.get(family, {}) or {}).get(
            "pyth_symbol", "unknown"),
    }
    ablations = _enrich_phase3_features(
        features, open_price, spot, yes_mid, model_p, seconds_left,
        per_min_vol_bps, now_ms, yahoo_series=yahoo_series)
    return {
        "ticker": ticker,
        "p_model": model_p,
        "p_ablations": ablations,
        "yes_mid": yes_mid,
        "features": features,
        "open_price": open_price,
        "book": race.extract_book(snapshot),
    }


def default_state():
    return {
        "schema": STATE_SCHEMA,
        "pending": {},
        "contract_scores_full": [],
        "contract_scores_market_mid_raw": [],
        "contract_scores_physics_tape_confirm_near_close": [],
        "contract_scores_physics_vol_regime_confirm": [],
        # Same contracts, scored over ONLY the observations where the model's
        # edge over the mid reached the bot's bid threshold -- the population
        # the trust flag is actually used to authorise. See
        # calibrator_eligibility.
        "contract_scores_eligible_full": [],
        "contract_scores_eligible_market_mid_raw": [],
        "contract_scores_eligible_physics_tape_confirm_near_close": [],
        "contract_scores_eligible_physics_vol_regime_confirm": [],
        # Each variant's OWN eligible mid population -- a contract can be
        # eligible for one variant and not another, so the variant's Brier
        # must be paired against the mid observed on ITS eligible contracts,
        # not physics's. (physics keeps using
        # contract_scores_eligible_market_mid_raw above.)
        "contract_scores_eligible_mid_physics_tape_confirm_near_close": [],
        "contract_scores_eligible_mid_physics_vol_regime_confirm": [],
        "resolved": 0,
        "observation_count": 0,
        "pyth_series": {},
        "settlement_parity": {"checked": 0, "matched": 0},
    }


def load_state(path=STATE_PATH):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            state = json.load(fh)
    except (OSError, ValueError):
        return default_state()
    if not isinstance(state, dict):
        return default_state()
    # Schema 1/2 → 3 keeps scored history; unknown future schemas refuse.
    schema = state.get("schema")
    if schema not in (1, 2, STATE_SCHEMA):
        return default_state()
    state["schema"] = STATE_SCHEMA
    state.setdefault("pending", {})
    state.setdefault("contract_scores_full", [])
    state.setdefault("contract_scores_market_mid_raw", [])
    state.setdefault("contract_scores_physics_tape_confirm_near_close", [])
    state.setdefault("contract_scores_physics_vol_regime_confirm", [])
    # Same hazard as the contract_scores_* keys above: a state file written
    # before this change predates these keys, so a bare index/append below
    # would KeyError on the first settlement.
    state.setdefault("contract_scores_eligible_full", [])
    state.setdefault("contract_scores_eligible_market_mid_raw", [])
    state.setdefault("contract_scores_eligible_physics_tape_confirm_near_close", [])
    state.setdefault("contract_scores_eligible_physics_vol_regime_confirm", [])
    # Same hazard again: each variant's own eligible-mid population.
    state.setdefault("contract_scores_eligible_mid_physics_tape_confirm_near_close", [])
    state.setdefault("contract_scores_eligible_mid_physics_vol_regime_confirm", [])
    state.setdefault("resolved", 0)
    state.setdefault("observation_count", 0)
    state.setdefault("pyth_series", {})
    state.setdefault("settlement_parity", {"checked": 0, "matched": 0})
    return state


def select_trusted_variant(state):
    board, _ = oif.paired_ablation_scoreboard(
        {
            "physics": state.get("contract_scores_full") or [],
            "physics_tape_confirm_near_close":
                state.get("contract_scores_physics_tape_confirm_near_close") or [],
            "physics_vol_regime_confirm":
                state.get("contract_scores_physics_vol_regime_confirm") or [],
        },
        state.get("contract_scores_market_mid_raw") or [],
        MIN_SCORED_CONTRACTS,
    )
    return oif.select_best_trusted(board, ABLATION_KEYS)


def _record_observation(state, obs, now_ms, predictions):
    ticker = obs["ticker"]
    close_ms = parse_close_ms(ticker)
    if close_ms is None:
        return
    entry = state["pending"].setdefault(
        ticker, {"close_ms": close_ms, "open_price": obs["open_price"], "obs": []}
    )
    if not entry.get("open_price"):
        entry["open_price"] = obs["open_price"]
    entry["close_ms"] = close_ms
    ablations = obs.get("p_ablations") or {
        "physics": obs["p_model"],
        "physics_tape_confirm_near_close": obs["p_model"],
        "physics_vol_regime_confirm": obs["p_model"],
    }
    if len(entry["obs"]) < MAX_OBS_PER_TICKER:
        entry["obs"].append({
            "p_model": obs["p_model"],
            "p_ablations": ablations,
            "yes_mid": obs["yes_mid"],
            "features": obs["features"],
            "ts_ms": now_ms,
        })
        state["observation_count"] = int(state.get("observation_count") or 0) + 1
    trusted = select_trusted_variant(state)
    live_p = obs["p_model"]
    if trusted and trusted in ablations:
        live_p = ablations[trusted]
    prediction = {
        "p_yes_full": live_p,
        "p_yes_market_baseline": obs["yes_mid"],
        "market_yes_mid": obs["yes_mid"],
        "edge": live_p - obs["yes_mid"],
        "features": obs["features"],
        "probability_source": "commodities-15m-directional-gaussian",
        "ablation_variant": trusted or "physics",
    }
    prediction.update(obs.get("book") or {})
    predictions[ticker] = prediction


def observe_cycle(state, evidence, now_ms, yahoo_cache=None, rest_markets=None,
                  pyth_fetcher=None, refresh_pyth=True):
    """Score open commodity 15m contracts from evidence and/or REST+Pyth."""
    if yahoo_cache is None:
        yahoo_cache = {}
    pyth_series = state.setdefault("pyth_series", {})
    if refresh_pyth:
        refresh_pyth_series(pyth_series, fetcher=pyth_fetcher)
    predictions = {}
    seen = set()

    snapshots = (evidence or {}).get("snapshots") or {}
    for ticker, snapshot in snapshots.items():
        obs = observation_from_evidence(
            ticker, snapshot, now_ms=now_ms, yahoo_cache=yahoo_cache)
        if obs is None:
            continue
        _record_observation(state, obs, now_ms, predictions)
        seen.add(obs["ticker"])

    if rest_markets is None:
        rest_markets = fetch_open_commodity_markets()
    for market in rest_markets:
        ticker = (market.get("ticker") or "").upper()
        if ticker in seen:
            continue
        obs = observation_from_rest(market, yahoo_cache, now_ms, pyth_series=pyth_series)
        if obs is None:
            continue
        _record_observation(state, obs, now_ms, predictions)
        seen.add(obs["ticker"])
    return predictions


def recorded_result(ticker, fetcher=None):
    return race.recorded_result(ticker, fetcher=fetcher)


def yahoo_nearest(series, ts_ms, max_gap_ms=90_000):
    if not series:
        return None
    lo, hi = 0, len(series) - 1
    while lo < hi:
        mid = (lo + hi) // 2
        if series[mid][0] < ts_ms:
            lo = mid + 1
        else:
            hi = mid
    candidates = [i for i in (lo - 1, lo) if 0 <= i < len(series)]
    best = min(candidates, key=lambda i: abs(series[i][0] - ts_ms))
    if abs(series[best][0] - ts_ms) > max_gap_ms:
        return None
    return series[best]


def derive_outcome_from_series(series, open_price, close_ms):
    """True/False from nearest series print at close vs open. None if hole."""
    if open_price is None or open_price <= 0.0 or close_ms is None:
        return None
    sample = yahoo_nearest([(int(t), float(p)) for t, p in (series or [])], close_ms)
    if sample is None:
        return None
    return sample[1] > open_price


def derive_outcome(ticker, open_price, yahoo_cache, close_ms=None, pyth_series=None):
    """Prefer Pyth series at close; Yahoo proxy fallback. None if hole."""
    if open_price is None or open_price <= 0.0:
        return None
    family = family_of(ticker)
    if family is None:
        return None
    close_ms = close_ms if close_ms is not None else parse_close_ms(ticker)
    if close_ms is None:
        return None
    pyth_symbol = FAMILIES[family]["pyth_symbol"]
    pyth = (pyth_series or {}).get(pyth_symbol) or []
    derived = derive_outcome_from_series(pyth, open_price, close_ms)
    if derived is not None:
        return derived, "derived_pyth"
    symbol = FAMILIES[family]["yahoo"]
    if symbol not in yahoo_cache:
        yahoo_cache[symbol] = fetch_yahoo_series(symbol)
    y = derive_outcome_from_series(yahoo_cache[symbol], open_price, close_ms)
    if y is None:
        return None, None
    return y, "derived_yahoo_proxy"


def resolve_outcome(ticker, open_price, yahoo_cache=None, fetcher=None, pyth_series=None):
    recorded = recorded_result(ticker, fetcher=fetcher)
    if recorded is not None:
        return recorded, "recorded"
    if yahoo_cache is None:
        yahoo_cache = {}
    derived, source = derive_outcome(
        ticker, open_price, yahoo_cache, pyth_series=pyth_series)
    if derived is None:
        return None, None
    return derived, source


def note_settlement_parity(state, ticker, open_price, recorded_outcome, pyth_series):
    """When Kalshi recorded a result, check whether Pyth would have matched."""
    if recorded_outcome is None:
        return
    family = family_of(ticker)
    if family is None:
        return
    close_ms = parse_close_ms(ticker)
    pyth = (pyth_series or {}).get(FAMILIES[family]["pyth_symbol"]) or []
    derived = derive_outcome_from_series(pyth, open_price, close_ms)
    if derived is None:
        return
    parity = state.setdefault("settlement_parity", {"checked": 0, "matched": 0})
    parity["checked"] = int(parity.get("checked") or 0) + 1
    if bool(derived) == bool(recorded_outcome):
        parity["matched"] = int(parity.get("matched") or 0) + 1


def settle_cycle(state, now_ms, resolver=None, yahoo_cache=None, pyth_series=None):
    if pyth_series is None:
        pyth_series = state.get("pyth_series") or {}
    if resolver is None:
        if yahoo_cache is None:
            yahoo_cache = {}

        def resolver(ticker, open_price):
            outcome, source = resolve_outcome(
                ticker, open_price, yahoo_cache, pyth_series=pyth_series)
            if source == "recorded":
                note_settlement_parity(
                    state, ticker, open_price, outcome, pyth_series)
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
            race.brier([(obs["p_model"], outcome) for obs in observations]))
        state["contract_scores_market_mid_raw"].append(
            race.brier([(obs["yes_mid"], outcome) for obs in observations]))
        tape_pairs = []
        vol_pairs = []
        for obs in observations:
            ablations = obs.get("p_ablations") or {}
            tape_pairs.append((
                ablations.get("physics_tape_confirm_near_close", obs["p_model"]),
                outcome))
            vol_pairs.append((
                ablations.get("physics_vol_regime_confirm", obs["p_model"]),
                outcome))
        state["contract_scores_physics_tape_confirm_near_close"].append(
            race.brier(tape_pairs))
        state["contract_scores_physics_vol_regime_confirm"].append(
            race.brier(vol_pairs))
        eligible_rows = [(obs["p_model"], obs["yes_mid"]) for obs in observations]
        eligible_model, eligible_mid = ce.eligible_pairs(eligible_rows, outcome)
        if eligible_model:
            state["contract_scores_eligible_full"].append(race.brier(eligible_model))
            state["contract_scores_eligible_market_mid_raw"].append(race.brier(eligible_mid))
        for key in ("physics_tape_confirm_near_close", "physics_vol_regime_confirm"):
            rows = [((obs.get("p_ablations") or {}).get(key, obs["p_model"]), obs["yes_mid"])
                    for obs in observations]
            v_model, v_mid = ce.eligible_pairs(rows, outcome)
            if v_model:
                # Pair this variant's Brier against the mid observed on ITS
                # OWN eligible contracts, not physics's -- eligibility is
                # per-predictor, so a variant's eligible population can
                # differ from physics's. Appended under the same `if
                # v_model:` guard as the model score so the two lists stay
                # index-aligned and equal-length.
                state[f"contract_scores_eligible_{key}"].append(race.brier(v_model))
                state[f"contract_scores_eligible_mid_{key}"].append(race.brier(v_mid))
        state["resolved"] = int(state.get("resolved") or 0) + 1
        del state["pending"][ticker]
    for key in (
        "contract_scores_full",
        "contract_scores_market_mid_raw",
        "contract_scores_physics_tape_confirm_near_close",
        "contract_scores_physics_vol_regime_confirm",
        "contract_scores_eligible_full",
        "contract_scores_eligible_market_mid_raw",
        "contract_scores_eligible_physics_tape_confirm_near_close",
        "contract_scores_eligible_physics_vol_regime_confirm",
        "contract_scores_eligible_mid_physics_tape_confirm_near_close",
        "contract_scores_eligible_mid_physics_vol_regime_confirm",
    ):
        state[key] = state[key][-SCORED_CONTRACT_WINDOW:]


def settlement_parity_summary(state):
    parity = state.get("settlement_parity") or {}
    checked = int(parity.get("checked") or 0)
    matched = int(parity.get("matched") or 0)
    rate = (matched / checked) if checked > 0 else None
    return {
        "checked": checked,
        "matched": matched,
        "match_rate": rate,
        "note": (
            "fraction of recorded Kalshi settlements where Pyth-derived "
            "direction matches; advisory only — does not grant trust"
        ),
    }


def ablation_scoreboard(state):
    return oif.paired_ablation_scoreboard(
        {
            "physics": state.get("contract_scores_full") or [],
            "physics_tape_confirm_near_close":
                state.get("contract_scores_physics_tape_confirm_near_close") or [],
            "physics_vol_regime_confirm":
                state.get("contract_scores_physics_vol_regime_confirm") or [],
        },
        state.get("contract_scores_market_mid_raw") or [],
        MIN_SCORED_CONTRACTS,
    )


def eligible_ablation_scoreboard(state):
    """Per-variant Brier vs mid over the BET-ELIGIBLE observations only.

    Each variant is paired against the mid observed on ITS OWN eligible
    contracts, not physics's. Eligibility is evaluated per predictor -- a
    contract can be eligible for one variant and not another -- so a shared
    mid baseline would score a variant's Brier against a different
    population's market prices than the one it was actually measured on.
    That is the exact defect this module exists to remove, so
    oif.paired_ablation_scoreboard (which takes one series map and one mid
    list) is called once per variant with that variant's own mid list, and
    the resulting rows are merged into a single board with the same shape
    the old single-call board produced, so oif.select_best_trusted still
    works unchanged.
    """
    board = {}
    physics_board, global_mid = oif.paired_ablation_scoreboard(
        {"physics": state.get("contract_scores_eligible_full") or []},
        state.get("contract_scores_eligible_market_mid_raw") or [],
        ce.MIN_ELIGIBLE_CONTRACTS,
    )
    board.update(physics_board)
    for key, mid_key in (
        ("physics_tape_confirm_near_close",
         "contract_scores_eligible_mid_physics_tape_confirm_near_close"),
        ("physics_vol_regime_confirm",
         "contract_scores_eligible_mid_physics_vol_regime_confirm"),
    ):
        variant_board, _variant_mid = oif.paired_ablation_scoreboard(
            {key: state.get(f"contract_scores_eligible_{key}") or []},
            state.get(mid_key) or [],
            ce.MIN_ELIGIBLE_CONTRACTS,
        )
        board.update(variant_board)
    return board, global_mid


def select_trusted_variant_eligible(state):
    """Best ablation that beats mid ON THE BET-ELIGIBLE SUBSET at >=100
    contracts; else None (fail-closed).

    Deliberately separate from select_trusted_variant: that one also selects
    live_p, the published probability. This one gates trust only.
    """
    board, _b_mid = eligible_ablation_scoreboard(state)
    return oif.select_best_trusted(board, ABLATION_KEYS)


def build_report(state, predictions, now_ms):
    scored = state.get("contract_scores_full") or []
    b_full = race.mean_or_none(scored)
    ablations, b_mid = ablation_scoreboard(state)
    trusted = select_trusted_variant(state)
    eligible_ablations, _b_eligible_mid = eligible_ablation_scoreboard(state)
    eligible = state.get("contract_scores_eligible_full") or []
    trusted_variant_eligible = select_trusted_variant_eligible(state)
    # The published eligible Briers must be the CLAIM'S OWN measurement.
    # `trusted_variant` is the variant that prices live_p, so it is the
    # variant every downstream reader is actually being asked to trust; the
    # numbers beside the flag are therefore ITS eligible Briers, taken from
    # its own row of the eligible board (paired against its own eligible mid
    # population). Publishing physics's numbers here instead would guard one
    # predictor's claim with another predictor's evidence.
    claim_row = ((eligible_ablations or {}).get(trusted) or {}) if trusted else {}
    b_eligible_full = claim_row.get("brier")
    b_eligible_mid = claim_row.get("brier_mid_paired")
    # Trust on the bet-eligible subset requires the SAME variant to win on
    # both boards. Two independent selections would let a bid be priced from
    # predictor A (select_trusted_variant, which sets live_p) while being
    # authorised by eligible evidence about predictor B. The
    # `eligible >= MIN_ELIGIBLE_CONTRACTS` conjunct is the design's own
    # literal definition of the flag and also keeps this strictly tighter
    # than the previous `trusted_variant_eligible is not None`: it cannot
    # publish doubles for a variant while physics's eligible population --
    # the one `eligible_scored_contracts` reports -- is empty or short.
    adds_value_on_bet_eligible = (
        trusted is not None
        and trusted_variant_eligible == trusted
        and b_eligible_full is not None
        and b_eligible_mid is not None
        and len(eligible) >= ce.MIN_ELIGIBLE_CONTRACTS
    )
    return {
        "schema": STATE_SCHEMA,
        "event": "commodities_15m_calibrator",
        "advisory_only": True,
        "family": "COMMODITIES15M",
        "families": sorted(FAMILIES.keys()),
        "generated_at_ms": now_ms,
        "resolved_contracts": int(state.get("resolved") or 0),
        "scored_contracts": len(scored),
        "training_observations": int(state.get("observation_count") or 0),
        "scoring_rule": (
            "one score per commodity-15m contract: mean squared error over that "
            "contract's observations; physics is P(close>open) Gaussian "
            "(Pyth Metal preferred, Yahoo cold fallback); "
            "physics_tape_confirm_near_close uses Yahoo futures tape confirm "
            "in the final 3m; physics_vol_regime_confirm clamps quiet/weekend "
            "to mid; market is raw yes mid"
        ),
        "spot_feeds": {
            family: {
                "pyth_symbol": meta["pyth_symbol"],
                "pyth_id": meta["pyth_id"],
                "yahoo_fallback": meta["yahoo"],
            }
            for family, meta in FAMILIES.items()
        },
        "settlement_parity": settlement_parity_summary(state),
        "min_scored_contracts": MIN_SCORED_CONTRACTS,
        "brier_full": b_full,
        "brier_market_mid_raw": b_mid,
        "ablations": ablations,
        "trusted_variant": trusted,
        "adds_value_over_market": trusted is not None,
        "eligible_ablations": eligible_ablations,
        # Count of contracts with at least one eligible observation for the
        # physics population -- the deployment-observability number, kept as
        # it was so "how long until the floor is reachable" stays readable.
        "eligible_scored_contracts": len(eligible),
        "brier_eligible_full": b_eligible_full,
        "brier_eligible_market_mid_raw": b_eligible_mid,
        "min_eligible_contracts": ce.MIN_ELIGIBLE_CONTRACTS,
        "trusted_variant_eligible": trusted_variant_eligible,
        "adds_value_on_bet_eligible": adds_value_on_bet_eligible,
        "predictions": predictions,
    }


def run_once(now_ms=None):
    now_ms = now_ms if now_ms is not None else int(time.time() * 1000)
    evidence = {}
    try:
        with open(EVIDENCE_PATH, "r", encoding="utf-8") as fh:
            evidence = json.load(fh)
    except (OSError, ValueError):
        evidence = {}
    state = load_state()
    yahoo_cache = {}
    predictions = observe_cycle(state, evidence, now_ms, yahoo_cache=yahoo_cache)
    settle_cycle(state, now_ms, yahoo_cache=yahoo_cache,
                 pyth_series=state.get("pyth_series") or {})
    race.save_json_atomic(state, STATE_PATH)
    report = build_report(state, predictions, now_ms)
    race.save_json_atomic(report, OUTPUT_PATH)
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
                "predictions": len(result.get("predictions") or {}),
                "adds_value_over_market": result.get("adds_value_over_market"),
                "error": result.get("error"),
            }), flush=True)
            time.sleep(interval)
    print("usage: commodities_15m_calibrator.py [once|run [--interval N]|report]",
          file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
