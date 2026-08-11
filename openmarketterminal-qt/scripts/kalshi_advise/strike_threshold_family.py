#!/usr/bin/env python3
"""Shared OnlineLogit calibrator for Kalshi *strike / above* families.

Used by commodities hourly (KXGOLDH/…), commodities daily (KXGOLDD/…/KXWTI),
and BTC daily band floors (KXBTC). These are NOT 15m open→close races —
settled samples are "Will X close above $Y?" with floor_strike and no cap.

Trust, eligible Brier, and state files stay per-family via Profile.
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
from dataclasses import dataclass, replace
from typing import Any, Callable, Dict, List, Optional, Sequence, Tuple

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from openterminal_paths import evidence_file

import calibrator_eligibility as ce
import spot_calibrator as sc

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
BRTI_PATH = evidence_file("kalshi-cf-benchmarks.jsonl")
BRTI_PATH_ROTATION = evidence_file("kalshi-cf-benchmarks.jsonl.1")
SETTLEMENTS_PATH = evidence_file("kalshi-settlements.jsonl")
EVIDENCE_PATH = evidence_file("kalshi-ws-books.json")

UA = "OpenTerminal/strike-threshold-family"
STATE_SCHEMA = 1
MAX_OBS_PER_TICKER = sc.MAX_OBS_PER_TICKER
SCORED_CONTRACT_WINDOW = sc.SCORED_CONTRACT_WINDOW
MIN_SCORED_CONTRACTS = sc.MIN_SCORED_CONTRACTS
DEFAULT_VOL_BPS = 3.0


@dataclass(frozen=True)
class SeriesSpec:
    yahoo: str = ""
    label: str = ""
    pyth_symbol: str = ""
    pyth_id: str = ""
    spot_mode: str = "pyth"  # pyth | brti | yahoo


@dataclass(frozen=True)
class Profile:
    event: str
    family: str
    series: Dict[str, SeriesSpec]
    state_path: str
    output_path: str
    probability_source: str


def series_of(ticker: str) -> str:
    head = (ticker or "").split("-", 1)[0].upper()
    return head


def is_profile_ticker(ticker: str, profile: Profile) -> bool:
    return series_of(ticker) in profile.series


def _http_json(url: str, timeout: float = 12.0) -> dict:
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _dollars(market: dict, *keys: str) -> Optional[float]:
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


def yes_mid_from_market(market: dict) -> Optional[float]:
    bid = _dollars(market, "yes_bid_dollars", "yes_bid")
    ask = _dollars(market, "yes_ask_dollars", "yes_ask")
    if bid is not None and ask is not None and 0.0 < bid < 1.0 and 0.0 < ask < 1.0:
        return (bid + ask) / 2.0
    last = _dollars(market, "last_price_dollars", "last_price")
    if last is not None and 0.0 < last < 1.0:
        return last
    return None


def seconds_left_from_market(market: dict, now_ms: int) -> Optional[int]:
    close = market.get("close_time") or market.get("expected_expiration_time")
    if not close:
        return None
    try:
        dt = datetime.datetime.fromisoformat(str(close).replace("Z", "+00:00"))
        return int((dt.timestamp() * 1000 - now_ms) / 1000)
    except (TypeError, ValueError):
        return None


def close_ms_from_market(market: dict) -> Optional[int]:
    close = market.get("close_time") or market.get("expected_expiration_time")
    if not close:
        return None
    try:
        dt = datetime.datetime.fromisoformat(str(close).replace("Z", "+00:00"))
        return int(dt.timestamp() * 1000)
    except (TypeError, ValueError):
        return None


def floor_cap(market: dict) -> Tuple[Optional[float], Optional[float]]:
    floor = None
    cap = None
    try:
        f = float(market.get("floor_strike") or 0.0)
        if f > 0.0:
            floor = f
    except (TypeError, ValueError):
        pass
    raw_cap = market.get("cap_strike")
    if raw_cap is not None and raw_cap != "":
        try:
            c = float(raw_cap)
            if c > 0.0:
                cap = c
        except (TypeError, ValueError):
            pass
    return floor, cap


def fetch_yahoo_series(symbol: str) -> List[Tuple[int, float]]:
    if not symbol:
        return []
    try:
        payload = _http_json(YAHOO_CHART_URL.format(symbol=urllib.parse.quote(symbol)))
        result = ((payload.get("chart") or {}).get("result") or [None])[0] or {}
        timestamps = result.get("timestamp") or []
        closes = (((result.get("indicators") or {}).get("quote") or [{}])[0].get("close") or [])
        out = []
        for ts, px in zip(timestamps, closes):
            if px is None:
                continue
            out.append((int(ts) * 1000, float(px)))
        return out
    except (OSError, ValueError, IndexError, TypeError, KeyError):
        return []


def per_min_vol_bps_from_series(points: Sequence[Tuple[int, float]]) -> Optional[float]:
    if len(points) < 8:
        return None
    rets = []
    for i in range(1, len(points)):
        a, b = points[i - 1][1], points[i][1]
        if a <= 0.0 or b <= 0.0:
            continue
        rets.append(math.log(b / a))
    if len(rets) < 5:
        return None
    mean = sum(rets) / len(rets)
    var = sum((r - mean) ** 2 for r in rets) / max(1, len(rets) - 1)
    # 1m bars → per-min vol in bps
    return math.sqrt(var) * 10000.0


def fetch_pyth_spot(pyth_id: str, fetcher=None) -> Optional[float]:
    if not pyth_id:
        return None
    fetch = fetcher or _http_json
    try:
        url = PYTH_HERMES_LATEST + "?" + urllib.parse.urlencode({"ids[]": pyth_id})
        payload = fetch(url)
        parsed = (payload.get("parsed") or [None])[0] or {}
        price_obj = parsed.get("price") or {}
        price = float(price_obj.get("price"))
        expo = int(price_obj.get("expo") or 0)
        return price * (10 ** expo)
    except (OSError, ValueError, TypeError, IndexError, KeyError):
        return None


def latest_brti_spot() -> Optional[float]:
    for path in (BRTI_PATH, BRTI_PATH_ROTATION):
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                lines = fh.readlines()[-80:]
        except OSError:
            continue
        for line in reversed(lines):
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except ValueError:
                continue
            if row.get("id") not in (None, "BRTI") and row.get("symbol") not in (None, "BRTI"):
                # allow plain price rows
                pass
            price = row.get("price") or row.get("value") or (row.get("payload") or {}).get("price")
            try:
                px = float(price)
            except (TypeError, ValueError):
                continue
            if px > 0.0:
                return px
    return None


def resolve_spot(spec: SeriesSpec, yahoo_cache: dict) -> Tuple[Optional[float], Optional[float], str]:
    """Return (spot, per_min_vol_bps, source)."""
    if spec.spot_mode == "brti":
        spot = latest_brti_spot()
        vol = None
        if spec.yahoo:
            if spec.yahoo not in yahoo_cache:
                yahoo_cache[spec.yahoo] = fetch_yahoo_series(spec.yahoo)
            series = yahoo_cache[spec.yahoo]
            vol = per_min_vol_bps_from_series(series)
            if spot is None and series:
                spot = series[-1][1]
                return spot, vol, "yahoo:%s" % spec.yahoo
        if spot is not None:
            return spot, vol if vol is not None else DEFAULT_VOL_BPS, "brti"
        return None, None, "missing"

    spot = None
    source = "missing"
    if spec.pyth_id:
        spot = fetch_pyth_spot(spec.pyth_id)
        if spot is not None:
            source = "pyth:%s" % spec.pyth_symbol
    vol = None
    if spec.yahoo:
        if spec.yahoo not in yahoo_cache:
            yahoo_cache[spec.yahoo] = fetch_yahoo_series(spec.yahoo)
        series = yahoo_cache[spec.yahoo]
        vol = per_min_vol_bps_from_series(series)
        if spot is None and series:
            spot = series[-1][1]
            source = "yahoo:%s" % spec.yahoo
    if spot is None:
        return None, None, source
    if vol is None:
        vol = DEFAULT_VOL_BPS
    return spot, vol, source


def fetch_open_markets(profile: Profile) -> List[dict]:
    markets = []
    for series in profile.series:
        try:
            payload = _http_json(KALSHI_MARKETS_URL.format(series=series))
        except (OSError, ValueError):
            continue
        for market in payload.get("markets") or []:
            ticker = (market.get("ticker") or "").upper()
            if is_profile_ticker(ticker, profile):
                markets.append(market)
    return markets


def features_from_market(
    market: dict, profile: Profile, now_ms: int, yahoo_cache: dict
) -> Optional[dict]:
    ticker = (market.get("ticker") or "").upper()
    if not is_profile_ticker(ticker, profile):
        return None
    floor, cap = floor_cap(market)
    # Floor-only threshold: a positive cap is a range band we do not model.
    if floor is None or floor <= 0.0 or (cap is not None and cap > 0.0):
        return None
    seconds_left = seconds_left_from_market(market, now_ms)
    if seconds_left is None or seconds_left <= 0:
        return None
    yes_mid = yes_mid_from_market(market)
    if yes_mid is None or not 0.0 < yes_mid < 1.0:
        return None
    spec = profile.series[series_of(ticker)]
    spot, vol, source = resolve_spot(spec, yahoo_cache)
    if spot is None or spot <= 0.0:
        return None
    minutes_left = seconds_left / 60.0
    required_move_bps = abs(spot - floor) / spot * 10000.0
    required_move_sigma = 0.0
    if vol and vol > 0.0 and minutes_left > 0.0:
        required_move_sigma = required_move_bps / (vol * math.sqrt(minutes_left))
    bid = _dollars(market, "yes_bid_dollars", "yes_bid")
    ask = _dollars(market, "yes_ask_dollars", "yes_ask")
    bid_sz = float(market.get("yes_bid_size") or 0.0)
    ask_sz = float(market.get("yes_ask_size") or 0.0)
    denom = bid_sz + ask_sz
    features = {
        "signed_distance_bps": (spot - floor) / spot * 10000.0,
        "per_min_vol_bps": float(vol or DEFAULT_VOL_BPS),
        "sqrt_minutes_left": math.sqrt(minutes_left),
        "required_move_sigma": required_move_sigma,
        "realized_move_bps": 0.0,
        "yes_mid": yes_mid,
        "book_imbalance": (bid_sz - ask_sz) / denom if denom > 0.0 else 0.0,
        "trade_flow": 0.0,
        "spot_drift": 0.0,
        "news_forecast": 0.0,
        "event_pressure": 0.0,
        "spot": spot,
        "floor_strike": floor,
        "spot_source": source,
        "underlier": spec.label,
    }
    book = {}
    if bid is not None and 0.0 < bid < 1.0:
        book["market_yes_bid"] = bid
    if ask is not None and 0.0 < ask < 1.0:
        book["market_yes_ask"] = ask
    return {
        "ticker": ticker,
        "features": features,
        "yes_mid": yes_mid,
        "close_ms": close_ms_from_market(market),
        "book": book,
    }


def default_state() -> dict:
    return {
        "schema": STATE_SCHEMA,
        "full": sc.OnlineLogit(sc.FULL_FEATURES).to_json(),
        "market": sc.OnlineLogit(("yes_mid",)).to_json(),
        "pending": {},
        "contract_scores_full": [],
        "contract_scores_market_trained_logit": [],
        "contract_scores_market_mid_raw": [],
        "contract_scores_eligible_full": [],
        "contract_scores_eligible_market_mid_raw": [],
        "resolved_record": [],
        "resolved": 0,
        "skipped_unmodeled": 0,
    }


def load_state(path: str) -> dict:
    try:
        with open(path, "r", encoding="utf-8") as fh:
            state = json.load(fh)
    except (OSError, ValueError):
        return default_state()
    if not isinstance(state, dict) or state.get("schema") != STATE_SCHEMA:
        return default_state()
    for key, value in default_state().items():
        state.setdefault(key, value)
    return state


def observe_cycle(
    state: dict,
    profile: Profile,
    now_ms: int,
    rest_markets: Optional[List[dict]] = None,
    yahoo_cache: Optional[dict] = None,
) -> Dict[str, Any]:
    yahoo_cache = {} if yahoo_cache is None else yahoo_cache
    full = sc.reconcile_full_model(state["full"])
    markets = rest_markets if rest_markets is not None else fetch_open_markets(profile)
    predictions: Dict[str, Any] = {}
    for market in markets:
        obs = features_from_market(market, profile, now_ms, yahoo_cache)
        if obs is None:
            state["skipped_unmodeled"] = int(state.get("skipped_unmodeled") or 0) + 1
            continue
        ticker = obs["ticker"]
        close_ms = obs.get("close_ms")
        if close_ms is None:
            continue
        features = obs["features"]
        p_full = full.predict(features)
        entry = state["pending"].setdefault(ticker, {"close_ms": close_ms, "obs": []})
        entry["close_ms"] = close_ms
        if len(entry["obs"]) < MAX_OBS_PER_TICKER:
            entry["obs"].append(dict(features))
        pred = {
            "p_yes_full": p_full,
            "p_yes_market_baseline": obs["yes_mid"],
            "market_yes_mid": obs["yes_mid"],
            "edge": p_full - obs["yes_mid"],
            "features": features,
            "probability_source": profile.probability_source,
        }
        pred.update(obs.get("book") or {})
        predictions[ticker] = pred
    state["full"] = full.to_json()
    return predictions


def resolve_outcome_kalshi(ticker: str) -> Optional[bool]:
    try:
        payload = _http_json(KALSHI_MARKET_URL.format(ticker=urllib.parse.quote(ticker)))
        market = payload.get("market") or payload
        result = (market.get("result") or "").upper()
        if result == "YES":
            return True
        if result == "NO":
            return False
    except (OSError, ValueError):
        pass
    # Settlements jsonl fallback
    try:
        with open(SETTLEMENTS_PATH, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                try:
                    row = json.loads(line)
                except ValueError:
                    continue
                if row.get("ticker") == ticker or row.get("kalshi_market_id") == ticker:
                    r = (row.get("result") or row.get("market_result") or "").upper()
                    if r == "YES":
                        return True
                    if r == "NO":
                        return False
    except OSError:
        pass
    return None


def settle_cycle(state: dict, now_ms: int, resolver=resolve_outcome_kalshi) -> None:
    full = sc.reconcile_full_model(state["full"])
    market = sc.OnlineLogit.from_json(state["market"])
    for ticker in list(state["pending"].keys()):
        entry = state["pending"][ticker]
        close_ms = int(entry.get("close_ms") or 0)
        if now_ms < close_ms + 120_000:
            continue
        outcome = resolver(ticker)
        if outcome is None:
            if now_ms > close_ms + 24 * 3600 * 1000:
                del state["pending"][ticker]
            continue
        observations = entry.get("obs") or []
        if not observations:
            del state["pending"][ticker]
            continue
        state["contract_scores_full"].append(
            sc.brier([(full.predict(f), outcome) for f in observations])
        )
        state["contract_scores_market_trained_logit"].append(
            sc.brier([(market.predict(f), outcome) for f in observations])
        )
        state["contract_scores_market_mid_raw"].append(
            sc.brier([(f["yes_mid"], outcome) for f in observations])
        )
        eligible_model, eligible_mid = ce.eligible_pairs(
            [(full.predict(f), f["yes_mid"]) for f in observations], outcome
        )
        if eligible_model:
            state["contract_scores_eligible_full"].append(sc.brier(eligible_model))
            state["contract_scores_eligible_market_mid_raw"].append(sc.brier(eligible_mid))
        state["resolved_record"].append({"observations": observations, "outcome": outcome})
        for features in observations:
            full.update(features, outcome, l2=sc.L2)
            market.update(features, outcome)
        state["resolved"] = int(state.get("resolved") or 0) + 1
        del state["pending"][ticker]
    for key in (
        "contract_scores_full",
        "contract_scores_market_trained_logit",
        "contract_scores_market_mid_raw",
        "resolved_record",
        "contract_scores_eligible_full",
        "contract_scores_eligible_market_mid_raw",
    ):
        state[key] = (state.get(key) or [])[-SCORED_CONTRACT_WINDOW:]
    state["full"] = full.to_json()
    state["market"] = market.to_json()


def build_report(state: dict, predictions: dict, now_ms: int, profile: Profile) -> dict:
    scored = state.get("contract_scores_full") or []
    b_full = sc.mean_or_none(scored)
    b_logit = sc.mean_or_none(state.get("contract_scores_market_trained_logit") or [])
    b_mid_raw = sc.mean_or_none(state.get("contract_scores_market_mid_raw") or [])
    eligible = state.get("contract_scores_eligible_full") or []
    eligible_mid = state.get("contract_scores_eligible_market_mid_raw") or []
    enough = len(scored) >= MIN_SCORED_CONTRACTS

    # POOLED EVIDENCE MUST NOT AUTHORIZE BIDS.
    #
    # A profile covering more than one series pools genuinely different
    # prediction problems into one model, one Brier and one trust flag.
    # Measured 2026-08-11: commodities-hourly pooled KXGOLDH + KXSILVERH +
    # KXWTIH over 500 contracts and published adds_value_on_bet_eligible=true,
    # which authorised bids in all three underlyings (KXGOLDH 58, KXSILVERH 41,
    # KXWTIH 1 on 08-10) from evidence that no single one of them had earned.
    #
    # Until this producer fits and scores per family, the pooled numbers are
    # DIAGNOSTICS: they are still published, under `pooled_*` names so nothing
    # mistakes them for a verdict, and the authorising flags read false. Note
    # false here means "not authorised", not "measured to have no edge" —
    # `pooled_trust_withheld` says which, so a later split is not read as a
    # regression.
    pooled = len(profile.series) > 1
    pooled_over_market = (
        b_full is not None and b_mid_raw is not None and enough and b_full < b_mid_raw
    )
    pooled_on_eligible = ce.adds_value(eligible, eligible_mid)
    report = {
        "schema": 2,
        "event": profile.event,
        "family": profile.family,
        "families": sorted(profile.series.keys()),
        "advisory_only": True,
        "generated_at_ms": now_ms,
        "resolved_contracts": int(state.get("resolved") or 0),
        "scored_contracts": len(scored),
        "training_observations": int((state.get("full") or {}).get("n_seen") or 0),
        "skipped_unmodeled": int(state.get("skipped_unmodeled") or 0),
        "min_scored_contracts": MIN_SCORED_CONTRACTS,
        "brier_full": b_full,
        "brier_market_mid_raw": b_mid_raw,
        "brier_market_trained_logit": b_logit,
        "adds_value_over_market": False if pooled else pooled_over_market,
        "eligible_scored_contracts": len(eligible),
        "brier_eligible_full": sc.mean_or_none(eligible),
        "brier_eligible_market_mid_raw": sc.mean_or_none(eligible_mid),
        "min_eligible_contracts": ce.MIN_ELIGIBLE_CONTRACTS,
        "adds_value_on_bet_eligible": False if pooled else pooled_on_eligible,
        "predictions": predictions,
        "probability_source": profile.probability_source,
    }
    if pooled:
        report["pooled_trust_withheld"] = True
        report["pooled_families"] = sorted(profile.series.keys())
        report["pooled_adds_value_over_market"] = pooled_over_market
        report["pooled_adds_value_on_bet_eligible"] = pooled_on_eligible
        report["pooled_note"] = (
            "evidence is pooled across %d series, so it cannot authorise a bid in any one of "
            "them; the pooled_* values are diagnostics only. Split this producer per family "
            "before reading them as trust." % len(profile.series)
        )
    return report


def family_profile(profile: Profile, series_ticker: str) -> Profile:
    """`profile` narrowed to a single series, so every downstream helper —
    fetching, feature building, fitting, scoring, reporting — operates on ONE
    prediction problem without knowing it was ever part of a group."""
    return replace(profile, series={series_ticker: profile.series[series_ticker]})


def split_state(outer: dict, profile: Profile) -> dict:
    """Per-family state slices, migrating a legacy pooled state on first run.

    A pooled state carries ONE OnlineLogit fitted across every series in the
    profile. It cannot seed any individual family: a gold-and-silver-and-oil
    model is not gold's model, and copying it into all three would recreate the
    exact contamination this split exists to remove. So the legacy blob is set
    aside (the caller archives it) and each family starts from an empty state.

    The cost is real and worth stating: pooled sample counts do not carry over.
    commodities-hourly had 500 pooled scored contracts; after the split every
    family starts at zero. That is the honest price of measuring three separate
    things separately.
    """
    by_family = outer.get("by_family")
    if not isinstance(by_family, dict):
        by_family = {}
        # A SINGLE-series profile was never pooled: its flat state already IS
        # that family's own evidence, so it is ADOPTED rather than discarded.
        # Resetting it would destroy legitimate history to fix a problem that
        # profile never had. Measured the hard way: kxbtc-daily (KXBTC only)
        # lost resolved=4 / n_seen=167 to exactly this mistake.
        if len(profile.series) == 1 and "full" in outer:
            only = next(iter(profile.series))
            by_family[only] = {k: v for k, v in outer.items() if k != "by_family"}
    for series_ticker in profile.series:
        if not isinstance(by_family.get(series_ticker), dict):
            by_family[series_ticker] = default_state()
    return by_family


def run_once(profile: Profile, now_ms: Optional[int] = None, rest_markets=None) -> dict:
    now_ms = now_ms if now_ms is not None else int(time.time() * 1000)
    outer = load_state(profile.state_path)

    # One-time migration: archive a legacy pooled state rather than deleting it,
    # so the evidence that produced earlier reports still exists to inspect.
    if "by_family" not in outer and outer.get("resolved"):
        legacy = "%s.pooled-legacy.json" % profile.state_path
        if not os.path.exists(legacy):
            sc.save_json_atomic(outer, legacy)

    by_family_state = split_state(outer, profile)
    markets = rest_markets if rest_markets is not None else fetch_open_markets(profile)
    yahoo_cache: dict = {}

    by_family_report: dict = {}
    predictions: dict = {}
    for series_ticker in sorted(profile.series):
        sub = family_profile(profile, series_ticker)
        state = by_family_state[series_ticker]
        # Only this family's markets reach this family's model.
        mine = [m for m in markets if series_of(str(m.get("ticker") or "")) == series_ticker]
        preds = observe_cycle(state, sub, now_ms, rest_markets=mine, yahoo_cache=yahoo_cache)
        settle_cycle(state, now_ms)
        by_family_state[series_ticker] = state
        # build_report on a SINGLE-series profile, so each family's trust is
        # computed from its own evidence and is not withheld as pooled.
        by_family_report[series_ticker] = build_report(state, preds, now_ms, sub)
        predictions.update(preds)

    outer = {"schema": STATE_SCHEMA, "by_family": by_family_state}
    sc.save_json_atomic(outer, profile.state_path)

    # The umbrella report keeps its pooled shape for existing readers, with the
    # authorising flags withheld (see build_report), and carries the per-family
    # verdicts that ARE authoritative under `by_family`. One atomic write.
    report = build_report(pooled_view(by_family_state), predictions, now_ms, profile)
    report["by_family"] = by_family_report
    sc.save_json_atomic(report, profile.output_path)
    return report


def pooled_view(by_family_state: dict) -> dict:
    """Every family's scores concatenated — DIAGNOSTIC ONLY.

    This is the apples-and-oranges number: it counts how many contracts were
    scored across the group, which is meaningful, and averages their Brier,
    which is not. build_report withholds the authorising flags for any
    multi-series profile, so this can be reported without being believed.
    """
    pooled = default_state()
    for state in by_family_state.values():
        for key in (
            "contract_scores_full",
            "contract_scores_market_trained_logit",
            "contract_scores_market_mid_raw",
            "contract_scores_eligible_full",
            "contract_scores_eligible_market_mid_raw",
        ):
            pooled[key] = list(pooled[key]) + list(state.get(key) or [])
        pooled["resolved"] += int(state.get("resolved") or 0)
        pooled["skipped_unmodeled"] += int(state.get("skipped_unmodeled") or 0)
    return pooled


def main_for(profile: Profile, argv: Sequence[str]) -> int:
    command = argv[1] if len(argv) > 1 else "once"
    if command == "once":
        print(json.dumps(run_once(profile)))
        return 0
    if command == "report":
        state = load_state(profile.state_path)
        print(json.dumps(build_report(state, {}, int(time.time() * 1000), profile)))
        return 0
    if command == "run":
        interval = 60
        if "--interval" in argv:
            interval = max(10, int(argv[argv.index("--interval") + 1]))
        while True:
            result = run_once(profile)
            print(
                json.dumps(
                    {
                        "cycle": result.get("generated_at_ms"),
                        "resolved": result.get("resolved_contracts"),
                        "scored": result.get("scored_contracts"),
                        "predictions": len(result.get("predictions") or {}),
                        "adds_value_over_market": result.get("adds_value_over_market"),
                        "family": profile.family,
                    }
                ),
                flush=True,
            )
            time.sleep(interval)
    print(
        "usage: %s [once|run [--interval N]|report]" % os.path.basename(argv[0]),
        file=sys.stderr,
    )
    return 2


# --- concrete profiles -------------------------------------------------------

_METAL_GOLD = SeriesSpec(
    yahoo="GC=F",
    label="gold",
    pyth_symbol="Metal.XAU/USD",
    pyth_id="765d2ba906dbc32ca17cc11f5310a89e9ee1f6420508c63861f2f8ba4ee34bb2",
)
_METAL_SILVER = SeriesSpec(
    yahoo="SI=F",
    label="silver",
    pyth_symbol="Metal.XAG/USD",
    pyth_id="f2fb02c32b055c805e7238d628e5e9dadef274376114eb1f012337cabe93871e",
)
_METAL_WTI = SeriesSpec(
    yahoo="CL=F",
    label="wti",
    pyth_symbol="Metal.XTI/USD",
    pyth_id="a35b407f0fa4b027c2dfa8dff0b7b99b853fb4d326a9e9906271933237b90c1c",
)

COMMODITIES_HOURLY = Profile(
    event="commodities_hourly_calibrator",
    family="COMMODITIES_HOURLY",
    series={
        "KXGOLDH": _METAL_GOLD,
        "KXSILVERH": _METAL_SILVER,
        "KXWTIH": _METAL_WTI,
    },
    state_path=evidence_file("commodities-hourly-calibrator-state.json"),
    output_path=evidence_file("commodities-hourly-calibrator.json"),
    probability_source="commodities-hourly-strike-logit",
)

COMMODITIES_DAILY = Profile(
    event="commodities_daily_calibrator",
    family="COMMODITIES_DAILY",
    series={
        "KXGOLDD": _METAL_GOLD,
        "KXSILVERD": _METAL_SILVER,
        # WTI daily up/above series ticker is KXWTI (not KXWTID).
        "KXWTI": _METAL_WTI,
    },
    state_path=evidence_file("commodities-daily-calibrator-state.json"),
    output_path=evidence_file("commodities-daily-calibrator.json"),
    probability_source="commodities-daily-strike-logit",
)

KXBTC_DAILY = Profile(
    event="kxbtc_daily_calibrator",
    family="KXBTC_DAILY",
    series={
        "KXBTC": SeriesSpec(
            yahoo="BTC-USD", label="btc", spot_mode="brti"
        ),
    },
    state_path=evidence_file("kxbtc-daily-calibrator-state.json"),
    output_path=evidence_file("kxbtc-daily-calibrator.json"),
    probability_source="kxbtc-daily-strike-logit",
)
