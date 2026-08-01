#!/usr/bin/env python3
"""Online probability calibrator for Kalshi hourly crypto contracts.

Advisory-only, engine-side evidence: learns P(settles YES) from the daemon's
decision snapshots (kalshi-ws-books.json) and updates itself on every public
settlement. It never touches the frozen duel scoring files and never places
orders; its output is one more evidence file beside the order books.

Two models are trained in parallel and BOTH are always reported:
  - "full":   physics features (signed distance, ambient vol, time, realized
              move) plus the market mid.
  - "market": the market mid alone — a TRAINED one-feature logit, which is a
              handicapped baseline, not the market. It is reported as
              `brier_market_trained_logit` so its name says what it is.
The market itself is scored separately and untrained, as `brier_market_mid_raw`
(the mid used verbatim as a probability). "Adds value over market" means
beating THAT, because that is the price the bot would otherwise take.

HOW THE BRIER IS SCORED (issue #171). Schema 1 appended one
`[probability, outcome]` pair per OBSERVATION, and a contract contributes up to
`MAX_OBS_PER_TICKER = 60` of them, all sharing one outcome. A trailing window of
500 such pairs is ~8–10 contracts of heavily correlated rows, so the old
`training_samples: 500` overstated the evidence by roughly fiftyfold and the
≥100 gate cleared after about two contracts settled
(docs/research/2026-07-27-kalshi-edge-autopsy.md, Q2).

Schema 2 scores ONE NUMBER PER RESOLVED CONTRACT. The rule, applied identically
to all three scorers:

    a contract's score = the MEAN squared error over that contract's own
    observations, using predictions taken BEFORE the models train on it.

Equal weight per contract (not per observation), so a 60-observation contract
and a 2-observation contract count the same; and out-of-sample with respect to
the contract being scored, because every prediction is read off the models as
they stood before that contract's outcome reached them. The autopsy's
reconstruction scored the same data row-weighted (0.1043 calibrated vs 0.0989
raw mid over 239 contracts); contract-weighting is a different estimator of the
same quantity, so the numbers here will not reproduce those digit for digit.

Training is unchanged: both models still learn from every observation. Only the
BOOKKEEPING is per contract.

Commands:  once | run [--interval 60] | report
"""
import collections
import datetime
import json
import math
import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from openterminal_paths import evidence_file

EVIDENCE_PATH = evidence_file("kalshi-ws-books.json")
STATE_PATH = evidence_file("spot-calibrator-state.json")
OUTPUT_PATH = evidence_file("calibrator.json")
TRADES_PATH = evidence_file("kalshi-trades.jsonl")
SPOT_SERIES_PATH = evidence_file("btc-intelligence.jsonl")
INTEL_LATEST_PATH = evidence_file("btc-intelligence-latest.json")
EVENT_IMPACT_LATEST_PATH = evidence_file("btc-event-impact-latest.json")
KALSHI_MARKET_URL = "https://api.elections.kalshi.com/trade-api/v2/markets/{ticker}"

# trade_flow / spot_drift / news_forecast tuning (issue tracked in docs/research).
# Real kalshi-trades.jsonl runs at several thousand rows / 5 minutes on an
# active hour, so TRADES_TAIL_LINES is sized generously above that so the
# window is never truncated by the tail read itself.
TRADE_FLOW_WINDOW_MS = 5 * 60 * 1000
TRADES_TAIL_LINES = 20000
# btc-intelligence.jsonl's real cadence is ~hourly (confirmed against the live
# file), not the "few minutes" the brief's docstring language suggests -- see
# the module note on spot_drift_feature. The lookback stays at 15 minutes;
# spot_prev_asof degrades gracefully to whatever the sparser real series has.
SPOT_DRIFT_LOOKBACK_MS = 15 * 60 * 1000
SPOT_SERIES_TAIL_LINES = 2000

PHYSICS_FEATURES = ("signed_distance_bps", "per_min_vol_bps", "sqrt_minutes_left",
                    "required_move_sigma", "realized_move_bps", "yes_mid")
# The hourly signal ensemble (issue tracked in docs/research): book_imbalance
# is computed in extract_features from the daemon's own book; trade_flow,
# spot_drift, news_forecast, and event_pressure are computed in observe_cycle
# from the auxiliary sources (trade tape, spot series, btc-intelligence,
# btc-event-impact) and override extract_features' neutral 0.0 stubs there --
# extract_features alone (no aux supplied) still returns those four as 0.0, so
# callers that only have a snapshot keep getting an honest neutral read. All
# five round-trip through the saved "full" model, hence PHYSICS_FEATURES
# staying named for `reconcile_full_model`.
ENSEMBLE_FEATURES = ("book_imbalance", "trade_flow", "spot_drift", "news_forecast", "event_pressure")
FULL_FEATURES = PHYSICS_FEATURES + ENSEMBLE_FEATURES
MARKET_FEATURES = ("yes_mid",)
MAX_OBS_PER_TICKER = 60
MIN_STANDARDIZE_SAMPLES = 8
STATE_SCHEMA = 2
# Both in CONTRACTS, never observations. See the module header for why the
# distinction is the whole point of schema 2.
SCORED_CONTRACT_WINDOW = 500
MIN_SCORED_CONTRACTS = 100
# L2 on the "full" model's feature weights (never the bias) — keeps the five
# new ensemble weights from running away before enough contracts settle.
L2 = 1e-3


