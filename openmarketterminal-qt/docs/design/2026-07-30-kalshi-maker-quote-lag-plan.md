# Kalshi Maker Quote-Lag Engine — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure honestly whether resting a quote to capture the Kalshi book-lag is a real edge, with a fill model bracketed between defensible optimistic/pessimistic bounds and the strategy-grid referee — plus the retention that gives the sweep a real window.

**Architecture:** A pure, unit-tested bracketed fill model at the core; a read-only sweep (`scripts/research/maker_quote_lag.py`, reusing `kalshi_edge_common` + `q1_quote_lag`) that detects σ-events, rests a maker quote, fills it via the model from real trade prints, exits as a taker, and scores under both bounds; then a retention extension so trade prints accrue. **Build order is fill-model → sweep → stats → retention:** the fill model and sweep are testable now (synthetic + ~2.8h of live trades); the retention change only needs to *land* to start accruing, and the verdict waits for accrual regardless.

**Tech Stack:** Python 3 stdlib; `unittest`.

## Global Constraints

- **Read-only research only. NO live maker order placement, ever** (that is far-future, human-armed, out of scope). **No fill-faking:** a fill exists only when retained trade prints say a real trade went through our price on the taking side; queue position is bracketed, never assumed away.
- **Python 3 stdlib only.**
- **Certification rule:** an edge is "real" only if positive under the **pessimistic** fill bound AND clears Benjamini–Hochberg AND persists walk-forward AND has `effective_n ≥ 30`. Optimistic-only edges are flagged candidates, never certified.
- **Two baselines, never zero:** doing nothing, and the **taker** version of the same capture (proving "maker beats taker").
- **Non-fill is a real outcome** — counted, never a free win.
- Pre-registered literals: σ `{2.0, 3.0}`; horizons `{5,15,30,60}s`; rest-offsets `{touch, +1¢ tick}`; event cooldown 300s; BH α=0.05; effective-n floor 30. Fees from `kalshi_edge_common.fee_per_contract` on both legs (maker saves the half-spread, not the fee).
- **Paths:** research scripts at repo-root `scripts/research/`; the compactor at `openmarketterminal-qt/scripts/kalshi_lag_series.py`. Tests at `openmarketterminal-qt/tests/`, reaching root scripts via `../../scripts/research` (same as `test_kalshi_lag_series.py`).

## File Structure

- `scripts/research/maker_quote_lag.py` (repo root) — **create**: fill model, sweep, stats, artifacts, `main`.
- `openmarketterminal-qt/tests/test_maker_quote_lag.py` — **create**: unit + smoke tests.
- `openmarketterminal-qt/scripts/kalshi_lag_series.py` — **modify** (Task 4): retain trade prints.
- `openmarketterminal-qt/tests/test_kalshi_lag_series.py` — **modify** (Task 4): retention test.

---

### Task 1: The bracketed fill model (pure heart)

**Files:**
- Create: `scripts/research/maker_quote_lag.py`
- Test: `openmarketterminal-qt/tests/test_maker_quote_lag.py`

**Interfaces:**
- Produces: `maker_fill(ahead_size: float, hits: list[tuple[int, float]]) -> dict` returning `{"optimistic": ts|None, "pessimistic": ts|None}`. `hits` is the sorted `[(ts_ms, size)]` of taker volume that would execute against our resting order (real trades through our price on the taking side) after we joined. Optimistic = the first hit's ts (front of queue). Pessimistic = the ts of the hit at which cumulative hit size first exceeds `ahead_size` (back of queue), else None.

- [ ] **Step 1: Write the failing test**

Create `openmarketterminal-qt/tests/test_maker_quote_lag.py`:

```python
import os, sys, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts", "research"))
import maker_quote_lag as mql  # noqa: E402

class FillModelTest(unittest.TestCase):
    def test_optimistic_is_the_first_through_trade(self):
        hits = [(1000, 5.0), (2000, 10.0)]
        self.assertEqual(mql.maker_fill(ahead_size=100.0, hits=hits)["optimistic"], 1000)

    def test_pessimistic_waits_for_ahead_size_to_clear(self):
        # ahead of us: 12 contracts. Hits 5,10,3 -> cumulative clears 12 at the 2nd hit.
        hits = [(1000, 5.0), (2000, 10.0), (3000, 3.0)]
        r = mql.maker_fill(ahead_size=12.0, hits=hits)
        self.assertEqual(r["optimistic"], 1000)
        self.assertEqual(r["pessimistic"], 2000)   # 5+10=15 > 12

    def test_never_fills_pessimistic_when_ahead_size_never_clears(self):
        hits = [(1000, 2.0), (2000, 1.0)]           # only 3 total, ahead is 50
        r = mql.maker_fill(ahead_size=50.0, hits=hits)
        self.assertEqual(r["optimistic"], 1000)
        self.assertIsNone(r["pessimistic"])

    def test_no_hits_fills_neither(self):
        r = mql.maker_fill(ahead_size=0.0, hits=[])
        self.assertIsNone(r["optimistic"])
        self.assertIsNone(r["pessimistic"])
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_maker_quote_lag -k FillModel -v`
Expected: FAIL — `ModuleNotFoundError` / no `maker_fill`.

