# Strategy Sandbox — design spec

**Date:** 2026-07-06
**Status:** draft for review
**Owner:** haydarevich
**Scope:** openterminalcli serve daemon + CLI (`openmarketterminal-qt`)

## 1. Purpose

Make trading strategies prove themselves on paper — with real fee models, real exits,
and honest scoring — before any of them can be considered for live capital.

The sandbox runs every strategy the terminal has (deterministic lanes first; LLM
signal sources deferred), simulates fills and exits against recorded market data,
scores each book net of fees, and reports which strategies meet an evidence bar.
Going live remains a separate, human-only ceremony through the existing gate chain.

### Non-goals

- No new order-execution path. The sandbox never calls `crypto_submit_order` or any
  live tool.
- No automatic promotion. The Live Eligibility Gate reports; it never arms.
- No LLM trade generation in this phase. LLMs read and write memos only.
- No new sidecar process. Everything runs inside the existing serve daemon and CLI.

## 2. Power boundaries (non-negotiable invariants)

| Actor | May | May never |
|---|---|---|
| serve daemon | run strategies, journal decisions, simulate fills, score books | place live orders, write `cli.*` settings |
| Paper Executor | consume journaled candidates, mutate paper tables | touch `ExchangeService` or any live tool |
| Risk Review (LLM) | read journals/leaderboard/health via `--json` CLI, write memo files | place trades, pause/arm/disarm anything, write settings |
| Human | arm live via GUI gates + `arm-bot` after reviewing eligibility | — |

These boundaries are enforced structurally: the sandbox components link only the
paper tables and the journal; there is no code path from any sandbox component to
`ExchangeService`, `crypto_submit_order`, or the settings-write tool. A test asserts
the Paper Executor translation unit has no such symbol dependency.

## 3. Prerequisites (P0 — from the 2026-07-06 audit; block everything below)

1. **Candidate dedup** — journal rows get a consumed marker; scalp jsonl decisions
   get a stable id (or move to the journal). No candidate may be acted on twice.
2. **Log rotation + tail-read** — `scalp_decisions.jsonl` / `scalp_ticks.jsonl`
   rotated by size; readers stop `readAll()`ing whole files (762 MB / 1.7 GB peak
   measured).
3. **Profile correctness** — automation/sandbox state paths resolve the `--profile`
   argument before use (today all state lands in the default profile).
4. **`automation stop` disarms** the live guard and `status` shows it.
5. **Spot edge units** — `edge_after_cost` is a probability edge; the spot candidate
   query must stop treating it as price bps, and must filter `horizon` so 15 s
   scalp-gate rows cannot feed longer-horizon books.

## 4. Components

### 4.1 Strategy Registry

New table `sandbox_strategy` (migration):

```
strategy_id   TEXT PRIMARY KEY   -- sha256 of (kind, symbols, params_json), truncated
kind          TEXT NOT NULL      -- 'scalp' | 'spot' | 'btc5m' | 'kalshi' | 'long_short'
symbols       TEXT NOT NULL      -- CSV, normalized (BTC-USD)
params_json   TEXT NOT NULL      -- full parameter set, canonical key order
status        TEXT NOT NULL      -- 'active' | 'paused' | 'retired'
created_at    INTEGER NOT NULL   -- ms epoch
notes         TEXT
```

**Honesty rule:** `strategy_id` is a hash of the params. Any parameter change mints
a new id and an empty book. History is immutable; there is no way to tune a strategy
and keep its track record.

Season 1 registry entries: `scalp` (as configured today), `spot` (post-fix),
`btc5m`, `kalshi` prediction lane, `long_short` (journal-only, `hypothetical`).

### 4.2 Decision journaling (existing, extended)

All lanes continue writing to `edge_decision_journal`. Additions:

- `strategy_id TEXT` column (nullable for legacy rows) + index `(strategy_id, created_at)`.
- `consumed_at INTEGER` column — set by the Paper Executor when it acts on a row.
- `data_quality TEXT` — `'ok'` or `'degraded'` (see §6 stale-feed rule), set at
  decision time by the producer from `freshest_age_ms` / `live_sources`.

### 4.3 Paper Executor

A daemon job (`sandbox-paper-executor`, interval 30 s) that:

1. Selects unconsumed (`consumed_at IS NULL`), fresh, gate-passing journal rows for
   `active` strategies; marks each `consumed_at` in the same transaction it acts.
2. Opens a position in `sandbox_position`:

```
position_id     TEXT PRIMARY KEY
strategy_id     TEXT NOT NULL
decision_id     TEXT NOT NULL UNIQUE   -- journal row; UNIQUE = structural dedup
symbol          TEXT NOT NULL
side            TEXT NOT NULL          -- 'buy' (spot), 'long'/'short' (long_short), 'yes'/'no' (prediction)
hypothetical    INTEGER NOT NULL DEFAULT 0  -- 1 = modeled instrument, excluded from eligibility
qty             REAL NOT NULL
limit_price     REAL NOT NULL
target_price    REAL                   -- from strategy params at open time
stop_price      REAL
expires_at      INTEGER NOT NULL       -- horizon expiry, ms epoch
state           TEXT NOT NULL          -- 'pending_fill' | 'open' | 'closed' | 'unfilled'
opened_at       INTEGER
closed_at       INTEGER
entry_fee       REAL
exit_fee        REAL
realized_pnl    REAL                   -- net of all costs; NULL until closed
close_reason    TEXT                   -- 'target' | 'stop' | 'expiry' | 'unfilled' | 'resolved'
```

