#!/usr/bin/env python3
"""Online probability calibrator for Kalshi hourly crypto contracts.

Advisory-only, engine-side evidence: learns P(settles YES) from the daemon's
decision snapshots (kalshi-ws-books.json) and updates itself on every public
settlement. It never touches the frozen duel scoring files and never places
orders; its output is one more evidence file beside the order books.

Two models are trained in parallel and BOTH are always reported:
  - "full":   physics features (signed distance, ambient vol, time, realized
              move) plus the market mid.
  - "market": the market mid alone.
If the full model's Brier score is not beating the market baseline, the
calibrator is adding nothing and the report says so — by construction.

Commands:  once | run [--interval 60] | report
"""
import json
import math
import os
import sys
import time
import urllib.request

EVIDENCE_PATH = os.path.expanduser(
    "~/Library/Application Support/Open Terminal/Open Terminal/kalshi-ws-books.json")
STATE_PATH = os.path.expanduser(
    "~/Library/Application Support/Open Terminal/Open Terminal/spot-calibrator-state.json")
OUTPUT_PATH = os.path.expanduser(
    "~/Library/Application Support/Open Terminal/Open Terminal/calibrator.json")
KALSHI_MARKET_URL = "https://api.elections.kalshi.com/trade-api/v2/markets/{ticker}"

FULL_FEATURES = ("signed_distance_bps", "per_min_vol_bps", "sqrt_minutes_left",
                 "required_move_sigma", "realized_move_bps", "yes_mid")
MARKET_FEATURES = ("yes_mid",)
MAX_OBS_PER_TICKER = 60
MIN_STANDARDIZE_SAMPLES = 8
BRIER_WINDOW = 500


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
    return {
        "signed_distance_bps": (spot - floor) / spot * 10000.0,
        "per_min_vol_bps": per_min_vol_bps,
        "sqrt_minutes_left": math.sqrt(minutes_left),
        "required_move_sigma": required_move_sigma,
        "realized_move_bps": float(horizon.get("realized_move_30s_bps") or 0.0),
        "yes_mid": yes_mid,
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

    def update(self, feature_dict, outcome):
        x = [float(feature_dict[f]) for f in self.features]
        p = self.predict(feature_dict)
        self._observe_stats(x)
        z = self._standardize(x)
        err = p - (1.0 if outcome else 0.0)
        grads = z + [1.0]
        for i, g in enumerate(grads):
            gi = err * g
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


def brier(history):
    """Mean squared error of (probability, outcome) pairs; None when empty."""
    if not history:
        return None
    return sum((p - (1.0 if y else 0.0)) ** 2 for p, y in history) / len(history)


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
    return {"schema": 1, "advisory_only": True,
            "full": OnlineLogit(FULL_FEATURES).to_json(),
            "market": OnlineLogit(MARKET_FEATURES).to_json(),
            "pending": {},          # ticker -> {"close_ms": int, "obs": [features...]}
            "brier_full": [],       # trailing [probability, outcome] pairs
            "brier_market": [],
            "resolved": 0, "skipped_unmodeled": 0}


def load_state(path=STATE_PATH):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return default_state()


def save_json_atomic(payload, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(payload, fh)
    os.replace(tmp, path)


def observe_cycle(state, evidence, now_ms):
    """Record one observation per modeled active snapshot; predict for output."""
    full = OnlineLogit.from_json(state["full"])
    market = OnlineLogit.from_json(state["market"])
    predictions = {}
    for ticker, snapshot in (evidence.get("snapshots") or {}).items():
        features = extract_features(snapshot)
        if features is None:
            state["skipped_unmodeled"] += 1
            continue
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
    return predictions


def settle_cycle(state, now_ms, resolver=resolve_outcome_kalshi):
    """Train both models on every pending ticker whose market has settled."""
    full = OnlineLogit.from_json(state["full"])
    market = OnlineLogit.from_json(state["market"])
    for ticker in list(state["pending"].keys()):
        entry = state["pending"][ticker]
        if now_ms < entry["close_ms"] + 120_000:   # grace for settlement to post
            continue
        outcome = resolver(ticker)
        if outcome is None:
            if now_ms > entry["close_ms"] + 24 * 3600 * 1000:
                del state["pending"][ticker]       # unresolvable; drop, don't guess
            continue
        for features in entry["obs"]:
            p_full = full.update(features, outcome)
            p_market = market.update(features, outcome)
            state["brier_full"].append([p_full, bool(outcome)])
            state["brier_market"].append([p_market, bool(outcome)])
        state["resolved"] += 1
        del state["pending"][ticker]
    state["brier_full"] = state["brier_full"][-BRIER_WINDOW:]
    state["brier_market"] = state["brier_market"][-BRIER_WINDOW:]
    state["full"] = full.to_json()
    state["market"] = market.to_json()


def build_report(state, predictions, now_ms):
    b_full = brier([(p, y) for p, y in state["brier_full"]])
    b_market = brier([(p, y) for p, y in state["brier_market"]])
    return {
        "schema": 1, "event": "spot_calibrator", "advisory_only": True,
        "generated_at_ms": now_ms,
        "resolved_contracts": state["resolved"],
        "training_samples": len(state["brier_full"]),
        "brier_full": b_full, "brier_market_baseline": b_market,
        "adds_value_over_market": (b_full is not None and b_market is not None
                                   and len(state["brier_full"]) >= 100 and b_full < b_market),
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
            print(json.dumps({"cycle": result.get("generated_at_ms"),
                              "resolved": result.get("resolved_contracts"),
                              "error": result.get("error")}), flush=True)
            time.sleep(interval)
    print("usage: spot_calibrator.py [once|run [--interval N]|report]", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