- [ ] **Step 3: Write the module header + fill model**

Create `scripts/research/maker_quote_lag.py`:

```python
#!/usr/bin/env python3
"""Kalshi maker quote-lag engine (read-only). Measures whether RESTING a quote to
capture the book-lag after a >=sigma BTC move is a real edge, with a fill model that
BRACKETS queue position between optimistic (front) and pessimistic (back) bounds
computed from real trade prints. Certifies an edge only under the pessimistic bound.
NO live execution, ever. See docs/design/2026-07-30-kalshi-maker-quote-lag-design.md.

    python3 scripts/research/maker_quote_lag.py
"""
import bisect, collections, json, math, sys
import kalshi_edge_common as common
import q1_quote_lag as q1

SCHEMA_VERSION = 1


def maker_fill(ahead_size, hits):
    """Bracket the fill of a resting order against the taker volume that would
    execute it. `hits` = sorted [(ts_ms, size)] of trades through our price on the
    taking side, after we joined. Optimistic = first hit (front of queue).
    Pessimistic = the hit at which cumulative size first exceeds `ahead_size`
    (back of queue), else None. Non-fill is a real outcome, not a free win.
    """
    if not hits:
        return {"optimistic": None, "pessimistic": None}
    optimistic = hits[0][0]
    cumulative = 0.0
    pessimistic = None
    for ts_ms, size in hits:
        cumulative += size
        if cumulative > ahead_size:
            pessimistic = ts_ms
            break
    return {"optimistic": optimistic, "pessimistic": pessimistic}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_maker_quote_lag -k FillModel -v`
Expected: PASS (4 tests).

- [ ] **Step 5: Commit**

```bash
git add scripts/research/maker_quote_lag.py openmarketterminal-qt/tests/test_maker_quote_lag.py
git commit -m "feat(maker): bracketed maker fill model (optimistic/pessimistic queue bounds)"
```

---

### Task 2: Trade-print prep + the event→rest→exit sweep

**Files:**
- Modify: `scripts/research/maker_quote_lag.py`
- Test: `openmarketterminal-qt/tests/test_maker_quote_lag.py`

**Interfaces:**
- Consumes: `maker_fill` (Task 1); `q1.load_quote_series`, `q1.QuoteBook`, `q1.detect_events`, `common.load_brti`, `common.parse_ticker`, `common.derive_outcome`, `common.load_settlements`, `common.fee_per_contract`, `common.read_jsonl`.
- Produces:
  - `load_trades() -> dict[ticker, sorted[(ts_ms, bet_yes_price, size, taker_sold_yes)]]` from `kalshi-trade-events.jsonl` (fields: `yes_price_dollars`, `count_fp`, `taker_outcome_side`/`taker_side`, `ts_ms`). `taker_sold_yes` is True when the taker was selling YES (hitting a YES bid).
  - `hits_against(bid_side, rest_price, join_ts, trades_for_ticker) -> list[(ts_ms, size)]` — the taker volume that would execute a resting BUY of `bid_side` at `rest_price`, after `join_ts` (a taker selling `bid_side` at a price ≤ `rest_price`). Backward-safe: only trades with `ts_ms > join_ts`.
  - `simulate_event(event, book, trades, outcomes) -> list[dict]` — for the lagging side implied by the move sign, rest at each rest-offset, fill via `maker_fill` under both bounds using `ahead_size` = the top-of-book size on our side at `join_ts`, exit as a taker at each horizon, and return one record per (side, offset, horizon) with `pnl_optimistic`, `pnl_pessimistic`, `pnl_taker` (the taker-capture baseline), `filled_opt`, `filled_pess`, `won`, `cluster` (event id), `event_ts`.

- [ ] **Step 1: Write the failing test** (synthetic: one event, a trade that fills optimistically but not pessimistically)