def extract_features(snapshot):
    """Flatten one decision snapshot into the calibrator feature dict.

    Returns None for snapshots this v1 does not model (range markets,
    missing strikes, expired/blank contracts) — skipping is honest, guessing
    is not.
    """
    contract = snapshot.get("contract") or {}
    horizon = contract.get("horizon") or {}
    spot = float(horizon.get("spot") or 0.0)
    floor = float(horizon.get("floor_strike") or 0.0)
    cap = float(horizon.get("cap_strike") or 0.0)
    seconds_left = contract.get("seconds_left")
    try:
        seconds_left = int(seconds_left)
    except (TypeError, ValueError):
        return None
    yes_mid = float(contract.get("yes_mid") or 0.0)
    if spot <= 0.0 or floor <= 0.0 or cap > 0.0 or seconds_left <= 0 or not 0.0 < yes_mid < 1.0:
        return None
    minutes_left = seconds_left / 60.0
    vol = horizon.get("realized_volatility") or {}
    per_min_vol_bps = float(vol.get("per_min_bps") or 0.0)
    required_move_bps = float(horizon.get("required_move_bps") or 0.0)
    required_move_sigma = 0.0
    if per_min_vol_bps > 0.0 and minutes_left > 0.0:
        required_move_sigma = required_move_bps / (per_min_vol_bps * math.sqrt(minutes_left))
    result = {
        "signed_distance_bps": (spot - floor) / spot * 10000.0,
        "per_min_vol_bps": per_min_vol_bps,
        "sqrt_minutes_left": math.sqrt(minutes_left),
        "required_move_sigma": required_move_sigma,
        "realized_move_bps": float(horizon.get("realized_move_30s_bps") or 0.0),
        "yes_mid": yes_mid,
    }
    # Ensemble signals (hourly signal ensemble). book_imbalance is real here:
    # the daemon's own top-of-book sizes. trade_flow/spot_drift/news_forecast
    # need auxiliary sources (trade tape, spot series, btc-intelligence) that
    # extract_features does not have access to (it only sees one snapshot),
    # so they start neutral here and observe_cycle overrides them with the
    # real as-of reads once the aux sources are in hand — never guessed,
    # never omitted, so FULL_FEATURES always has a value for every key it
    # trains on, even when called standalone with no aux (as this function
    # always is).
    execution = snapshot.get("execution") or {}
    yes = execution.get("yes") or {}
    bid_sz = float(yes.get("bid_size") or 0.0)
    ask_sz = float(yes.get("ask_size") or 0.0)
    denom = bid_sz + ask_sz
    result["book_imbalance"] = (bid_sz - ask_sz) / denom if denom > 0.0 else 0.0
    result["trade_flow"] = 0.0      # overridden in observe_cycle
    result["spot_drift"] = 0.0      # overridden in observe_cycle
    result["news_forecast"] = 0.0   # overridden in observe_cycle
    result["event_pressure"] = 0.0  # overridden in observe_cycle
    return result


def extract_book(snapshot):
    """The daemon's observed top-of-book for both sides, passed through whole.

    Nothing here is modeled, learned, or derived: these are the quotes the
    daemon saw (kalshi_flow_execution_to_json in ServeCommand.cpp, the same
    normalized book `yes_mid` is the midpoint of), carried into the report so
    the bot can price a crossing bid against the spread it would actually pay
    instead of guessing one.

    A side the book does not quote is OMITTED, never zeroed: the bot's rule is
    to fail closed to passive quoting when it cannot see the ask it would have
    to cross, and a fabricated 0.0 would read as a free contract. Each side's
    own book is reported — Kalshi's NO book is a book, not `1 - yes_bid`.
    """
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


def trade_flow_feature(ticker, trades, now_ms, window_ms=TRADE_FLOW_WINDOW_MS):
    """Signed taker volume for `ticker`'s yes-contract trades over
    `(now_ms - window_ms, now_ms]`, normalized to roughly `[-1, 1]`.

    A row counts only when its `asset_id` EXACTLY matches `f"{ticker}:yes"`
    (never a prefix match -- ticker strings can be prefixes of one another,
    e.g. two strike-suffixed tickers sharing a stem) and its `ts` falls in
    the half-open window, strictly excluding `ts > now_ms`: that is the
    no-look-ahead guard, and it is enforced here, not by the caller.

    Side vocabulary: the real kalshi-trades.jsonl uses `BUY`/`SELL` (taker
    aggressor, uppercase) -- confirmed against the live evidence file, not
    assumed. `yes`/`no` is also accepted so any future producer using that
    vocabulary still scores. BUY/`yes` is taker demand for the yes contract
    (bullish, `+size`); SELL/`no` is taker supply (bearish, `-size`).

    `0.0` when there is no in-window volume for this ticker at all -- that is
    "no signal", not "flat signal", but they collapse to the same neutral
    value here as elsewhere in this module.
    """
    asset_id = "%s:yes" % ticker
    window_start = now_ms - window_ms
    net = 0.0
    denom = 0.0
    for row in trades or []:
        if row.get("asset_id") != asset_id:
            continue
        try:
            ts = float(row.get("ts"))
        except (TypeError, ValueError):
            continue
        if ts > now_ms or ts <= window_start:
            continue
        try:
            size = float(row.get("size") or 0.0)
        except (TypeError, ValueError):
            continue
        side = row.get("side")
        if side in ("yes", "BUY", "buy"):
            signed = size
        elif side in ("no", "SELL", "sell"):
            signed = -size
        else:
            continue
        net += signed
        denom += abs(size)
    if denom <= 0.0:
        return 0.0
    return net / (denom + 1.0)


