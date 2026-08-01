# Kalshi hourly signal ensemble — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the calibrator's online logistic model with four already-collected signals (book imbalance, trade flow, spot drift, news forecast) so the hourly `p_yes_full` is a genuine multi-signal read — under the unchanged no-look-ahead + beats-the-market discipline.

**Architecture:** Pure Python change to `spot_calibrator.py`. Add feature names to `FULL_FEATURES`; compute them (book imbalance from the snapshot; trade flow / spot drift / news forecast from auxiliary evidence joined *as-of* `now_ms`); add L2 to the AdaGrad update; migrate saved state (zero-init the new weights). The report schema is unchanged, so the C++ bot/gate/cockpit need no changes.

**Tech Stack:** Python 3, stdlib only (no new deps); `unittest` (`test_spot_calibrator.py`).

## Global Constraints

- **Report schema unchanged.** `build_report` must keep emitting `p_yes_full`, `p_yes_market_baseline`, `market_yes_mid`, per-contract `features`, `adds_value_over_market`, `brier_full`, `brier_market_mid_raw`, `resolved_contracts`, `scored_contracts`. Add fields only; never remove/rename.
- **No look-ahead.** Every auxiliary signal is read with timestamp **≤ the snapshot's decision time**; the model predicts before it trains on the outcome (existing pattern, do not break).
- **Neutral on missing.** An absent/stale/garbage auxiliary source yields the feature's neutral value (0.0 pre-standardization → the online mean), never a crash and never a fabricated extreme.
- **Discipline is the safety net.** The beats-the-market gate (`adds_value_over_market`) is unchanged — a useless signal only leaves the forecast untrusted; it can never make the bot bet noise. Do not weaken it.
- **Anchors** (`openmarketterminal-qt/scripts/kalshi_advise/spot_calibrator.py`): `FULL_FEATURES` (~line 61), `extract_features` (~73), `extract_book` (~110; note `execution[side]` carries `bid_size`/`ask_size`), `OnlineLogit.update` AdaGrad (~178), `observe_cycle` join point (~297, features dict built before `full.predict`), `build_report` (~375). Trades: `kalshi-trades.jsonl` rows `{asset_id, side, size, price, ts}` (asset_id is `<ticker>:yes`/`:no`). News: `btc-intelligence-latest.json`. Tests: `openmarketterminal-qt/tests/test_spot_calibrator.py`. Run: `python3 -m unittest openmarketterminal-qt.tests.test_spot_calibrator` (or the repo's usual invocation — match the existing test file's import style).

---

### Task 1: Model scaffolding — new features, L2, state migration, `book_imbalance`

**Files:**
- Modify: `openmarketterminal-qt/scripts/kalshi_advise/spot_calibrator.py`
- Test: `openmarketterminal-qt/tests/test_spot_calibrator.py`

**Interfaces:**
- Produces: `FULL_FEATURES` extended with `("book_imbalance", "trade_flow", "spot_drift", "news_forecast")`; `extract_features` returns those keys (real value for `book_imbalance`, `0.0` neutral stubs for the other three until Task 2); `OnlineLogit.update` takes an `l2` term; state migration reconciles an old saved model to the new feature tuple.

- [ ] **Step 1: Write the failing tests**

Add to `test_spot_calibrator.py`:

```python
def test_book_imbalance_from_snapshot_sizes(self):
    # execution.yes carries bid_size/ask_size; imbalance is (bid-ask)/(bid+ask).
    snap = make_snapshot(yes_bid_size=300, yes_ask_size=100)   # helper below/extend existing
    feats = spot_calibrator.extract_features(snap)
    self.assertAlmostEqual(feats["book_imbalance"], 0.5)       # (300-100)/(300+100)
    self.assertEqual(feats["trade_flow"], 0.0)                 # neutral stub (Task 2)
    self.assertEqual(feats["spot_drift"], 0.0)
    self.assertEqual(feats["news_forecast"], 0.0)

def test_full_features_include_the_four_new_signals(self):
    for f in ("book_imbalance", "trade_flow", "spot_drift", "news_forecast"):
        self.assertIn(f, spot_calibrator.FULL_FEATURES)

def test_l2_shrinks_a_weight_toward_zero(self):
    # With no error signal, repeated L2-regularized updates pull a nonzero
    # feature weight toward 0; the bias is NOT regularized.
    m = spot_calibrator.OnlineLogit(("x",))
    m.w[0] = 5.0
    for _ in range(200):
        m.update({"x": 0.0}, outcome=False, l2=0.1)   # x standardized ~0 => no data gradient
    self.assertLess(abs(m.w[0]), 5.0)

def test_state_migrates_old_model_to_new_features(self):
    # An old saved "full" model with only the physics features loads into the
    # new FULL_FEATURES with the physics weights preserved and the four new
    # weights zero-initialized.
    old = spot_calibrator.OnlineLogit(spot_calibrator.PHYSICS_FEATURES)  # the pre-ensemble tuple
    old.w = [1.0] * (len(spot_calibrator.PHYSICS_FEATURES) + 1)
    migrated = spot_calibrator.reconcile_full_model(old.to_json())
    self.assertEqual(tuple(migrated.features), spot_calibrator.FULL_FEATURES)
    for f in ("book_imbalance", "trade_flow", "spot_drift", "news_forecast"):
        self.assertEqual(migrated.w[migrated.features.index(f)], 0.0)
```

Add a `make_snapshot(**overrides)` helper beside the existing fixtures (reuse whatever the file already builds; it must produce `contract.horizon.spot/floor_strike/seconds_left/realized_volatility` and `execution.yes.bid_size/ask_size`).

- [ ] **Step 2: Run — confirm fail**

Run: `python3 -m unittest openmarketterminal-qt.tests.test_spot_calibrator -v`
Expected: `AttributeError`/`KeyError` (no `book_imbalance`, no `reconcile_full_model`, `update` has no `l2`).

- [ ] **Step 3: Extend `FULL_FEATURES` and preserve the old tuple name**

Keep the physics tuple under a name the migration can reference, then extend:

```python
PHYSICS_FEATURES = ("signed_distance_bps", "per_min_vol_bps", "sqrt_minutes_left",
                    "required_move_sigma", "realized_move_bps", "yes_mid")
ENSEMBLE_FEATURES = ("book_imbalance", "trade_flow", "spot_drift", "news_forecast")
FULL_FEATURES = PHYSICS_FEATURES + ENSEMBLE_FEATURES
```

- [ ] **Step 4: Compute `book_imbalance`; neutral-stub the other three**

In `extract_features`, after the physics dict is built (and before returning), read the sizes and add all four keys:

```python
    execution = snapshot.get("execution") or {}
    yes = execution.get("yes") or {}
    bid_sz = float(yes.get("bid_size") or 0.0)
    ask_sz = float(yes.get("ask_size") or 0.0)
    denom = bid_sz + ask_sz
    result["book_imbalance"] = (bid_sz - ask_sz) / denom if denom > 0.0 else 0.0
    result["trade_flow"] = 0.0      # Task 2
    result["spot_drift"] = 0.0      # Task 2
    result["news_forecast"] = 0.0   # Task 2
```
(where `result` is the dict `extract_features` returns — adapt to its actual variable name.)

- [ ] **Step 5: Add L2 to `OnlineLogit.update`; migration helper**

L2 on the feature weights only (not the bias, the last `grads` element `1.0`):

```python
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
```

Migration — reconcile a saved `full` model to the current `FULL_FEATURES`:

```python
def reconcile_full_model(blob):
    """Load a saved 'full' model into the current FULL_FEATURES, preserving the
    weights of features it already had and zero-initializing any new ones."""
    old = OnlineLogit.from_json(blob)
    if tuple(old.features) == FULL_FEATURES:
        return old
    fresh = OnlineLogit(FULL_FEATURES, lr=old.lr)
    old_index = {f: i for i, f in enumerate(old.features)}
    for i, f in enumerate(FULL_FEATURES):
        if f in old_index:
            fresh.w[i] = old.w[old_index[f]]
            fresh.g2[i] = old.g2[old_index[f]]
            # carry the standardizer stats too, if present
    fresh.w[-1] = old.w[-1]      # bias
    fresh.g2[-1] = old.g2[-1]
    return fresh
```

