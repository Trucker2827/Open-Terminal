# Kalshi event-impact news signal — the news comprehension upgrade — design

**Date:** 2026-08-01 · **Status:** approved design, pre-implementation ·
**Branch:** `finn/kalshi-event-impact-signal` (off `main`)

## Why

The hourly signal-ensemble (PR #194, shipped) gave the bot four new senses, one
of which is `news_forecast` — but that signal is **lexical and stateless**. BTC
news is scored by hardcoded bullish/bearish phrase lists (C++ `BtcNewsPulse`),
and the calibrator reads the result as a single scalar, `clamp(news_context.
score / 100, ±1)`, with **no memory of when an event happened or how fast it
fades**. So a cold-wallet exchange hack and a routine bullish headline collapse
into the same undifferentiated number.

That is the gap this phase closes. A hack that drains coins is rightly bearish
for hours — sharply at first, decaying as the market absorbs it. The bot should
*feel* that: a signal that reads the actual event, judges its **direction,
magnitude, and half-life**, and **decays in real time** toward neutral. This is
the news *comprehension* upgrade the ensemble design named as its next phase.

## Goals & non-goals

**Goal:** an LLM scores each significant BTC event into
`{direction, magnitude, half_life_hours}`; the calibrator consumes a new
**time-decaying `event_pressure` feature** that must earn its weight through the
same ablation gate as every other signal.

**Non-goals / NEVER:**
- **Never lower the beats-the-market gate.** The LLM's judgment is a
  *hypothesis*, not an edge. `event_pressure` enters the trusted forecast only
  if the ablation shows it improves out-of-sample Brier. Worst case of a useless
  signal: the forecast stays untrusted → the bot stays idle → nothing is lost.
  This is the safety net, unchanged.
- **No look-ahead.** The producer scores only stories with `published_ts ≤`
  scan time and stamps `as_of_ms` = scan time. The consumer drops any record
  with `as_of_ms > now_ms`, counts only events with `event_ts_ms ≤ now_ms`, and
  decays using only elapsed time. A leak here fabricates a fake edge that loses
  real money.
- **No fake edge / no forced signal.** The LLM abstains on noise (empty event
  list → neutral feature). Intuition ("a hack is bearish") is a hypothesis the
  record judges, never a hand-tuned weight.
- **The calibrator never calls the LLM.** It stays a pure, fast consumer; the
  LLM runs only in the hourly producer job, off the decision path.
- **No new news collection.** The producer reads stories already on the
  evidence bus (`btc-news-pulse-latest.json`); it adds a scorer, not a feed.
- Not a sub-hourly breaking-event scan (a documented later phase); not hold/cut;
  not the 15-minute brain.

## The design

### Architecture — one producer, one consumer, mirroring btc-intelligence

The existing BTC intelligence pipeline is a **C++ producer → JSON evidence file
→ Python consumer** split. This phase adds one of each, in that exact shape.

**Producer — a new hourly managed job + a thin C++ command.**

- A new managed job `btc-event-impact-hourly` in `ServeCommand.cpp`'s registry
  (interval `3600`, offset after the pulse writes, e.g. `240`), argv
  `{"news", "bitcoin-event-impact", "--limit", "60"}`, mirroring
  `btc-evidence-hourly-intelligence`.
- A new C++ subcommand `news bitcoin-event-impact` (alias) in
  `CommandDispatch.cpp` that:
  1. reads the stories in `btc-news-pulse-latest.json` (the same upstream input
     the intelligence engine consumes — `news_context`/`stories[]`),
  2. hands them as a **blind context JSON on stdin** to the Python scorer
     `python3 scripts/kalshi_advise/btc_event_impact.py score`, invoked via
     `QProcess` (the serve loop already runs `python3` scripts this way, e.g.
     `crypto_scalp_report.py`), with a bounded timeout,
  3. writes the scorer's stdout through the existing
     `bitcoin_evidence_write_snapshot("btc-event-impact", output)` helper →
     `btc-event-impact-latest.json` + `btc-event-impact.jsonl` in the evidence
     dir (`~/Library/Application Support/Open Terminal/Open Terminal/`).
  On scorer failure/timeout/non-zero exit, it writes **no** new record (or a
  well-formed empty-events record) and logs — never a partial/garbage snapshot.

**The Python scorer — `scripts/kalshi_advise/btc_event_impact.py`.**

Follows the `claude_forecaster.py` template exactly: provider `anthropic`, model
`claude-opus-4-8` (env override), structured `json_schema` output, effort knob,
no API key read or logged (SDK-resolved), `anthropic` imported lazily so
`identify` works without the SDK. CLI contract:
- `identify` → `{provider, model, prompt_version}` (frozen, version-pinned
  `PROMPT_VERSION = "event-impact-v1"`).
- `score` → reads the blind stories JSON on stdin, returns:
```json
{
  "as_of_ms": "<scan time epoch ms, string>",
  "events": [
    {
      "event_ts_ms": <int epoch ms, the story's published_ts>,
      "direction":   <float -1..1>,     // -1 bearish … +1 bullish
      "magnitude":   <float 0..1>,      // how big the price impact
      "half_life_hours": <float > 0>,   // how fast it fades
      "kind": "<short label, e.g. exchange-hack | etf-flow | regulation>",
      "headline": "<the story headline>",
      "rationale": "<one line>"
    }
  ],
  "model": "claude-opus-4-8",
  "prompt_version": "event-impact-v1"
}
```
The system prompt instructs: score **only** events likely to move BTC
materially; **abstain** (omit) on routine/ambiguous news; `event_ts_ms` is the
story's own `published_ts` and must be `≤ as_of_ms`; a hack/exploit/theft is
bearish with a multi-hour half-life; ETF inflows bullish; magnitude reflects
expected price impact, not headline drama.

Each scan re-emits **every event still materially live** (roughly, within a few
half-lives of `as_of_ms`), not only newly-discovered ones — so the freshest
`-latest.json` record is self-sufficient and the consumer never has to stitch or
dedup across historical records. Events decayed to negligibility simply drop
out of the next scan. (The `.jsonl` history is retained only for backtest
replay.)

**Consumer — a new decaying feature in `spot_calibrator.py`.**

Follows the windowed `spot_drift`/`trade_flow` template (per-row `as_of_ms`
against `now_ms`), *not* the stateless `news_forecast` one.

```
event_pressure(now_ms) =
    clamp( Σ over events e with e.event_ts_ms ≤ now_ms :
             e.direction · e.magnitude · 0.5 ** (Δt_hours / e.half_life_hours),
           -1.0, +1.0 )
    where Δt_hours = (now_ms − e.event_ts_ms) / 3_600_000
```

- `load_event_impact_latest(path, now_ms)` — reads `btc-event-impact-latest.json`;
  returns `None` if `int(as_of_ms) > now_ms` (no-look-ahead) or on any
  read/parse error (mirrors `load_intel_latest`). Registered in
  `load_auxiliary_sources`'s fan-in dict as `"event_impact"`.
- `event_pressure_feature(record, now_ms)` — computes the decayed sum from the
  freshest valid record; `0.0` on `None`/empty/malformed (neutral).
- Wired in `observe_cycle` alongside the other per-contract overrides:
  `features["event_pressure"] = event_pressure_value`.
- `ENSEMBLE_FEATURES += ("event_pressure",)` → `FULL_FEATURES` grows 10 → 11.
  `reconcile_full_model` already migrates on feature-count growth: it preserves
  the 10 learned weights + g2 + bias and zero-inits the new one; extend it to 11
  and add the migration test. The standardizer reset self-heals within a couple
  of cycles, far inside the 100-contract gate.
- The ablation (`ablation_report`) automatically A/Bs `event_pressure` because it
  walks one ablated model per `ENSEMBLE_FEATURE`; no ablation code changes
  beyond the feature list. The lexical `news_forecast` **stays** — the record
  decides whether one, both, or neither earns weight.

### Why decay lives in the consumer

Computing `0.5 ** (Δt / half_life)` at *decision* time (not baking a decayed
value into the file) is what makes it honest and no-look-ahead: the same stored
event yields a different, correct pressure at each `now_ms`, and a replay/
backtest over the retained jsonl reproduces exactly what the bot saw. This is
the identical discipline `spot_drift_feature` already follows.

## Discipline (unchanged engine, made explicit)

- **No-look-ahead:** producer scores `published_ts ≤ as_of_ms`; consumer guards
  `as_of_ms ≤ now_ms` and `event_ts_ms ≤ now_ms`; decay uses elapsed time only.
- **Beats-the-market gate:** `adds_value_over_market` (100-contract, out-of-
  sample) unchanged; the C++ bot/gate/cockpit read the same report fields and
  need no change.
- **Per-signal ablation:** `event_pressure` reported with its own OOS Brier
  delta; a signal that doesn't help is visible and droppable.
- **L2** (already in `OnlineLogit.update`) shrinks a noise weight so 11 features
  can't overfit into a fake edge.
- **Graceful degradation:** any producer failure or missing/stale/empty file →
  neutral `0.0` → the bot behaves exactly as it does today. The upgrade can only
  add signal, never subtract safety.

## Testing

**Producer scorer (`btc_event_impact.py`)** — LLM stubbed, no network:
- `identify` returns the frozen contract without the SDK installed.
- Given fixture pulse stories, `score` emits the documented schema; each event's
  `event_ts_ms ≤ as_of_ms`.
- Abstention: noise-only stories → `events: []` (neutral, not a fabricated
  score).
- Malformed/empty stdin → a well-formed empty-events record, non-zero never
  crashes the caller.

**Consumer (`spot_calibrator.py`)**:
- **Half-life decay** — one event at exactly one half-life ago contributes
  `direction·magnitude·0.5`; at two half-lives, `·0.25`; at `Δt=0`, full.
- **Summation + clamp** — two live events sum; opposing events partially cancel;
  the total clamps to `±1`.
- **No-look-ahead** — a record with `as_of_ms > now_ms` is ignored; an event
  with `event_ts_ms > now_ms` does not contribute (a look-ahead leak fails
  this).
- **Neutral-on-missing** — missing file / parse error / empty events → `0.0`.
- **Migration** — a saved 10-feature model reconciles to 11: the 10 physics/
  ensemble weights + g2 + bias are preserved, `event_pressure` zero-inits;
  end-to-end through `settle_cycle` so it persists.
- **Ablation** — the report carries a real `event_pressure`
  `brier_delta_vs_full`; the empty/insufficient record stays honest
  (`beats_market: false`, null deltas), respecting the `MIN_SCORED_CONTRACTS`
  floor.
- **Report additive** — existing keys (`p_yes_full`, `adds_value_over_market`,
  per-contract predictions) unchanged; a golden check pins the schema so the
  C++ bot/cockpit stay untouched.

Each test fails before the change and passes after.

## Rollout / phases

1. **This build** — the Python scorer, the C++ producer command + managed job,
   and the consumer `event_pressure` feature (migration, ablation, tests).
   Ships behind the unchanged gate; needs a serve-daemon rebuild + redeploy to
   activate the new job (same as prior C++ evidence changes). Honest headline
   expected at first: `event_pressure` delta null / `beats_market: false` — "no
   evidence yet," populating as events occur and contracts settle.
2. **Next — sub-hourly breaking-event scan:** a fast (~10 min) re-score when new
   high-impact stories arrive, so a hack registers in minutes, not up to an
   hour. Ties to the 15-minute-contract speed theme.
3. **Later — hold/cut position management** driven by this richer forecast, and
   the **15-minute brain** (reverse-direction coupling + maker quote-lag fast
   path).
