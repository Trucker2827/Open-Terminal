#!/usr/bin/env python3
"""Shared outside-info features for 15m race calibrators (Phase 3–4 observe).

Veto/confirm + scored-only mid-prior tilt — never invents trust. Used by
commodities_15m_calibrator and kxbtc15m_calibrator.
"""
from __future__ import annotations

import datetime
import math
import zoneinfo

EASTERN = zoneinfo.ZoneInfo("America/New_York")
TAPE_LOOKBACK_MS = 3 * 60 * 1000
TAPE_MOVED_BPS = 1.5
NEAR_CLOSE_S = 180
# Absolute per-minute vol floors (bps). Quiet markets get clamped to mid.
VOL_QUIET_BPS = 3.0
VOL_ELEVATED_BPS = 12.0
# Phase-4 observe: max |Δlogit| when updating mid prior (~±5¢ near 0.5).
TILT_MAX_ABS_LOGIT = 0.20
_PROB_EPS = 1e-6


def session_regime_at(now_ms):
    """Coarse US/Eastern session bucket for vol priors.

    Returns one of: weekend | overnight | rth | evening.
    """
    try:
        dt = datetime.datetime.fromtimestamp(now_ms / 1000.0, tz=EASTERN)
    except (TypeError, ValueError, OSError, OverflowError):
        return "unknown"
    if dt.weekday() >= 5:
        return "weekend"
    minutes = dt.hour * 60 + dt.minute
    # COMEX metals / energy liquid day + equity RTH overlap.
    if 8 * 60 + 20 <= minutes < 16 * 60 + 0:
        return "rth"
    if 16 * 60 <= minutes < 20 * 60:
        return "evening"
    return "overnight"


def vol_regime(per_min_vol_bps):
    """quiet | normal | elevated from realized per-minute vol (bps)."""
    try:
        vol = float(per_min_vol_bps or 0.0)
    except (TypeError, ValueError):
        return "unknown"
    if vol <= 0.0:
        return "unknown"
    if vol < VOL_QUIET_BPS:
        return "quiet"
    if vol >= VOL_ELEVATED_BPS:
        return "elevated"
    return "normal"


def series_change_bps(series, now_ms, lookback_ms=TAPE_LOOKBACK_MS):
    """Signed bps change of a (ts_ms, price) series over lookback. None if hole."""
    if not series:
        return None
    pts = [(int(t), float(p)) for t, p in series if p and float(p) > 0.0]
    if len(pts) < 2:
        return None
    latest_ts, latest_px = pts[-1]
    if abs(latest_ts - int(now_ms)) > lookback_ms:
        # Stale series — refuse rather than invent a lead.
        if latest_ts < int(now_ms) - lookback_ms:
            return None
    target = int(now_ms) - int(lookback_ms)
    prior = None
    for ts, px in pts:
        if ts <= target:
            prior = px
        else:
            break
    if prior is None:
        prior = pts[0][1]
    if prior <= 0.0 or latest_px <= 0.0:
        return None
    return (latest_px / prior - 1.0) * 10000.0


def futures_tape_flags(spot, open_price, yahoo_change_bps):
    """Confirm/conflict between race direction (spot vs open) and futures tape."""
    try:
        spot = float(spot)
        open_price = float(open_price)
    except (TypeError, ValueError):
        return {
            "tape_change_bps": None,
            "tape_confirms": False,
            "tape_conflicts": False,
            "tape_available": False,
        }
    if yahoo_change_bps is None:
        return {
            "tape_change_bps": None,
            "tape_confirms": False,
            "tape_conflicts": False,
            "tape_available": False,
        }
    try:
        tape = float(yahoo_change_bps)
    except (TypeError, ValueError):
        return {
            "tape_change_bps": None,
            "tape_confirms": False,
            "tape_conflicts": False,
            "tape_available": False,
        }
    race_up = spot > open_price
    race_down = spot < open_price
    tape_up = tape > TAPE_MOVED_BPS
    tape_down = tape < -TAPE_MOVED_BPS
    conflicts = (race_up and tape_down) or (race_down and tape_up)
    confirms = (race_up or race_down) and not conflicts and (
        (not tape_up and not tape_down)  # sticky tape while race has direction
        or (race_up and tape_up)
        or (race_down and tape_down)
    )
    # Sticky tape with a race direction is a weak confirm for near-close —
    # require actual tape move for stronger confirm used by ablation.
    confirms_strong = (race_up and tape_up) or (race_down and tape_down)
    return {
        "tape_change_bps": tape,
        "tape_confirms": bool(confirms_strong),
        "tape_conflicts": bool(conflicts),
        "tape_available": True,
        "tape_sticky_ok": bool(confirms and not confirms_strong),
    }


