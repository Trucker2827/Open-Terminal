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
KALSHI_MARKET_URL = "https://api.elections.kalshi.com/trade-api/v2/markets/{ticker}"

FULL_FEATURES = ("signed_distance_bps", "per_min_vol_bps", "sqrt_minutes_left",
                 "required_move_sigma", "realized_move_bps", "yes_mid")
MARKET_FEATURES = ("yes_mid",)
MAX_OBS_PER_TICKER = 60
MIN_STANDARDIZE_SAMPLES = 8
STATE_SCHEMA = 2
# Both in CONTRACTS, never observations. See the module header for why the
# distinction is the whole point of schema 2.
SCORED_CONTRACT_WINDOW = 500
MIN_SCORED_CONTRACTS = 100


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
    for key in ("full", "market", "pending", "resolved", "skipped_unmodeled"):
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
        for features in observations:
            full.update(features, outcome)
            market.update(features, outcome)
        state["resolved"] += 1
        del state["pending"][ticker]
    for key in ("contract_scores_full", "contract_scores_market_trained_logit",
                "contract_scores_market_mid_raw"):
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