Use `reconcile_full_model(state["full"])` in `observe_cycle` where it currently does `OnlineLogit.from_json(state["full"])`, and pass `l2=L2` (add `L2 = 1e-3` as a module constant) in the `full.update(...)` call.

- [ ] **Step 6: Run — confirm pass**

Run: `python3 -m unittest openmarketterminal-qt.tests.test_spot_calibrator -v` → all pass, existing tests included.

- [ ] **Step 7: Commit**

```bash
git add openmarketterminal-qt/scripts/kalshi_advise/spot_calibrator.py openmarketterminal-qt/tests/test_spot_calibrator.py
git commit -m "feat(calibrator): ensemble feature scaffolding — book_imbalance, L2, state migration"
```

---

### Task 2: The auxiliary-source features — `trade_flow`, `spot_drift`, `news_forecast`

**Files:**
- Modify: `openmarketterminal-qt/scripts/kalshi_advise/spot_calibrator.py`
- Test: `openmarketterminal-qt/tests/test_spot_calibrator.py`

**Interfaces:**
- Consumes: `kalshi-trades.jsonl` (`{asset_id, side, size, ts}`), a spot lookback, `btc-intelligence-latest.json`.
- Produces: real values for `trade_flow`, `spot_drift`, `news_forecast`, computed as-of the snapshot decision time; each pure and unit-testable via an injected reader so no disk is needed in tests.

- [ ] **Step 1: Write the failing tests**

```python
def test_trade_flow_signed_taker_volume(self):
    # Trades on this ticker's asset within the window, signed by side, normalized.
    trades = [
        {"asset_id": "KXBTCD-X:yes", "side": "yes", "size": 300, "ts": 1000},
        {"asset_id": "KXBTCD-X:yes", "side": "no",  "size": 100, "ts": 1005},
        {"asset_id": "OTHER:yes",     "side": "yes", "size": 999, "ts": 1006},  # other market, ignored
        {"asset_id": "KXBTCD-X:yes", "side": "yes", "size": 200, "ts": 9999},   # future, ignored
    ]
    tf = spot_calibrator.trade_flow_feature("KXBTCD-X", trades, now_ms=2000, window_ms=5000)
    self.assertGreater(tf, 0.0)   # net buy pressure (300 yes - 100 no = +200 in-window)

def test_trade_flow_no_look_ahead(self):
    trades = [{"asset_id": "KXBTCD-X:yes", "side": "yes", "size": 500, "ts": 5000}]
    self.assertEqual(spot_calibrator.trade_flow_feature("KXBTCD-X", trades, now_ms=1000, window_ms=5000), 0.0)

def test_spot_drift_normalized_recent_move(self):
    # Positive drift when spot rose over the lookback, scaled by ambient vol.
    drift = spot_calibrator.spot_drift_feature(spot_now=100_100.0, spot_prev=100_000.0, per_min_vol_bps=8.0)
    self.assertGreater(drift, 0.0)

def test_news_forecast_neutral_when_absent(self):
    self.assertEqual(spot_calibrator.news_forecast_feature(None), 0.0)

def test_news_forecast_directional_read(self):
    intel = {"forecast": {"p_up": 0.7}}   # adapt to the real btc-intelligence shape you find
    self.assertGreater(spot_calibrator.news_forecast_feature(intel), 0.0)
```

- [ ] **Step 2: Run — confirm fail** (functions undefined).

- [ ] **Step 3: Implement the three pure feature functions**