# 15 minutes, expressed the way `required_move_sigma` already expresses vol
# lookback elsewhere in this module (per_min_vol_bps * sqrt(minutes)).
SPOT_DRIFT_LOOKBACK_MINUTES = SPOT_DRIFT_LOOKBACK_MS / 60000.0


def spot_drift_feature(spot_now, spot_prev, per_min_vol_bps,
                        lookback_minutes=SPOT_DRIFT_LOOKBACK_MINUTES):
    """Normalized recent spot move: the raw bps move from `spot_prev` to
    `spot_now`, scaled by the ambient vol expected over `lookback_minutes`
    (the same `per_min_vol_bps * sqrt(minutes)` convention `extract_features`
    already uses for `required_move_sigma`), clamped to `[-1, 1]`.

    `spot_prev` is meant to be a spot reading a short lookback before the
    decision -- in the real wiring, btc-intelligence.jsonl's ~hourly
    `current_spot` series, so this reads as the hourly directional move the
    brief names. `0.0` whenever either spot or the vol is missing or
    non-positive: there is no honest reading without both legs and a scale
    to measure them against.
    """
    try:
        spot_now = float(spot_now)
        spot_prev = float(spot_prev)
        per_min_vol_bps = float(per_min_vol_bps)
    except (TypeError, ValueError):
        return 0.0
    if spot_now <= 0.0 or spot_prev <= 0.0 or per_min_vol_bps <= 0.0:
        return 0.0
    move_bps = (spot_now - spot_prev) / spot_prev * 10000.0
    ambient_bps = per_min_vol_bps * math.sqrt(lookback_minutes)
    if ambient_bps <= 0.0:
        return 0.0
    return max(-1.0, min(1.0, move_bps / ambient_bps))


def news_forecast_feature(intel):
    """Centered directional read from a btc-intelligence-latest.json snapshot.

    The real payload (inspected against the live evidence file) has NO
    `forecast`/`p_up` field anywhere in it -- `abstention`, `adaptive_weights`,
    `calibration`, `as_of_ms` and siblings, none of them a per-decision
    directional probability. `news_context.score` is the best-available
    directional read instead: a signed "weighted narrative pressure" already
    centered at 0, roughly on a -100..100 scale (bullish stories positive,
    bearish negative, per the file's own `explanation` field), computed by
    the news scan the field name is actually about. It is divided by 100 and
    clamped to `[-1, 1]`; `0.0` when `intel` is `None`, unreadable, or the
    field is absent -- never guessed.
    """
    if not intel:
        return 0.0
    try:
        news_context = intel.get("news_context") or {}
        score = news_context.get("score")
        if score is None:
            return 0.0
        return max(-1.0, min(1.0, float(score) / 100.0))
    except (AttributeError, TypeError, ValueError):
        return 0.0


def event_pressure_feature(record, now_ms):
    """Signed, decaying pressure from live BTC events, as-of now_ms.

    Sum over events with event_ts_ms <= now_ms of
    direction*magnitude*0.5**(dt_hours/half_life_hours), clamped [-1,1].
    0.0 when record is None/empty/malformed or has no live event -- neutral,
    never a fabricated extreme. Pure: the disk read is in
    load_event_impact_latest, so this is unit-testable against an injected
    record.
    """
    if not record:
        return 0.0
    try:
        events = record.get("events")
    except AttributeError:
        return 0.0
    if not isinstance(events, list):
        return 0.0
    total = 0.0
    for e in events:
        try:
            ts = int(e["event_ts_ms"])
            hl = float(e["half_life_hours"])
            d = float(e["direction"])
            m = float(e["magnitude"])
        except (KeyError, TypeError, ValueError):
            continue
        if ts > now_ms or hl <= 0.0:
            continue
        dt_hours = (now_ms - ts) / 3_600_000.0
        total += d * m * (0.5 ** (dt_hours / hl))
    return max(-1.0, min(1.0, total))


def latest_spot_asof(spot_series, now_ms):
    """The most recent `(ts_ms, spot)` pair from `spot_series` with
    `ts_ms <= now_ms`; `None` when no such entry exists. Pure -- the disk
    read lives only in `load_spot_series_tail`, never here, so this is
    directly unit-testable against an injected series.
    """
    best = None
    for ts_ms, spot in spot_series or []:
        if ts_ms <= now_ms and (best is None or ts_ms > best[0]):
            best = (ts_ms, spot)
    return best[1] if best else None


def spot_prev_asof(spot_series, now_ms, lookback_ms=SPOT_DRIFT_LOOKBACK_MS):
    """The spot from `spot_series` for the lookback-target read, considering
    only entries with `ts_ms <= now_ms` (no look-ahead). `None` when the
    series has no such entry at all.

    Picks the MOST RECENT entry at or before `now_ms - lookback_ms`, not the
    entry with the smallest absolute distance to the target: on a sparse,
    coarsely-cadenced series (the real btc-intelligence.jsonl runs ~hourly,
    confirmed against the live file, not every 15 minutes) the nearest-by-
    distance entry is usually the LATEST point in the series -- the one
    `latest_spot_asof` already returns as `spot_now` -- which would make
    `spot_prev == spot_now` and `spot_drift` silently 0.0 on every cycle.
    Falls back to the OLDEST available entry only when nothing reaches back
    to the target yet (series shorter than the lookback), so a young series
    still yields a genuinely earlier reading instead of the newest one.
    """
    target = now_ms - lookback_ms
    candidates = [(ts_ms, spot) for ts_ms, spot in (spot_series or []) if ts_ms <= now_ms]
    if not candidates:
        return None
    at_or_before_target = [c for c in candidates if c[0] <= target]
    if at_or_before_target:
        return max(at_or_before_target, key=lambda c: c[0])[1]
    return min(candidates, key=lambda c: c[0])[1]


