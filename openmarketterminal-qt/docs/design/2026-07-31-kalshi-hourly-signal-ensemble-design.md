# Kalshi hourly signal ensemble — the entry brain — design

**Date:** 2026-07-31 · **Status:** approved design, pre-implementation ·
**Branch:** `finn/kalshi-hourly-signal-ensemble` (off `main`)

## Why

The bot sits idle on `SIGNAL_UNTRUSTED` because its calibrator is one dull tool:
an online logistic model over ~5 **physics** features (spot-vs-strike distance,
ambient vol, time), which honestly does not beat the market mid. A bot cannot
*predict* BTC — nobody can — so its edge, if any, comes from **aggregating many
signals a human can't hold at once**, from **speed**, and from **sizing/
discipline**, not from a crystal ball. This builds the aggregation half: a
richer hourly (`KXBTCD`) forecast that blends the physics signal with signals
already being collected but unused — orderbook imbalance, trade flow, a
cross-timeframe 15-minute read, and the existing news/intelligence forecast.

This is the **entry brain, foundation phase**. Two things are deliberately
deferred: the LLM **event-impact** signal ("cold-wallet hack → down, decaying
over hours") — the news *comprehension* upgrade — and signal-driven **hold/cut**
position management. Both build on this.

## Goals & non-goals

**Goal:** extend the existing calibrator's online logit with four new signal
features, under the exact same discipline, so `p_yes_full` is a genuinely richer
forecast — and measure honestly whether it beats the market.

**Non-goals / NEVER:**
- **Never lower the beats-the-market gate.** The forecast is "trusted" (and the
  bot bids) only when its Brier beats the market mid **out-of-sample**. Adding
  signals must not relax this. *This is the safety net: the worst case of a
  useless signal is the forecast stays untrusted → the bot stays idle → nothing
  is lost. Signals can only help, never make the bot bet noise.*
- **No look-ahead.** Every signal is read **as-of the snapshot time**, never a
  future value; the model predicts before it trains on the outcome (unchanged).
- **No fake edge.** A signal enters the model only if it improves out-of-sample
  Brier (per-signal ablation). Intuition ("a hack is bearish") is a hypothesis,
  not an edge — the record decides.
- **No C++ / report-format change.** `calibrator.json` keeps emitting
  `p_yes_full` + the market baseline, so the bot, gate, and cockpit are
  untouched. This is a Python-only signal-quality upgrade behind the same
  interface.
- Not the LLM event-impact signal, not hold/cut, not the 15-minute brain (all
  later phases).

## The design

### Where it lives

`openmarketterminal-qt/scripts/kalshi_advise/spot_calibrator.py` — extend
`FULL_FEATURES` and the feature extraction; the `OnlineLogit` class already
standardizes and learns arbitrary features, and the no-look-ahead scoring loop,
Brier-vs-market accounting, and `calibrator.json` emission are unchanged.

### The four new features (each already on the evidence bus)

| Feature | Source | Definition (as-of snapshot time) |
|---|---|---|
| **`book_imbalance`** | the snapshot's own book (`extract_book` already reads it) | `(yes_bid_size - yes_ask_size) / (yes_bid_size + yes_ask_size)` — resting-size pressure the calibrator currently reads but never models |
| **`trade_flow`** | `kalshi-trades.jsonl` | signed taker volume over a short trailing window on this market, normalized (recent buy vs sell pressure) |
| **`spot_drift`** | spot history in the snapshot / a short lookback | the **cross-timeframe / 15-minute read**: short-horizon spot move (e.g. last few minutes) normalized by ambient vol — near-term directional pressure. *Starts as spot drift for robustness; the 15-minute market's own signal is a documented upgrade.* |
| **`news_forecast`** | `btc-intelligence-latest.json` | the existing intelligence forecast's directional read, as-of the snapshot (a ready-made calibrated number — its abstention/weights already learned). The **LLM event-impact** upgrade replaces/augments this later. |

Each is standardized by the existing `OnlineLogit._standardize` (online mean/
std), so scale differences are handled and a missing signal maps to its mean
(neutral), never a fabricated extreme.

### The cross-timeframe link ("it's all connected")

`spot_drift` is the 15-minute → hourly direction: a near-term move against the
hourly thesis pulls `p_yes_full`. The reverse (hourly regime → 15-minute) is the
future 15-minute brain; both consume the same signal bus so they *can* couple.
Hold/cut is the next phase, built on this richer forecast.

### Discipline (unchanged engine, made explicit)

- **No-look-ahead:** signals joined by timestamp **≤ snapshot time**; predict
  before train (the existing pattern). Backtests over the retained jsonl files
  must respect the same bound.
- **Beats-the-market gate:** `adds_value_over_market` = full-model Brier <
  market-mid Brier, out-of-sample. Unchanged. This alone bounds the downside.
- **Per-signal ablation:** the calibrator reports each feature's out-of-sample
  contribution (Brier with vs without), so a signal that doesn't help is
  visible and can be dropped. Nothing rides along unmeasured.
- **L2 regularization** added to `OnlineLogit.update` so more features do not
  overfit into a fake edge.
- **Graceful degradation:** an unreadable/stale auxiliary source yields the
  neutral (mean) feature value and is logged — never a crash, never a
  fabricated signal.

## Testing

- **Per-feature ablation** — for each new feature, a walk-forward run shows its
  out-of-sample Brier delta vs the market baseline; the test asserts the
  ablation harness reports a real number (and pins the sign on a fixture where
  the feature is constructed to help).
- **No-look-ahead invariant** — a feature built from a future-only row does not
  change the as-of-snapshot prediction (a look-ahead leak would fail this).
- **Neutral-on-missing** — a snapshot with a missing auxiliary source scores as
  if that feature were at its mean (no crash, no extreme).
- **Report unchanged** — `calibrator.json` still carries `p_yes_full`,
  `market_yes_mid`, `adds_value_over_market`, and the per-contract predictions
  the bot/cockpit read; a golden check pins the schema.
- **Honest headline** — a real-data walk-forward reports whether the full
  ensemble beats the market baseline. The truthful outcome may be "it does not
  yet" — that is a real answer, and it costs nothing (the gate keeps it
  untrusted).

## Rollout / phases

1. **This build** — the four-signal ensemble in `spot_calibrator.py`, with
   ablation + L2 + the tests above. Ship behind the unchanged gate.
2. **Next — the LLM event-impact signal:** an LLM scores significant BTC events
   into {direction, magnitude, decay half-life} → a time-decaying `event_pressure`
   feature; earns its weight only by improving OOS Brier. (Your hack example is
   its first test case.)
3. **Later — hold/cut position management** driven by this forecast, and the
   **15-minute brain** (the reverse-direction coupling + the maker quote-lag
   fast path).