def ablation_tape_confirm_near_close(p_physics, yes_mid, seconds_left, tape_confirms,
                                     near_close_s=NEAR_CLOSE_S):
    """Leave mid unless near close *and* futures tape confirms race direction."""
    p_physics = float(p_physics)
    yes_mid = float(yes_mid)
    try:
        seconds_left = int(seconds_left)
    except (TypeError, ValueError):
        return yes_mid
    if seconds_left > near_close_s:
        return p_physics
    return p_physics if tape_confirms else yes_mid


def ablation_vol_regime_confirm(p_physics, yes_mid, per_min_vol_bps, session=None):
    """Quiet (or weekend) regimes clamp to mid — confirm only when ambient vol is alive."""
    p_physics = float(p_physics)
    yes_mid = float(yes_mid)
    regime = vol_regime(per_min_vol_bps)
    if session == "weekend":
        return yes_mid
    if regime == "quiet":
        return yes_mid
    return p_physics


def _finite_prob(value):
    try:
        p = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(p):
        return None
    return p


def logit(p, eps=_PROB_EPS):
    """log-odds; clamps to (eps, 1-eps). Raises ValueError if p is not finite."""
    p = _finite_prob(p)
    if p is None:
        raise ValueError("logit: non-finite probability")
    p = min(max(p, eps), 1.0 - eps)
    return math.log(p / (1.0 - p))


def sigmoid(x):
    """Inverse logit."""
    try:
        x = float(x)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(x):
        return None
    # Stable sigmoid.
    if x >= 0.0:
        z = math.exp(-x)
        return 1.0 / (1.0 + z)
    z = math.exp(x)
    return z / (1.0 + z)


def capped_mid_prior_tilt(mid, p_private, max_abs_logit=TILT_MAX_ABS_LOGIT):
    """Market mid prior × private likelihood, capped in log-odds space.

    logit(p_post) = logit(mid) + clip(logit(p_private) − logit(mid), ±cap)

    Fail-closed: invalid mid → None; invalid private → mid (no update).
    """
    mid_p = _finite_prob(mid)
    if mid_p is None:
        return None
    try:
        cap = abs(float(max_abs_logit))
    except (TypeError, ValueError):
        cap = TILT_MAX_ABS_LOGIT
    if not math.isfinite(cap) or cap <= 0.0:
        return mid_p
    priv = _finite_prob(p_private)
    if priv is None:
        return mid_p
    try:
        delta = logit(priv) - logit(mid_p)
    except ValueError:
        return mid_p
    if delta > cap:
        delta = cap
    elif delta < -cap:
        delta = -cap
    post = sigmoid(logit(mid_p) + delta)
    return mid_p if post is None else post


def mean_or_none(values):
    if not values:
        return None
    return sum(values) / len(values)


def paired_ablation_scoreboard(series_map, mid_scores, min_contracts):
    """Per-variant Brier vs mid on overlapping trailing windows."""
    mid_scores = mid_scores or []
    global_mid = mean_or_none(mid_scores)
    out = {}
    for name, scores in (series_map or {}).items():
        scores = scores or []
        n = min(len(scores), len(mid_scores))
        if n <= 0:
            out[name] = {
                "brier": None,
                "brier_mid_paired": None,
                "beats_mid": False,
                "scored_contracts": 0,
            }
            continue
        brier_val = mean_or_none(scores[-n:])
        b_mid = mean_or_none(mid_scores[-n:])
        beats = (brier_val is not None and b_mid is not None
                 and n >= min_contracts and brier_val < b_mid)
        out[name] = {
            "brier": brier_val,
            "brier_mid_paired": b_mid,
            "beats_mid": beats,
            "scored_contracts": n,
        }
    return out, global_mid


def select_best_trusted(board, keys):
    winners = []
    for name in keys:
        row = (board or {}).get(name) or {}
        if row.get("beats_mid") and row.get("brier") is not None:
            winners.append((row["brier"], name))
    if not winners:
        return None
    winners.sort()
    return winners[0][1]