def _parse_trade_ts_ms(ts):
    """Normalize a trade row's `ts` to an int epoch-ms.

    The real kalshi-trades.jsonl carries an ISO-8601 string with a trailing
    `Z` (e.g. `"2026-08-01T01:25:34.052Z"`), confirmed against the live
    file -- not epoch seconds or ms as the brief's placeholder assumed. Test
    fixtures pass an int/float ms directly and are returned unchanged.

    The ISO string is parsed as UTC explicitly. A naive
    `strptime(...).timestamp()` interprets the parsed (naive) datetime as
    LOCAL time, which would skew every `ts` by the host's UTC offset --
    silently breaking the no-look-ahead guard in `trade_flow_feature` (real
    trades would look like they are from the future, or from hours in the
    past, depending on which way the offset runs).
    """
    if isinstance(ts, (int, float)):
        return int(ts)
    try:
        dt = datetime.datetime.fromisoformat(str(ts).replace("Z", "+00:00"))
        return int(dt.timestamp() * 1000)
    except (TypeError, ValueError):
        return None


def load_trades_tail(path=TRADES_PATH, max_lines=TRADES_TAIL_LINES):
    """Read the last `max_lines` of the trade tape, `ts` normalized to
    epoch-ms. Guarded: any read or parse failure yields `[]` (neutral),
    never raises -- this runs once per cycle from `load_auxiliary_sources`
    and must never crash the calibrator.
    """
    try:
        with open(path, "r", encoding="utf-8") as fh:
            lines = collections.deque(fh, maxlen=max_lines)
    except OSError:
        return []
    trades = []
    for line in lines:
        try:
            row = json.loads(line)
        except ValueError:
            continue
        ts_ms = _parse_trade_ts_ms(row.get("ts"))
        if ts_ms is None:
            continue
        row = dict(row)
        row["ts"] = ts_ms
        trades.append(row)
    return trades


def load_spot_series_tail(path=SPOT_SERIES_PATH, max_lines=SPOT_SERIES_TAIL_LINES):
    """Read the last `max_lines` of btc-intelligence.jsonl as `(ts_ms, spot)`
    pairs -- the same `as_of_ms`/`current_spot` fields -latest.json exposes
    for one cycle, here with history. Guarded: `[]` on any failure.
    """
    try:
        with open(path, "r", encoding="utf-8") as fh:
            lines = collections.deque(fh, maxlen=max_lines)
    except OSError:
        return []
    series = []
    for line in lines:
        try:
            row = json.loads(line)
            ts_ms = int(row.get("as_of_ms"))
            spot = float(row.get("current_spot"))
        except (TypeError, ValueError):
            continue
        series.append((ts_ms, spot))
    return series


def load_intel_latest(path=INTEL_LATEST_PATH, now_ms=None):
    """Load btc-intelligence-latest.json, guarded against a stale future
    write (no look-ahead, checked via its own `as_of_ms`) and any read
    failure. `None` on either -- `news_forecast_feature(None)` is `0.0`.
    """
    try:
        with open(path, "r", encoding="utf-8") as fh:
            intel = json.load(fh)
    except (OSError, ValueError):
        return None
    if now_ms is not None:
        try:
            as_of_ms = int(intel.get("as_of_ms"))
        except (TypeError, ValueError):
            return None
        if as_of_ms > now_ms:
            return None
    return intel


def load_event_impact_latest(path=EVENT_IMPACT_LATEST_PATH, now_ms=None):
    """Load btc-event-impact-latest.json, guarded against a stale future write
    (no look-ahead via its own as_of_ms) and any read failure. None on either
    -- event_pressure_feature(None) is 0.0. Mirrors load_intel_latest.
    """
    try:
        with open(path, "r", encoding="utf-8") as fh:
            record = json.load(fh)
    except (OSError, ValueError):
        return None
    if not isinstance(record, dict):
        return None  # valid JSON but not an object (list/str/number) -> neutral, never crash
    if now_ms is not None:
        try:
            as_of_ms = int(record.get("as_of_ms"))
        except (TypeError, ValueError):
            return None
        if as_of_ms > now_ms:
            return None
    return record


def load_auxiliary_sources(now_ms):
    """Read all four auxiliary sources once, as-of `now_ms`, guarded
    end-to-end so a missing or malformed file degrades to neutral instead of
    crashing the cycle. Called once per cycle from `run_once`; tests never
    call this -- they inject `aux` directly into `observe_cycle`, so no disk
    I/O happens under test.
    """
    return {
        "trades": load_trades_tail(),
        "spot_series": load_spot_series_tail(),
        "intel": load_intel_latest(now_ms=now_ms),
        "event_impact": load_event_impact_latest(now_ms=now_ms),
    }