```python
class HitsTest(unittest.TestCase):
    def test_hits_are_taker_sells_through_our_price_after_join(self):
        # resting a YES buy at 0.40; a taker sells YES at 0.39 (through) after join.
        trades = [(500, 0.42, 3.0, True),    # before join -> excluded
                  (1500, 0.39, 5.0, True),   # taker sold YES at 0.39 <= 0.40 -> hit
                  (1600, 0.41, 9.0, True),   # 0.41 > 0.40 -> not a hit
                  (1700, 0.38, 4.0, False)]  # taker BOUGHT yes -> not a hit
        hits = mql.hits_against("YES", 0.40, join_ts=1000, trades_for_ticker=trades)
        self.assertEqual(hits, [(1500, 5.0)])
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_maker_quote_lag -k Hits -v`
Expected: FAIL — no `hits_against`.

- [ ] **Step 3: Implement `load_trades`, `hits_against`, `simulate_event`**

Append to `maker_quote_lag.py`. `hits_against` filters `trades_for_ticker` to `ts_ms > join_ts`, taker selling `bid_side`, price (in `bid_side` terms) ≤ `rest_price`, returning `[(ts_ms, size)]`. `simulate_event` picks the lagging side from `event["move_bps"]` sign, computes `ahead_size` from the book's top-of-book size on that side at the event ts, calls `maker_fill`, and for a fill computes exit P&L at each horizon as a taker sell at the observed bid, net of `fee(rest_price)` (entry) + `fee(exit_bid)` (exit); `pnl_taker` is the same capture entered at the *ask* (crossing) for the baseline. Records `won` from `derive_outcome`/settlement, `cluster = event ts`.

*(Full body written by the implementer; the interfaces, the "hit = taker sell through our price, after join" rule, `ahead_size` = top-of-book size on our side at join, the three P&L fields, and the two-baseline definition above are the exact contract, matched by the Task-1 fill model and the Task-3 stats.)*

- [ ] **Step 4: Run the Hits test + full module**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_maker_quote_lag -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add scripts/research/maker_quote_lag.py openmarketterminal-qt/tests/test_maker_quote_lag.py
git commit -m "feat(maker): trade-print hits + event->rest->exit sweep under both fill bounds"
```

---

### Task 3: Stats, certification, artifacts + smoke

**Files:**
- Modify: `scripts/research/maker_quote_lag.py`
- Test: `openmarketterminal-qt/tests/test_maker_quote_lag.py`

**Interfaces:**
- Consumes: everything above; reuse the strategy-grid statistics *by re-implementing them here* (this module must stand alone / stdlib-only): `clustered_mean(values, clusters)`, `benjamini_hochberg(pvals, alpha)`, a walk-forward final-holdout delta, per the `strategy_grid.py` definitions.
- Produces: `run(evidence) -> dict` (schema_version, provenance, method, and per-cell results: for each (σ, side, offset, horizon), the mean `pnl_pessimistic`/`pnl_optimistic`/`pnl_taker`, fill rates, clustered effective-n + t + p on `pnl_pessimistic`, walk-forward, `certified` bool). `certify(cell, rejected)` applies the rule (positive pessimistic mean AND BH-rejected AND walk-forward sign holds AND effective_n≥30). `main()` writes `maker-quote-lag.json` atomically + prints a report.

- [ ] **Step 1: Write the failing test** (a cell certified only if positive under the pessimistic bound)

```python
class CertifyTest(unittest.TestCase):
    def test_optimistic_only_edge_is_not_certified(self):
        cell = {"mean_pnl_pessimistic": -0.002, "mean_pnl_optimistic": 0.03,
                "effective_n": 40, "walkforward_pessimistic": 0.01, "bh_rejected": True}
        self.assertFalse(mql.certify(cell))
    def test_positive_pessimistic_edge_certifies(self):
        cell = {"mean_pnl_pessimistic": 0.012, "mean_pnl_optimistic": 0.03,
                "effective_n": 40, "walkforward_pessimistic": 0.008, "bh_rejected": True}
        self.assertTrue(mql.certify(cell))
    def test_insufficient_sample_never_certifies(self):
        cell = {"mean_pnl_pessimistic": 0.05, "mean_pnl_optimistic": 0.06,
                "effective_n": 12, "walkforward_pessimistic": 0.04, "bh_rejected": True}
        self.assertFalse(mql.certify(cell))
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_maker_quote_lag -k Certify -v`
Expected: FAIL — no `certify`.

- [ ] **Step 3: Implement stats + `certify` + `run` + `main`**

Append the stats (mirroring `strategy_grid.py`'s `clustered_mean`/`benjamini_hochberg`/walk-forward), `certify(cell)` (the exact rule above), `run`, and `main` (atomic `maker-quote-lag.json` + report to stdout). Certification uses `pnl_pessimistic` only; the optimistic bound and both fill rates are reported for context, never for certification.

- [ ] **Step 4: Run the Certify test + full module**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_maker_quote_lag -v`
Expected: PASS.

- [ ] **Step 5: Smoke-run on real evidence (read-only)**