3. **Fill model (conservative by construction):** a post-only limit buy fills only
   if recorded ticks show trade prices at or below the limit within the entry window
   (default: the decision's `max_age` horizon). If tick coverage is missing or
   ambiguous, the position closes `unfilled`. Never assume a favorable fill.
4. **Exit handling:** every open position carries target, stop, and expiry from the
   moment it opens. Each cycle the executor evaluates tick lows/highs since the last
   check; the first of stop / target / expiry to be touched closes the position at
   that price (stop and target fills modeled as taker: taker fee + slippage bps from
   the venue fee profile; expiry closes at last trade price as taker). If both stop
   and target were touched within one interval and ordering is unknown, **the stop
   wins** (conservative).
5. Every state transition appends to `sandbox_fill` (position_id, ts, kind, price,
   fee, note) for replayability.

Sizing: fixed notional per book from strategy params (default $50), so books are
comparable and PnL is interpretable.

### 4.4 Outcome Resolver

For prediction books (`btc5m`, `kalshi`): after resolution time, realized PnL =
payout − entry cost − fees, written to the position and back to the journal row's
`outcome`. Reuses the existing proof-loop/resolution collectors where they exist;
consolidates their output into `sandbox_position` so the Scorer has one source.

`long_short` rows are resolved the same way against recorded prices but positions
are flagged `hypothetical = 1` (no execution router exists; leverage/liquidation is
modeled, not simulated).

### 4.5 Scorer + Strategy Leaderboard

Nightly daemon job (`sandbox-scorer`) aggregates per `strategy_id` into
`sandbox_score` (one row per strategy per UTC day):

- resolved_count, open_count, unfilled_count (fill rate)
- net_pnl (sum of realized_pnl — already net of fees/slippage/spread)
- hit_rate, avg_win, avg_loss
- max_drawdown (peak-to-trough of the cumulative realized PnL curve)
- degraded_count (decisions tagged `data_quality='degraded'` — tracked, excluded)

CLI surface:

```
openterminalcli sandbox leaderboard [--season N] [--json]
openterminalcli sandbox book <strategy_id> [--json]
openterminalcli sandbox eligibility [--json]
openterminalcli sandbox positions [--open|--closed] [--json]
```

Leaderboard display rules: books below the minimum sample (30 resolved) are listed
under "insufficient sample", never ranked; `hypothetical` books are sectioned
separately; a flat book ("no demonstrated edge") is a valid, honest result.

### 4.6 Risk Review

A scheduled daily Claude session (cron/schedule outside the daemon), strictly
read-only. It runs `sandbox leaderboard/eligibility/positions --json`, `daemon
audit --json`, and journal stats, then writes
`docs/risk-review/YYYY-MM-DD.md` covering: anomalies (PnL spikes, fill-rate
collapse), data-integrity flags (degraded decision share, stale sources, journal
growth), contradiction checks between lanes, and gate/arm states. It recommends
actions; it performs none.

### 4.7 Live Eligibility Gate

`sandbox eligibility` computes, per non-hypothetical strategy:

| Criterion | Bar (season 1 defaults) |
|---|---|
| Active duration | ≥ 28 days |
| Resolved sample | ≥ 30 positions |
| Net-of-fee PnL | > 0 |
| Max drawdown | ≤ 10% of cumulative gross notional traded |
| Data integrity | 0 unresolved flags; degraded share < 10% |

Output is a report. Arming remains: human opens GUI Security gates + runs
`automation arm-bot` with small `--max-order-usd`. The eligibility code has no
write path to any arm or setting. Bars are constants in one header so changing
them is a reviewed code change, not a runtime knob.

## 5. Data flow summary

```
lanes (scalp/spot/btc5m/kalshi/long-short)
      │  journal decisions (strategy_id, data_quality)
      ▼
edge_decision_journal ──(unconsumed, fresh, gate=pass)──▶ Paper Executor
                                                            │ fills/exits vs recorded ticks
                                                            ▼
                                              sandbox_position / sandbox_fill
                                                            │
                          Outcome Resolver (prediction books)│
                                                            ▼
                                                     sandbox_score  ◀── nightly Scorer
                                                            │
                    ┌───────────────────────────────────────┤
                    ▼                                       ▼
            sandbox leaderboard/eligibility          Risk Review memo (LLM, read-only)
                    │
                    ▼
            human decision → existing GUI gates + arm-bot (outside sandbox)
```

## 6. Stale-feed rule

A decision produced while `freshest_age_ms` exceeded the lane's threshold or
`live_sources < 2` is tagged `data_quality='degraded'` at write time. Degraded
decisions are journaled and counted but: the Paper Executor still simulates them
(so we learn what bad-data trades would have done), the Scorer reports them
separately, and the Eligibility Gate excludes them entirely. A strategy cannot
qualify for live on data the system itself distrusts.

## 7. Error handling

- Executor cycles are transactional per position; a crash mid-cycle can lose at
  most one un-actioned candidate (it stays unconsumed) — never a double-open
  (`decision_id UNIQUE`).
- Missing tick coverage → position `unfilled` / expiry at last known price with a
  `data gap` note; never interpolate favorable prices.
- Scorer failures leave prior scores intact (write-then-swap per day row).
- All sandbox tables live in the profile's existing SQLite DB → same
  migration/backup path as the journal.

## 8. Testing

Per repo discipline, every behavior ships with a failing-without-it test, wired
into ctest:

- **Executor fill model:** synthetic tick fixtures — fills at/through limit, no-fill
  when price never touches, ambiguous stop+target interval resolves to stop, expiry
  close, unfilled expiry.
- **Dedup:** same decision row can never open two positions (UNIQUE + consumed_at).
- **Scorer math:** fixture book with hand-computed net PnL, hit rate, max drawdown.
- **Eligibility boundaries:** 29 vs 30 samples, 27 vs 28 days, PnL exactly 0,
  drawdown at the cap, degraded share at 10%.
- **Registry honesty:** param change ⇒ new strategy_id; old book untouched.
- **Power boundary:** link-level test that the executor/scorer objects have no
  dependency on `ExchangeService` / live tool symbols.
- e2e smoke: sandbox runs against a recorded tick fixture end-to-end
  (decision → fill → exit → score → leaderboard).

## 9. Build order

- **P0** — audit prerequisites (§3). No sandbox code lands before these.
- **P1** — Strategy Registry + journal columns + Paper Executor with exits
  (+ migrations, unit tests).
- **P2** — Outcome Resolver consolidation + Scorer + `sandbox` CLI
  (leaderboard/book/positions/eligibility) + e2e smoke.
- **P3** — Risk Review daily memo (scheduled, read-only).
- **P4 (deferred, separate spec)** — LLM-derived *inputs* (news classification,
  event tagging) as features for deterministic lanes; LLM persona books only after
  the deterministic books have validated the executor, resolver, and scoring math
  for a full season.

## 10. Open questions (deliberately deferred, not blockers)

- Tick storage for the fill model: reuse `scalp_ticks.jsonl` post-rotation, or a
  compact SQLite tick cache (decide in the P1 plan by measuring coverage).
- Whether `sandbox_score` should also snapshot open-position mark-to-market
  (season 1: realized-only keeps the math simple and honest).