class OnlineLogit:
    """Dependency-free online logistic regression with running z-scoring.

    Adagrad-scaled SGD on log-loss. Deterministic; state round-trips through
    plain JSON so a fresh process resumes exactly where the last one stopped.
    """

    def __init__(self, features, lr=0.1):
        self.features = tuple(features)
        self.lr = lr
        n = len(self.features)
        self.w = [0.0] * (n + 1)              # bias last
        self.g2 = [0.0] * (n + 1)
        self.n_seen = 0
        self.mean = [0.0] * n
        self.m2 = [0.0] * n

    def _standardize(self, x):
        z = []
        for i, v in enumerate(x):
            if self.n_seen >= MIN_STANDARDIZE_SAMPLES and self.m2[i] > 0.0:
                std = math.sqrt(self.m2[i] / self.n_seen)
                z.append((v - self.mean[i]) / std if std > 0.0 else 0.0)
            else:
                z.append(0.0)
        return z

    def _observe_stats(self, x):
        self.n_seen += 1
        for i, v in enumerate(x):
            delta = v - self.mean[i]
            self.mean[i] += delta / self.n_seen
            self.m2[i] += delta * (v - self.mean[i])

    def predict(self, feature_dict):
        x = [float(feature_dict[f]) for f in self.features]
        z = self._standardize(x)
        s = self.w[-1] + sum(wi * zi for wi, zi in zip(self.w, z))
        return 1.0 / (1.0 + math.exp(-max(-30.0, min(30.0, s))))

    def update(self, feature_dict, outcome, l2=0.0):
        x = [float(feature_dict[f]) for f in self.features]
        p = self.predict(feature_dict)
        self._observe_stats(x)
        z = self._standardize(x)
        err = p - (1.0 if outcome else 0.0)
        grads = z + [1.0]
        for i, g in enumerate(grads):
            gi = err * g
            if l2 and i < len(self.features):     # regularize weights, not the bias
                gi += l2 * self.w[i]
            self.g2[i] += gi * gi
            self.w[i] -= self.lr * gi / math.sqrt(1e-8 + self.g2[i])
        return p

    def to_json(self):
        return {"features": list(self.features), "lr": self.lr, "w": self.w,
                "g2": self.g2, "n_seen": self.n_seen, "mean": self.mean, "m2": self.m2}

    @classmethod
    def from_json(cls, blob):
        model = cls(blob["features"], blob.get("lr", 0.1))
        model.w = list(blob["w"])
        model.g2 = list(blob["g2"])
        model.n_seen = int(blob["n_seen"])
        model.mean = list(blob["mean"])
        model.m2 = list(blob["m2"])
        return model


def reconcile_full_model(blob):
    """Load a saved 'full' model into the current FULL_FEATURES, preserving the
    weights of features it already had and zero-initializing any new ones.

    A no-op when the blob already matches FULL_FEATURES. The standardizer
    stats (mean/m2/n_seen) are deliberately NOT carried across: n_seen is one
    shared scalar for every feature, so inheriting it would hand the newly
    added features a large sample count against zero observed variance and blow
    up their z-scores on first use. Starting fresh re-warms in
    MIN_STANDARDIZE_SAMPLES observations, which is cheap next to getting the
    scale wrong.
    """
    old = OnlineLogit.from_json(blob)
    if tuple(old.features) == FULL_FEATURES:
        return old
    fresh = OnlineLogit(FULL_FEATURES, lr=old.lr)
    old_index = {f: i for i, f in enumerate(old.features)}
    for i, f in enumerate(FULL_FEATURES):
        if f in old_index:
            fresh.w[i] = old.w[old_index[f]]
            fresh.g2[i] = old.g2[old_index[f]]
    fresh.w[-1] = old.w[-1]      # bias
    fresh.g2[-1] = old.g2[-1]
    return fresh


def brier(history):
    """Mean squared error of (probability, outcome) pairs; None when empty."""
    if not history:
        return None
    return sum((p - (1.0 if y else 0.0)) ** 2 for p, y in history) / len(history)


def mean_or_none(values):
    """Mean of already-computed per-contract scores; None when empty.

    Distinct from `brier()` on purpose: `brier()` takes forecast pairs and is
    the WITHIN-contract step, this takes one number per contract and is the
    ACROSS-contract step. Collapsing them would be exactly the conflation
    schema 2 exists to remove.
    """
    if not values:
        return None
    return sum(values) / len(values)


def _ablation_walk_forward(resolved_observations):
    """The no-look-ahead walk-forward at the heart of `ablation_report`,
    UNGATED: two model families (full + one ablated-per-ENSEMBLE_FEATURE)
    walked from cold over `resolved_observations` in order, each contract
    scored with `predict()` before any of them trains on it. Returns the raw
    per-contract score lists -- `(full_scores, market_scores, ablated_scores)`
    -- exactly as `ablation_report` means and then floor-gates them. Split out
    so the ordering invariant (score before train) stays testable on its own,
    independent of the `MIN_SCORED_CONTRACTS` floor `ablation_report` enforces
    on its SURFACED numbers.
    """
    full = OnlineLogit(FULL_FEATURES)
    ablated_models = {f: OnlineLogit(FULL_FEATURES) for f in ENSEMBLE_FEATURES}
    full_scores = []
    market_scores = []
    ablated_scores = {f: [] for f in ENSEMBLE_FEATURES}
    for observations, outcome in resolved_observations:
        if not observations:
            continue
        # --- score pass: every model predicts BEFORE any of them trains on
        # this contract, so every score here is out-of-sample. ---
        full_scores.append(brier([(full.predict(f), outcome) for f in observations]))
        market_scores.append(brier([(f["yes_mid"], outcome) for f in observations]))
        for feature_name, model in ablated_models.items():
            pinned = [dict(f, **{feature_name: 0.0}) for f in observations]
            ablated_scores[feature_name].append(
                brier([(model.predict(p), outcome) for p in pinned]))
        # --- train pass: now let every model learn this contract. ---
        for f in observations:
            full.update(f, outcome, l2=L2)
        for feature_name, model in ablated_models.items():
            for f in observations:
                model.update(dict(f, **{feature_name: 0.0}), outcome, l2=L2)
    return full_scores, market_scores, ablated_scores