Run: `cd .. && python3 scripts/research/maker_quote_lag.py > /tmp/maker.out 2>&1; tail -5 /tmp/maker.out`
Expected: emits without error over the ~2.8h of retained trades; honest headline (likely "fee-eaten even as a maker on this window" or "insufficient sample"). Note the sample is tiny until Task 4's retention accrues.

- [ ] **Step 6: Commit**

```bash
git add scripts/research/maker_quote_lag.py openmarketterminal-qt/tests/test_maker_quote_lag.py
git commit -m "feat(maker): stats, pessimistic-bound certification, evidence artifacts"
```

---

### Task 4: Retention — keep trade prints in the lag series

**Files:**
- Modify: `openmarketterminal-qt/scripts/kalshi_lag_series.py`
- Test: `openmarketterminal-qt/tests/test_kalshi_lag_series.py`

**Interfaces:**
- Produces: a third retained stream `SOURCE_TRADES = "kalshi-trade-events.jsonl"` in the lag series, tagged `"source": SOURCE_TRADES`, for the same families/expiry window as the quotes; top-of-book `*_size_fp` fields confirmed retained in the quote rows.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_kalshi_lag_series.py`: seed a `kalshi-trade-events.jsonl` with an in-window `KXBTCD` trade and one past close; run `series.collect_trades(None)` (new); assert only the in-window trade is collected, tagged `source == SOURCE_TRADES`, carrying `yes_price_dollars`/`count_fp`/`taker_*`. Also assert a retained quote row still carries `yes_bid_size_fp`.

- [ ] **Step 2: Run to verify it fails**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_kalshi_lag_series -k trade -v`
Expected: FAIL — no `collect_trades`.

- [ ] **Step 3: Add the trades stream by mirroring the BRTI stream**

`grep -n "SOURCE_BRTI\|collect_brti\|brti_last_ts_ms\|RETAINED_TS_KEY" openmarketterminal-qt/scripts/kalshi_lag_series.py scripts/research/kalshi_edge_common.py` and mirror the BRTI stream for `SOURCE_TRADES` in every place it appears: the constant, a `collect_trades(after_ms)` (like `collect_brti`, filtered to the retained families/expiry via `parse_threshold_ticker`/`parse_15m_ticker` and `[MIN, MAX]_SECONDS_TO_CLOSE`, keeping the verbatim trade row + `source`), the state cursor (`trades_last_ts_ms`), `recovered_state`, the main compaction loop (collect + interleave into the day file), `header_row`'s `sources`, and `RETAINED_TS_KEY` in `kalshi_edge_common` (key `"ts_ms"`) so a reader can pull trades back. Keep the change surgical — do not weaken the quote/BRTI paths.

- [ ] **Step 4: Run the trade test + the full compactor module**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_kalshi_lag_series -v`
Expected: PASS (existing + new).

- [ ] **Step 5: Smoke — confirm trades now retained, then measure size impact**

Run the compactor once read-only; confirm `kalshi-lag-series/*.jsonl` now carries `source":"kalshi-trade-events.jsonl"` rows; note the added MB/day (trades are sparse, so small). No silent cap.

- [ ] **Step 6: Commit**

```bash
git add openmarketterminal-qt/scripts/kalshi_lag_series.py openmarketterminal-qt/tests/test_kalshi_lag_series.py
git commit -m "feat(lag-series): retain trade prints for the maker quote-lag engine"
```

---

## Self-Review

**Spec coverage:** bracketed fill model, pessimistic/optimistic bounds, non-fill counted (Task 1) ✓; trade-print hits + event→rest→exit sweep, two baselines incl. taker (Task 2) ✓; clustered effective-n + walk-forward + BH + pessimistic-bound certification + artifacts (Task 3) ✓; retention extension for trade prints + sizes (Task 4) ✓; read-only, stdlib-only, no live execution (constraints + all tasks) ✓. **Deferred (separate):** maker variants folded into the strategy grid; any live path.

**Placeholder scan:** Task 1 and the test bodies are fully literal; Tasks 2–4 give the exact contracts (the "hit" rule, `ahead_size` source, three P&L fields, the certification rule, the grep-and-mirror stream targets) with literal tests, while the larger function bodies and the intricate compactor mirror are described against named, grepped anchors rather than transcribed — flagged honestly.

**Type consistency:** `maker_fill` return keys (`optimistic`/`pessimistic`) match Task 1's test and Task 2's use; `hits_against` output `[(ts_ms, size)]` matches `maker_fill`'s `hits` input; the cell keys `certify` reads (`mean_pnl_pessimistic`, `effective_n`, `walkforward_pessimistic`, `bh_rejected`) match Task 3's `run` output; `SOURCE_TRADES` string matches between the compactor and `RETAINED_TS_KEY`.
