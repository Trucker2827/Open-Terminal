# Kalshi 15-minute top-of-book capture & retention — design

**Date:** 2026-07-29 · **Status:** approved design, pre-implementation ·
**Base branch:** `main` (`finn/kalshi-15m-topofbook-capture`)

## Why

The exit-strategy investigation (scratchpad `exit_vs_hold` / `cashout_timing` /
`path_exit_sim`, 2026-07-29) could not answer the operator's real question —
*what is the best exit rule for the contracts I actually trade?* — because the
retained quote series (`kalshi-lag-series/`, issue #170) holds **only KXBTCD
hourly-threshold and KXBTC band** top-of-book. The operator trades mostly
**KXBTC15M** (15-minute directional) contracts, and their top-of-book is captured
**nowhere**:

- `kalshi-tickers.jsonl` is written in `KalshiScreen.cpp` from `ws_ticker_event`,
  which only fires for markets the Kalshi screen is **subscribed to** — driven by
  the current UI selection/ladder (hourly + bands).
- The only place `KXBTC15M` appears is `kalshi-lifecycle.jsonl`, and those are
  market-metadata lifecycle events, **not** quotes.

So the fix is upstream of the compactor: the app must **continuously subscribe to
and record 15-minute top-of-book**, after which a small compactor change retains
it. Confirmed cross-analysis result this unblocks: the hourly instrument and the
15-minute instrument had **opposite** exit tilts, so the hourly answer cannot be
transported — 15-minute paths are required.

## Goal & non-goals

**Goal:** whenever the app runs, capture KXBTC15M top-of-book into the existing
ticker evidence sink, and retain it in the 30-day lag series, so `path_exit_sim`
can be re-run on real 15-minute paths after a few days of accrual.

**Non-goals:** no headless/24-7 recorder (accepted: capture only while the app
runs; gaps surface honestly as manifest holes); no new evidence file; no change to
any analysis script's outcome logic; no autonomous trading anything.

## Scope

`KXBTC15M` only (the operator's instrument), behind a **configurable series list**
(default `["KXBTC15M"]`) so ETH/SOL/etc. 15-minute series can be added later
without code changes.

## Architecture — two components

### 1. C++ capture controller (the real change)

A self-contained `Kalshi15mCaptureController`, owned by the Kalshi service and
active for the app's lifetime, independent of UI selection.

- **Discover.** On a ~30 s `QTimer`, call
  `KalshiRestClient::fetch_markets(status="open", series_ticker="KXBTC15M")`
  (paginated) to get the currently-open 15-minute tickers. (The
  `market_lifecycle_v2` stream is a possible latency optimisation and is
  explicitly **out of the MVP**.)
- **Reconcile — a pure function.**
  `desired_subscriptions(open_markets, now) -> set<ticker>` contains all the
  policy (which families, any close-time guard) and has **no Qt/WS dependency**,
  so it is unit-testable in isolation. The controller diffs it against the set it
  currently holds and issues `subscribe(new)` / `unsubscribe(rolled_off)` on the
  adapter.
- **Isolation from the UI subscription.** The controller tracks **only the tickers
  it added** and never unsubscribes anything else. The WS client keeps a single
  `subscribed_tickers_` set, so the controller's additions union safely with the
  UI/ladder subscription and its removals touch only its own tickers.
- **Record — no new code.** Subscribed 15-minute ticks arrive as `ws_ticker_event`
  and are appended to `kalshi-tickers.jsonl` by the **existing** handler. The sink
  is unchanged.

Testability boundary: the controller separates *policy* (pure `desired_*` +
diff, unit-tested) from *plumbing* (timer, REST call, adapter subscribe — thin,
integration-covered by the canary).

### 2. Python compactor change (`kalshi_lag_series.py`)

Small and surgical:

- Add 15-minute recognition **alongside** `parse_threshold_ticker` (do not weaken
  the threshold parser): parse the close instant from the ticker date segment,
  set `strike = None`, recognise the `KXBTC15M` family.
- In `collect_quotes`, retain a 15-minute row when
  `0 <= seconds_to_close <= 900` (its full ~15-minute life), tagged with its
  family and `strike: null`. On-change + `HEARTBEAT_MS` downsampling already
  applies per market and needs no change.
- Update `header_row`'s `quote_rule` string to state the added 15-minute
  population truthfully.
- Keep the threshold cross-check intact; add a **separate** 15-minute cross-check
  so the series and `kalshi_edge_common` continue to agree on close times.

Downstream analyses need **no change**: 15-minute contracts carry no strike, so
their outcomes resolve from **recorded settlements only** — which
`path_exit_sim`/`kalshi_edge_common` already prefer.

## Data flow

```
REST discover (30s) → controller reconciles subscriptions → WS `ticker` channel
→ existing ws_ticker_event sink → kalshi-tickers.jsonl
→ kalshi_lag_series compactor retains 15M → kalshi-lag-series/
→ path_exit_sim re-run on real 15-minute paths
```

## Testing

- **Pure reconcile function (C++ unit):** subscribe/unsubscribe deltas across a
  15-minute roll; dedup against a simulated UI subscription; empty/one-open edge
  cases; controller never emits an unsubscribe for a ticker it did not add.
- **Compactor (Python, extend `test_kalshi_lag_series.py`):** a KXBTC15M ticker
  parses to (close, `strike=None`); a 15-minute row inside `[0,900]s` is retained
  and one outside is dropped; the 15-minute close-time cross-check against
  `kalshi_edge_common` passes; threshold behaviour is unchanged.
- **Canary (integration):** run the app briefly; assert KXBTC15M rows appear in
  `kalshi-tickers.jsonl` and are retained into `kalshi-lag-series/`; **measure the
  size/rotation impact** (see risk below) and report it, do not assume it.

## Risks & open items

- **Feed volume & rotation (primary risk).** Adding 15-minute markets raises the
  `kalshi-tickers.jsonl` write rate, shortening its size-bounded raw retention and
  the compactor's `ROTATION_HORIZON_HOURS` safety margin. Mitigation: the canary
  **measures** rows/day and MB/day added; if the margin tightens, shorten the
  compactor's launchd interval (currently 15 min) accordingly. Quantify before
  claiming safe — no silent cap.
- **App-open-only coverage.** Accepted. Missing spans while the app is closed are
  recorded as manifest `gaps`, never closed over.
- **Discovery latency.** A 30 s poll can miss the first seconds of a fresh
  15-minute market. Acceptable for exit-rule research; the lifecycle-driven
  optimisation is deferred.
- **Subscription volume ceiling.** If open KXBTC15M markets ever exceed a sane
  bound, the controller caps the subscribed set and logs the cap (no silent
  truncation).

## Rollout

1. Compactor change + tests (independent, safe to land first; retains 15-minute
   rows the moment they appear).
2. C++ controller + unit tests.
3. Canary run; measure size/rotation; tune compactor interval if needed.
4. After ~3–7 days of accrual, re-run `path_exit_sim` on 15-minute paths.

## Amendments during implementation (2026-07-29)

Recorded so this spec matches the code that shipped (final whole-branch review):

- **Subscription reconcile is a full re-assert, not a delta.** The Architecture /
  isolation text above describes issuing `subscribe(new)` / `unsubscribe(rolled_off)`
  deltas. The shipped `reconcile_and_apply` instead **re-subscribes the full desired
  set every poll** and unsubscribes only `held \ desired`. Reason (decided during
  review): the app's WS keeps a **single, non-ref-counted** subscription set shared
  with the UI, so a delta-only approach could leave a silent capture gap if the UI
  unsubscribed a ticker the controller still wanted. The re-assert self-heals within
  one poll (≤30 s). The isolation invariant still holds — `held_` only ever contains
  the controller's own desired 15-minute tickers, so `held \ desired` can never
  unsubscribe a UI ticker. Note this re-assert is **not free at the wire level**:
  `KalshiWsClient::subscribe` re-sends a subscribe frame each call.
- **`desired_subscriptions` signature.** Implemented as
  `desired_subscriptions(markets, families, cap)` — no `now` and no close-time guard
  (the controller intentionally subscribes ALL open 15-minute markets; the compactor
  does the `[0,900]s` filtering). "Over-subscribe, under-retain," by design.
- **Single-family discovery limit.** `poll()` discovers via
  `fetch_markets(series_ticker = families_.first())`, so today only the first family
  is *discovered* even though `desired_subscriptions` filters against all of
  `families_`. Adding a second 15-minute series (ETH/SOL…) therefore DOES need a
  small code change (a per-family fetch loop) — the "without code changes" claim
  holds only for the retain/subscribe filter, not discovery. Acceptable for the
  KXBTC15M-only MVP; tracked for when a second series is wanted.
- **Canary must measure the whole subscribe surface, not just ticker rows.**
  `subscribe_market` requests the `orderbook_delta`, `ticker`, `trade`, and
  `market_lifecycle_v2` channels **and** triggers a per-ticker orderbook snapshot,
  for up to `cap_ = 200` markets. The primary-risk framing above (ticker write-rate
  only) understates the load. The canary should measure the added rows/day across
  `kalshi-tickers.jsonl`, `kalshi-book-events.jsonl`, and `kalshi-trade-events.jsonl`
  (and any orderbook/ladder log), plus the WS subscribe-frame traffic — then decide
  whether to narrow the subscribed channel set to `ticker` only if the extra
  channels prove costly and unused by capture.