def ablation_report(resolved_observations):
    """Per-signal ablation over already-resolved contracts: proves whether
    each ENSEMBLE_FEATURE earns its place, plus an overall beats-market
    verdict, using ONLY out-of-sample predictions.

    `resolved_observations` is a list of `(observations, outcome)` pairs, one
    per resolved contract, IN THE ORDER THE CONTRACTS RESOLVED — the same
    shape `settle_cycle` now appends to `state["resolved_record"]`
    (`observations` a list of feature dicts carrying every FULL_FEATURES key,
    `outcome` a bool).

    Walk-forward, no-look-ahead, mirroring settle_cycle's own discipline
    exactly: for each contract, in order, every model SCORES it first with
    `predict()` (so the score reflects only contracts that resolved earlier,
    never this one or a later one) and only THEN trains on it observation by
    observation. Contract-weighted, not observation-weighted, for the same
    reason the live scorer is (issue #171): one Brier per contract, meaned
    across contracts.

    Two model families are walked simultaneously, from cold, over the same
    contracts in the same order:
      - "full": one OnlineLogit(FULL_FEATURES), trained normally.
      - one "ablated" OnlineLogit(FULL_FEATURES) PER ENSEMBLE_FEATURE, trained
        and scored on a copy of every observation with that one feature
        pinned to its neutral value (0.0) — otherwise identical, including
        the same L2 as the live "full" model, so the comparison isolates
        exactly that feature's contribution, nothing else.

    Reports, per feature: `full_brier`, `ablated_brier`, and
    `brier_delta_vs_full = ablated_brier - full_brier` — POSITIVE means the
    feature HELPS (removing it made the Brier worse), matched consistently
    here, in the docstring, and in the test suite. `None` for all three
    (never a fabricated number) when there are no resolved contracts to
    replay yet, OR when there are fewer than `MIN_SCORED_CONTRACTS` of them —
    the same floor `build_report`'s live money-gate enforces (line ~99). On a
    small handful of contracts the walk-forward's own numbers are real but
    are pure noise as evidence; surfacing them (and letting `beats_market`
    flip to `True` on that noise) would be dishonest by the same standard the
    empty-record case already meets. Below the floor, `scored_contracts`
    still reports the TRUE count, so progress toward it stays visible even
    though the verdict does not.

    `beats_market`: whether the full model's contract-weighted Brier beats
    the untrained raw-mid Brier over these same contracts, once at least
    `MIN_SCORED_CONTRACTS` have been walked. `False` below that floor, or
    when there is no data at all, is "no evidence", not "the market won" —
    callers that need to tell the two apart should check `scored_contracts`
    too.
    """
    full_scores, market_scores, ablated_scores = _ablation_walk_forward(resolved_observations)
    full_brier = mean_or_none(full_scores)
    market_brier = mean_or_none(market_scores)
    # Same floor the live money-gate enforces in build_report (>= 100
    # CONTRACTS, never observations): below it, the walk-forward's numbers
    # are real but are noise as evidence, so they are withheld here exactly
    # like the empty-record case, not surfaced with a caveat. scored_contracts
    # is set from len(full_scores) below, unaffected by this gate, so
    # progress toward the floor stays visible.
    enough = len(full_scores) >= MIN_SCORED_CONTRACTS
    ablated_briers = {f: mean_or_none(ablated_scores[f]) for f in ENSEMBLE_FEATURES}
    if not enough:
        full_brier = None
        market_brier = None
        ablated_briers = {f: None for f in ENSEMBLE_FEATURES}
    report = {}
    for feature_name in ENSEMBLE_FEATURES:
        ablated_brier = ablated_briers[feature_name]
        delta = (ablated_brier - full_brier
                 if ablated_brier is not None and full_brier is not None else None)
        report[feature_name] = {
            "full_brier": full_brier,
            "ablated_brier": ablated_brier,
            "brier_delta_vs_full": delta,
        }
    report["full_brier"] = full_brier
    report["market_mid_brier"] = market_brier
    report["scored_contracts"] = len(full_scores)
    report["beats_market"] = (enough and full_brier is not None and market_brier is not None
                              and full_brier < market_brier)
    return report


def resolve_outcome_kalshi(ticker, fetcher=None):
    """True/False once the market settled, None while open or on any error."""
    try:
        if fetcher is None:
            def fetcher(url):
                with urllib.request.urlopen(url, timeout=10) as resp:
                    return json.loads(resp.read().decode("utf-8"))
        payload = fetcher(KALSHI_MARKET_URL.format(ticker=ticker))
        market = payload.get("market") or {}
        if market.get("status") not in ("settled", "finalized"):
            return None
        result = market.get("result")
        return {"yes": True, "no": False}.get(result)
    except Exception:
        return None