- `trade_flow_feature(ticker, trades, now_ms, window_ms=...)`: sum `+size` for `side=="yes"` and `-size` for `side=="no"` over rows whose `asset_id` starts with `ticker` and `ts` in `(now_ms - window_ms, now_ms]`; normalize (e.g. `net / (abs sum + 1)`) to a bounded value; `0.0` if none. **First inspect a real `btc-intelligence-latest.json` and a real `kalshi-trades.jsonl` row** to confirm the exact `side` vocabulary (`yes`/`no` vs `buy`/`sell`) and the news field names, and match them.
- `spot_drift_feature(spot_now, spot_prev, per_min_vol_bps)`: `(spot_now - spot_prev)/spot_prev * 10000.0` normalized by `per_min_vol_bps` (guard vol>0); `0.0` on missing. `spot_prev` is the spot a short lookback (e.g. a few minutes) before the snapshot — the 15-min→hourly directional read.
- `news_forecast_feature(intel)`: map the intelligence forecast's directional read to a centered value (e.g. `2*p_up - 1`); `0.0` when `intel` is None/unreadable.

- [ ] **Step 4: Wire them into `extract_features` / `observe_cycle`**

Read the auxiliary sources **once per cycle** in `observe_cycle` (as-of `now_ms`): load `btc-intelligence-latest.json`, read the `kalshi-trades.jsonl` tail, and the spot lookback; pass them into the per-ticker feature computation (replace the Task-1 `0.0` stubs). Keep reads guarded (neutral on failure). Preserve no-look-ahead: only rows with `ts ≤ now_ms`.

- [ ] **Step 5: Run — confirm pass**; then a smoke run of the calibrator against the live evidence dir prints a `calibrator.json` whose predictions now carry nonzero new features.

- [ ] **Step 6: Commit**

```bash
git commit -m "feat(calibrator): trade_flow, spot_drift, news_forecast features (as-of, neutral-on-missing)"
```

---

### Task 3: Per-signal ablation + walk-forward beats-market report

**Files:**
- Modify: `openmarketterminal-qt/scripts/kalshi_advise/spot_calibrator.py` (report an ablation block) **or** add `openmarketterminal-qt/scripts/research/ensemble_ablation.py` (read-only, over the retained record)
- Test: `openmarketterminal-qt/tests/test_spot_calibrator.py`

**Interfaces:**
- Produces: a per-feature out-of-sample Brier delta (full model vs the same model with that feature held at neutral), and an overall "beats market" verdict, over the resolved record — the honest headline.

- [ ] **Step 1: Write the failing test**

```python
def test_ablation_reports_a_delta_per_feature(self):
    # Given a small resolved fixture, ablation returns one Brier delta per
    # ensemble feature vs the market baseline (sign not asserted — real data
    # decides; only that a real number is produced per feature).
    report = spot_calibrator.ablation_report(fixture_resolved_observations())
    for f in spot_calibrator.ENSEMBLE_FEATURES:
        self.assertIn(f, report)
        self.assertIsInstance(report[f]["brier_delta_vs_full"], float)

def test_ablation_positive_when_feature_is_constructed_to_help(self):
    # A fixture where a feature perfectly separates the outcome must show the
    # full model beating the ablated one (delta < 0 = full has lower Brier).
    report = spot_calibrator.ablation_report(fixture_where("spot_drift"))
    self.assertLess(report["spot_drift"]["brier_delta_vs_full"], 0.0)
```

- [ ] **Step 2: Run — confirm fail.**

- [ ] **Step 3: Implement `ablation_report`** — walk-forward over the resolved observations (predict-before-train, no look-ahead), computing the full model's Brier and, for each ensemble feature, the Brier of an otherwise-identical model trained with that feature pinned to neutral; report `brier_delta_vs_full` per feature and the overall `beats_market` (full Brier < market-mid Brier). Surface it in `calibrator.json` under a new `ablation` block (additive — does not change existing fields).

- [ ] **Step 4: Run — confirm pass.**

- [ ] **Step 5: Honest smoke run** — run the ablation over the real retained record and record the verdict in the report (it may honestly say the ensemble does not yet beat the market — that is a real answer, and the gate keeps it untrusted, so it costs nothing).

- [ ] **Step 6: Commit**

```bash
git commit -m "feat(calibrator): per-signal ablation + beats-market verdict in the report"
```