def default_state():
    # The three score lists hold ONE FLOAT PER RESOLVED CONTRACT — that
    # contract's mean squared error — not forecast pairs. Same contracts, same
    # order, in all three, so they are always comparable like for like.
    return {"schema": STATE_SCHEMA, "advisory_only": True,
            "full": OnlineLogit(FULL_FEATURES).to_json(),
            "market": OnlineLogit(MARKET_FEATURES).to_json(),
            "pending": {},                    # ticker -> {"close_ms": int, "obs": [features...]}
            "contract_scores_full": [],
            "contract_scores_market_trained_logit": [],
            "contract_scores_market_mid_raw": [],
            # One {"observations": [features...], "outcome": bool} per resolved
            # contract, same window/order as the contract_scores_* lists above.
            # This is the ONLY place a contract's raw (features, outcome) survive
            # past settle_cycle -- ablation_report's walk-forward has no other
            # source of ground truth to replay. Bounded the same way for the
            # same reason (issue #171: this module stays frugal on purpose).
            "resolved_record": [],
            "resolved": 0, "skipped_unmodeled": 0}


def migrate_state(state):
    """Bring a state file up to `STATE_SCHEMA`, discarding what it cannot mean.

    A schema-1 file's `brier_full` holds per-OBSERVATION pairs. There is no
    honest way to turn 500 of those into contract scores — the file does not
    record which pairs belonged to which contract — so they are DROPPED and
    counted, never reinterpreted as if they had been contracts all along. The
    calibrator then reads as having scored zero contracts, which is the truth,
    and stays below the gate until real contracts settle under the new rule.

    The trained models and the pending map survive untouched: the weights were
    always fitted per observation and that has not changed.
    """
    if int(state.get("schema") or 0) >= STATE_SCHEMA:
        return state
    fresh = default_state()
    for key in ("full", "market", "pending", "resolved", "skipped_unmodeled", "resolved_record"):
        if key in state:
            fresh[key] = state[key]
    fresh["migrated_from_schema"] = state.get("schema")
    fresh["discarded_observation_pairs"] = len(state.get("brier_full") or [])
    return fresh


def load_state(path=STATE_PATH):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return migrate_state(json.load(fh))
    except (OSError, ValueError):
        return default_state()


def save_json_atomic(payload, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(payload, fh)
    os.replace(tmp, path)


def observe_cycle(state, evidence, now_ms, aux=None):
    """Record one observation per modeled active snapshot; predict for output.

    `aux` bundles the four auxiliary sources (see `load_auxiliary_sources`):
    `trades`, `spot_series`, `intel`, `event_impact`. It is optional and defaults to empty/
    `None`, which reproduces the Task-1 neutral 0.0 stubs exactly -- every
    caller from before this task (all pre-Task-2 tests, and any future one
    that does not care about the ensemble signals) keeps working unchanged.
    `spot_now`/`spot_prev`/the news read are resolved ONCE per cycle here
    (they are market-wide, not per-contract); `trade_flow` is resolved per
    ticker since it depends on which market's trades are being summed.
    """
    aux = aux or {}
    trades = aux.get("trades") or []
    spot_series = aux.get("spot_series") or []
    intel = aux.get("intel")
    spot_now = latest_spot_asof(spot_series, now_ms) or 0.0
    spot_prev = spot_prev_asof(spot_series, now_ms) or 0.0
    news_forecast = news_forecast_feature(intel)
    event_pressure = event_pressure_feature(aux.get("event_impact"), now_ms)
    full = reconcile_full_model(state["full"])
    market = OnlineLogit.from_json(state["market"])
    predictions = {}
    for ticker, snapshot in (evidence.get("snapshots") or {}).items():
        features = extract_features(snapshot)
        if features is None:
            state["skipped_unmodeled"] += 1
            continue
        # Replace the Task-1 neutral 0.0 stubs with the real, as-of reads
        # BEFORE this goes into entry["obs"] -- that list is what
        # settle_cycle trains the model on, so it must see real values.
        features["trade_flow"] = trade_flow_feature(ticker, trades, now_ms)
        features["spot_drift"] = spot_drift_feature(spot_now, spot_prev, features["per_min_vol_bps"])
        features["news_forecast"] = news_forecast
        features["event_pressure"] = event_pressure
        close_ms = now_ms + int((snapshot.get("contract") or {}).get("seconds_left") or 0) * 1000
        entry = state["pending"].setdefault(ticker, {"close_ms": close_ms, "obs": []})
        entry["close_ms"] = close_ms
        if len(entry["obs"]) < MAX_OBS_PER_TICKER:
            entry["obs"].append(features)
        predictions[ticker] = {
            "p_yes_full": full.predict(features),
            "p_yes_market_baseline": market.predict(features),
            "market_yes_mid": features["yes_mid"],
            "features": features,
        }
        # Book passthrough, deliberately OUTSIDE `features`: FULL_FEATURES and
        # MARKET_FEATURES are the trained models' input tuples and round-trip
        # through the saved state, so adding to them would retrain nothing and
        # invalidate everything. These keys are evidence for the reader (and
        # for the bot's crossing tier), not signal for the model.
        predictions[ticker].update(extract_book(snapshot))
    return predictions


def settle_cycle(state, now_ms, resolver=resolve_outcome_kalshi):
    """Train both models on every pending ticker whose market has settled.

    Scoring and training are deliberately two passes over the same
    observations. The scoring pass runs FIRST and uses `predict()`, so all
    three scores for a contract are read off the models as they stood before
    this contract's outcome existed for them; the training pass then updates
    the weights observation by observation as it always has. One score per
    contract goes into each list (see the module header for the rule).
    """
    # settle_cycle is the sole place that persists state["full"] back to disk
    # (observe_cycle's load is predict-only and never saved), so the migration
    # from an old saved model to FULL_FEATURES has to happen HERE, not just in
    # observe_cycle, or the on-disk model would never advance past its old
    # feature tuple.
    full = reconcile_full_model(state["full"])
    market = OnlineLogit.from_json(state["market"])
    # Guarded, not assumed: a state loaded from disk that predates this field
    # (any already-schema-2 file — migrate_state only backfills schema-1
    # states) would otherwise KeyError the first time a contract resolves.
    state.setdefault("resolved_record", [])
    for ticker in list(state["pending"].keys()):
        entry = state["pending"][ticker]
        if now_ms < entry["close_ms"] + 120_000:   # grace for settlement to post
            continue
        outcome = resolver(ticker)
        if outcome is None:
            if now_ms > entry["close_ms"] + 24 * 3600 * 1000:
                del state["pending"][ticker]       # unresolvable; drop, don't guess
            continue
        observations = entry["obs"]
        if not observations:
            # Settled with nothing observed: there is no forecast to score, so
            # none is recorded. It still leaves pending — an empty contract is
            # resolved, not evidence.
            del state["pending"][ticker]
            continue
        # One pre-training score per model, over this contract's own rows.
        state["contract_scores_full"].append(
            brier([(full.predict(f), outcome) for f in observations]))
        state["contract_scores_market_trained_logit"].append(
            brier([(market.predict(f), outcome) for f in observations]))
        state["contract_scores_market_mid_raw"].append(
            brier([(f["yes_mid"], outcome) for f in observations]))
        # The raw material ablation_report replays: this contract's own
        # observations paired with its outcome, captured here (same place,
        # same moment as the three scores above) because nowhere else keeps it.
        state["resolved_record"].append({"observations": observations, "outcome": outcome})
        for features in observations:
            full.update(features, outcome, l2=L2)
            market.update(features, outcome)
        state["resolved"] += 1
        del state["pending"][ticker]
    for key in ("contract_scores_full", "contract_scores_market_trained_logit",
                "contract_scores_market_mid_raw", "resolved_record"):
        state[key] = state[key][-SCORED_CONTRACT_WINDOW:]
    state["full"] = full.to_json()
    state["market"] = market.to_json()


def build_report(state, predictions, now_ms):
    """The report every consumer reads. Contracts and observations never share
    a field: `scored_contracts` is the Brier's denominator, `resolved_contracts`
    is the lifetime settled count, and `training_observations` is how many rows
    the weights were fitted on. After a schema-1 migration the first two
    disagree loudly (hundreds resolved, zero scored) — that is the migration
    reading as insufficient, exactly as intended.
    """
    scored = state["contract_scores_full"]
    b_full = mean_or_none(scored)
    b_logit = mean_or_none(state["contract_scores_market_trained_logit"])
    b_mid_raw = mean_or_none(state["contract_scores_market_mid_raw"])
    # The gate, stated where it is enforced: at least MIN_SCORED_CONTRACTS = 100
    # CONTRACTS (not observations), and a Brier strictly better than the RAW
    # MID — the price the bot would otherwise take. Beating the trained logit
    # is reported beside it but does not confer trust: that baseline is
    # handicapped, so clearing it is not the claim the field name makes.
    enough = len(scored) >= MIN_SCORED_CONTRACTS
    return {
        "schema": 2, "event": "spot_calibrator", "advisory_only": True,
        "generated_at_ms": now_ms,
        "resolved_contracts": state["resolved"],
        "scored_contracts": len(scored),
        "training_observations": int((state.get("full") or {}).get("n_seen") or 0),
        "scoring_rule": ("one score per contract: mean squared error over that "
                         "contract's observations, predicted before training on it"),
        "min_scored_contracts": MIN_SCORED_CONTRACTS,
        "brier_full": b_full,
        "brier_market_mid_raw": b_mid_raw,
        "brier_market_trained_logit": b_logit,
        "adds_value_over_market": (b_full is not None and b_mid_raw is not None
                                   and enough and b_full < b_mid_raw),
        "beats_trained_logit_baseline": (b_full is not None and b_logit is not None
                                         and enough and b_full < b_logit),
        "predictions": predictions,
        # Additive-only (Task 3): per-signal ablation over the retained
        # resolved-contract record, plus its own beats-market verdict. Does
        # not touch or gate any field above -- see ablation_report's docstring
        # for the walk-forward discipline and the delta sign convention.
        "ablation": ablation_report(
            [(r["observations"], r["outcome"]) for r in (state.get("resolved_record") or [])]),
    }


def run_once(now_ms=None):
    now_ms = now_ms if now_ms is not None else int(time.time() * 1000)
    try:
        with open(EVIDENCE_PATH, "r", encoding="utf-8") as fh:
            evidence = json.load(fh)
    except (OSError, ValueError):
        return {"error": "evidence file unavailable", "path": EVIDENCE_PATH}
    state = load_state()
    aux = load_auxiliary_sources(now_ms)
    predictions = observe_cycle(state, evidence, now_ms, aux=aux)
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
            print(json.dumps({"cycle": result.get("generated_at_ms"),
                              "resolved": result.get("resolved_contracts"),
                              "error": result.get("error")}), flush=True)
            time.sleep(interval)
    print("usage: spot_calibrator.py [once|run [--interval N]|report]", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
