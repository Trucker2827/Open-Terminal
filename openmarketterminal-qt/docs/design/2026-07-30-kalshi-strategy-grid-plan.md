# Kalshi Paper Strategy-Grid (Analytical Core) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A read-only, reproducible sweep that replays a pre-registered grid of mechanical + signal-gated paper strategies over the retained Kalshi book + real settlements, and emits versioned, machine/CLI/bot-consumable verdict files with honest multiple-comparison discipline.

**Architecture:** One read-only script `scripts/research/strategy_grid.py` reusing the repo's `kalshi_edge_common` (loaders, fee, outcomes, validation) and `q1_quote_lag` (quote series, event detection). Pure, testable units for the grid, the per-contract taker simulation, the gates, and the statistics; a thin `main()` glues them and writes the evidence artifacts. This plan covers the analytical core only (spec rollout 1–4); the CLI verb and bot panel line are a separate follow-on plan.

**Tech Stack:** Python 3 stdlib only (matches `scripts/research/`); `unittest`.

## Global Constraints

- **Python 3 stdlib only** — no third-party imports (matches the existing research scripts).
- **Read-only** — open every evidence log with mode `r`; write only the output artifacts. Never import or call `spot_calibrator.run_once()`.
- **No look-ahead** — a gate/entry decision at instant `t` may use only data at or before `t`; the quote book is backward-looking (`bisect_right - 1`).
- **Taker fills only** — buy at the ask, sell (cash-out) at the bid; fee `common.fee_per_contract` on both legs. No maker variants.
- **Two nulls, never zero** — every variant is scored vs HOLD and vs the market mid.
- **Honesty metadata travels with every verdict** — each survivor record carries `effective_n`, `ci95`, `survives_correction`, walk-forward status; `-latest.json` is the compact consumer file.
- **Pre-registered literals** (fixed in code): `MIN_ENTRY_S_TO_CLOSE = 120`; entry bands `{(0.02,0.10),(0.10,0.25),(0.25,0.50),(0.50,0.75),(0.75,0.90),(0.90,1.01)}`; TP `{0.05,0.10,0.15,0.20,0.30}`; SL `{0.05,0.10,0.15,0.20}`; trail `{0.05,0.10,0.15}`; walk-forward = 5 expanding folds by close time; Benjamini–Hochberg α = 0.05; survivor floor `effective_n ≥ 30`; calibrator gate `|calibrated_p − mid| ≥ 0.03`; calibrator freshness 60 s.
- **Families:** `KXBTCD` (recorded + validated-derived outcomes) and `KXBTC15M` (recorded-only); exclude `KXBTC` band, counted.
- **Schema versioned** — output JSON carries `schema_version` (start at `1`).

Paths are relative to `openmarketterminal-qt/`. Run tests from `openmarketterminal-qt/`.

## File Structure

- `scripts/research/strategy_grid.py` — **create**: the whole sweep (grid, sim, gates, stats, artifacts, `main`).
- `tests/test_strategy_grid.py` — **create**: unit + end-to-end tests on a seeded temp evidence dir (mirror `test_kalshi_lag_series.py`'s `OPENTERMINAL_EVIDENCE_DIR` pattern).
- Reused, unchanged: `scripts/research/kalshi_edge_common.py`, `scripts/research/q1_quote_lag.py`.

---

### Task 1: The pre-registered grid + stable variant ids

**Files:**
- Create: `scripts/research/strategy_grid.py`
- Test: `tests/test_strategy_grid.py`

**Interfaces:**
- Produces: constants (`ENTRY_BANDS`, `TPS`, `SLS`, `TRAILS`, `GATES`, `SIDES`, `MIN_ENTRY_S_TO_CLOSE`); `@dataclass`-free plain-dict variant records; `build_grid() -> list[dict]` where each variant is `{"variant_id": str, "side": "YES"|"NO", "band": [lo,hi], "gate": str, "exit": {"kind": "hold"|"tp"|"sl"|"trail", "amount": float|None}}`; `variant_id(v) -> str` deterministic and stable.

- [ ] **Step 1: Write the failing test**

Create `tests/test_strategy_grid.py`:

```python
import os, sys, json, unittest, tempfile, datetime
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts", "research"))
import strategy_grid as sg  # noqa: E402

class GridTest(unittest.TestCase):
    def test_grid_is_pre_registered_and_ids_stable(self):
        g1 = sg.build_grid()
        g2 = sg.build_grid()
        ids = [v["variant_id"] for v in g1]
        self.assertEqual(len(ids), len(set(ids)))            # ids unique
        self.assertEqual([v["variant_id"] for v in g2], ids) # stable across calls
        # 2 sides * 6 bands * 3 gates * (1 hold + 5 tp + 4 sl + 3 trail) exits = 468
        self.assertEqual(len(g1), 2 * 6 * 3 * (1 + 5 + 4 + 3))
        v = g1[0]
        self.assertIn(v["side"], ("YES", "NO"))
        self.assertIn(v["gate"], ("mechanical", "physics", "calibrator"))

    def test_variant_id_encodes_the_tuple(self):
        v = {"side": "NO", "band": [0.10, 0.25], "gate": "physics",
             "exit": {"kind": "sl", "amount": 0.15}}
        vid = sg.variant_id(v)
        self.assertEqual(vid, "NO|b10-25|physics|sl15")
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_strategy_grid -k Grid -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'strategy_grid'`.

- [ ] **Step 3: Write the grid**

Create `scripts/research/strategy_grid.py` (module header + grid):

```python
#!/usr/bin/env python3
"""Kalshi paper strategy-grid: a reproducible sweep over a pre-registered grid of
mechanical + signal-gated paper strategies, marked to the retained real book and
settled on real outcomes net of fees. Read-only; emits versioned evidence artifacts.
See docs/design/2026-07-30-kalshi-strategy-grid-design.md.

    python3 scripts/research/strategy_grid.py
"""
import bisect, collections, json, math, sys
import kalshi_edge_common as common

SCHEMA_VERSION = 1
SIDES = ("YES", "NO")
ENTRY_BANDS = ((0.02, 0.10), (0.10, 0.25), (0.25, 0.50),
               (0.50, 0.75), (0.75, 0.90), (0.90, 1.01))
GATES = ("mechanical", "physics", "calibrator")
TPS = (0.05, 0.10, 0.15, 0.20, 0.30)
SLS = (0.05, 0.10, 0.15, 0.20)
TRAILS = (0.05, 0.10, 0.15)
MIN_ENTRY_S_TO_CLOSE = 120

def _band_tag(lo, hi):
    return "b%d-%d" % (round(lo * 100), round(min(hi, 1.0) * 100))

def _exit_tag(ex):
    return ex["kind"] if ex["amount"] is None else "%s%d" % (ex["kind"], round(ex["amount"] * 100))

def variant_id(v):
    return "|".join((v["side"], _band_tag(v["band"][0], v["band"][1]),
                     v["gate"], _exit_tag(v["exit"])))

def _exits():
    out = [{"kind": "hold", "amount": None}]
    out += [{"kind": "tp", "amount": a} for a in TPS]
    out += [{"kind": "sl", "amount": a} for a in SLS]
    out += [{"kind": "trail", "amount": a} for a in TRAILS]
    return out

def build_grid():
    grid = []
    for side in SIDES:
        for lo, hi in ENTRY_BANDS:
            for gate in GATES:
                for ex in _exits():
                    v = {"side": side, "band": [lo, hi], "gate": gate, "exit": ex}
                    v["variant_id"] = variant_id(v)
                    grid.append(v)
    return grid
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_strategy_grid -k Grid -v`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
git add openmarketterminal-qt/scripts/research/strategy_grid.py openmarketterminal-qt/tests/test_strategy_grid.py
git commit -m "feat(strategy-grid): pre-registered variant grid + stable ids"
```

---

### Task 2: Per-contract taker simulation (entry, exit rules, fees, settle)

**Files:**
- Modify: `scripts/research/strategy_grid.py`
- Test: `tests/test_strategy_grid.py`

**Interfaces:**
- Consumes: `build_grid` (Task 1); a per-contract quote path as sorted `[(ts_ms, bet_bid, bet_ask)]` already in **bet-side** terms, plus `won` (bool, bet-side outcome) and `close_ms`.
- Produces:
  - `bet_side_quotes(rows, side) -> list[(ts_ms, bet_bid, bet_ask)]` converting a YES `(ts,yes_bid,yes_ask)` path to bet-side prices (YES: as-is; NO: `1-yes_ask`, `1-yes_bid`).
  - `simulate_exit(path, i0, entry_ask, exit_rule, close_ms) -> (sell_bid|None, exit_ts|None)` — first tick after `i0`, before close, satisfying the rule (taker at bid); `None` if never.
  - `contract_pnl(path, i0, entry_ask, exit_rule, won) -> {"pnl","exited","hold_pnl"}` where `hold_pnl` is the settle-to-end P&L and `pnl` is the rule's P&L; both net of taker fees.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_strategy_grid.py`:

```python
class SimTest(unittest.TestCase):
    def _path(self):
        # bet-side (ts_ms, bid, ask); price rises 0.20 -> 0.45 then settles
        return [(1000, 0.19, 0.20), (2000, 0.30, 0.31), (3000, 0.44, 0.45)]

    def test_take_profit_sells_at_bid_net_of_fees(self):
        path = self._path()
        # entered at ask 0.20 at i0=0; TP +0.20 -> first bid >= 0.40 is 0.44 @ t=3000
        r = sg.contract_pnl(path, 0, 0.20, {"kind": "tp", "amount": 0.20}, won=False)
        self.assertTrue(r["exited"])
        expected = (0.44 - 0.20) - sg.common.fee_per_contract(0.20) - sg.common.fee_per_contract(0.44)
        self.assertAlmostEqual(r["pnl"], expected, places=6)

    def test_hold_uses_settlement_not_a_sale(self):
        path = self._path()
        r = sg.contract_pnl(path, 0, 0.20, {"kind": "hold", "amount": None}, won=True)
        self.assertFalse(r["exited"])
        self.assertAlmostEqual(r["pnl"], (1.0 - 0.20) - sg.common.fee_per_contract(0.20), places=6)

    def test_stop_loss_triggers_on_decline(self):
        path = [(1000, 0.49, 0.50), (2000, 0.33, 0.34), (3000, 0.20, 0.21)]
        # entry ask 0.50; SL -0.15 -> first bid <= 0.35 is 0.33 @ t=2000
        r = sg.contract_pnl(path, 0, 0.50, {"kind": "sl", "amount": 0.15}, won=False)
        self.assertTrue(r["exited"])
        expected = (0.33 - 0.50) - sg.common.fee_per_contract(0.50) - sg.common.fee_per_contract(0.33)
        self.assertAlmostEqual(r["pnl"], expected, places=6)

    def test_bet_side_quotes_flips_for_NO(self):
        yes = [(1000, 0.60, 0.62)]
        self.assertEqual(sg.bet_side_quotes(yes, "NO"), [(1000, 1 - 0.62, 1 - 0.60)])
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_strategy_grid -k Sim -v`
Expected: FAIL — `AttributeError: module 'strategy_grid' has no attribute 'contract_pnl'`.

- [ ] **Step 3: Write the simulation**

Append to `strategy_grid.py`:

```python
FEE = common.fee_per_contract

def bet_side_quotes(rows, side):
    """(ts, yes_bid, yes_ask) -> (ts, bet_bid, bet_ask). NO buys/sells the complement."""
    if side == "YES":
        return [(ts, b, a) for ts, b, a in rows]
    return [(ts, 1.0 - a, 1.0 - b) for ts, b, a in rows]   # NO: bid/ask flip & complement

def simulate_exit(path, i0, entry_ask, exit_rule, close_ms):
    """First (sell_bid, exit_ts) after i0 (before close) where the rule fires; taker at bid."""
    kind, amt = exit_rule["kind"], exit_rule["amount"]
    if kind == "hold":
        return None, None
    running_max = None
    for ts, bid, _ask in path[i0 + 1:]:
        if close_ms is not None and ts >= close_ms:
            break
        if kind == "tp" and bid >= entry_ask + amt:
            return bid, ts
        if kind == "sl" and bid <= entry_ask - amt:
            return bid, ts
        if kind == "trail":
            running_max = bid if running_max is None else max(running_max, bid)
            if bid <= running_max - amt:
                return bid, ts
    return None, None

def contract_pnl(path, i0, entry_ask, exit_rule, won, close_ms=None):
    hold_pnl = ((1.0 if won else 0.0) - entry_ask) - FEE(entry_ask)
    sell_bid, _ts = simulate_exit(path, i0, entry_ask, exit_rule, close_ms)
    if sell_bid is None:
        return {"pnl": hold_pnl, "exited": False, "hold_pnl": hold_pnl}
    pnl = (sell_bid - entry_ask) - FEE(entry_ask) - FEE(sell_bid)
    return {"pnl": pnl, "exited": True, "hold_pnl": hold_pnl}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_strategy_grid -k Sim -v`
Expected: PASS (4 tests).

- [ ] **Step 5: Commit**

```bash
git add openmarketterminal-qt/scripts/research/strategy_grid.py openmarketterminal-qt/tests/test_strategy_grid.py
git commit -m "feat(strategy-grid): taker per-contract simulation (entry/exit/fees/settle)"
```

---

### Task 3: Entry-band first-touch + signal gates (no look-ahead)

**Files:**
- Modify: `scripts/research/strategy_grid.py`
- Test: `tests/test_strategy_grid.py`

**Interfaces:**
- Consumes: bet-side path (Task 2); a `Signals` object exposing `physics_ok(ticker, ts_ms, side)` and `calibrator_ok(ticker, ts_ms, side)` (both backward-looking, `None` when un-evaluable).
- Produces: `find_entry(path, lo, hi, close_ms) -> (i0, entry_ask) | None` (first tick with `lo <= bet_ask < hi` and `>= MIN_ENTRY_S_TO_CLOSE` left); `gate_ok(gate, ticker, ts_ms, side, signals) -> bool | None` (`mechanical` → True; `physics`/`calibrator` delegate; `None` = un-gateable, counted not imputed).

- [ ] **Step 1: Write the failing test**

Add to `tests/test_strategy_grid.py`:

```python
class GateTest(unittest.TestCase):
    def test_find_entry_first_touch_with_room(self):
        # bet-side asks: 0.30 (too little time), 0.08 (in 2-10 band, room), 0.06
        close = 10_000_000
        path = [(close - 60_000, 0.29, 0.30),     # 60s left < 120s floor -> skip
                (close - 300_000, 0.07, 0.08),    # 300s left, ask 0.08 in [0.02,0.10)
                (close - 200_000, 0.05, 0.06)]
        path.sort()
        got = sg.find_entry(path, 0.02, 0.10, close)
        self.assertIsNotNone(got)
        i0, ask = got
        self.assertAlmostEqual(ask, 0.06 if path[0][2] == 0.06 else 0.08)  # first in-band by time

    def test_gate_mechanical_always_true_calibrator_ungateable_is_none(self):
        class NoSig:
            def physics_ok(self, *a): return True
            def calibrator_ok(self, *a): return None   # no fresh prediction
        s = NoSig()
        self.assertTrue(sg.gate_ok("mechanical", "T", 1, "YES", s))
        self.assertTrue(sg.gate_ok("physics", "T", 1, "YES", s))
        self.assertIsNone(sg.gate_ok("calibrator", "T", 1, "YES", s))
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_strategy_grid -k Gate -v`
Expected: FAIL — no attribute `find_entry`.

- [ ] **Step 3: Write entry + gate**

Append to `strategy_grid.py`:

```python
def find_entry(path, lo, hi, close_ms):
    for i, (ts, _bid, ask) in enumerate(path):
        if close_ms is not None and (close_ms - ts) / 1000.0 < MIN_ENTRY_S_TO_CLOSE:
            continue
        if lo <= ask < hi:
            return i, ask
    return None

def gate_ok(gate, ticker, ts_ms, side, signals):
    if gate == "mechanical":
        return True
    if gate == "physics":
        return signals.physics_ok(ticker, ts_ms, side)
    return signals.calibrator_ok(ticker, ts_ms, side)   # None when un-gateable
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_strategy_grid -k Gate -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add openmarketterminal-qt/scripts/research/strategy_grid.py openmarketterminal-qt/tests/test_strategy_grid.py
git commit -m "feat(strategy-grid): first-touch entry + no-look-ahead signal gates"
```

---

### Task 4: Statistics — two nulls, clustered effective_n, walk-forward, BH correction

**Files:**
- Modify: `scripts/research/strategy_grid.py`
- Test: `tests/test_strategy_grid.py`

**Interfaces:**
- Consumes: for a variant, a list of per-contract records `{"pnl","hold_pnl","market_pnl","won","cluster","close_ms"}` (one per entered contract; `cluster` = settlement-window/spot-move key).
- Produces:
  - `clustered_mean(values, clusters) -> {"n","effective_n","mean","se","t","ci95":[lo,hi]}` (cluster-robust SE; `effective_n = n^2 / sum(cluster_size^2)`).
  - `walkforward_delta(records, key) -> float` — pooled `key`-vs-baseline mean on the last of 5 expanding close-time folds (sign-persistence check).
  - `benjamini_hochberg(pvals, alpha=0.05) -> list[bool]` — which hypotheses are rejected.
  - `score_variant(records) -> dict` with `delta_vs_hold`, `delta_vs_market`, clustered stats on `delta_vs_hold`, `walkforward_delta`, and a two-sided p-value.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_strategy_grid.py`:

```python
class StatsTest(unittest.TestCase):
    def test_effective_n_collapses_correlated_clusters(self):
        # 10 values all in ONE cluster -> effective_n == 1
        r = sg.clustered_mean([0.1]*10, ["A"]*10)
        self.assertEqual(r["n"], 10)
        self.assertAlmostEqual(r["effective_n"], 1.0, places=6)
        # 10 values in 10 clusters -> effective_n == 10
        r2 = sg.clustered_mean([0.1]*10, [str(i) for i in range(10)])
        self.assertAlmostEqual(r2["effective_n"], 10.0, places=6)

    def test_benjamini_hochberg_rejects_expected_set(self):
        # classic BH example
        pvals = [0.001, 0.008, 0.039, 0.041, 0.9]
        rej = sg.benjamini_hochberg(pvals, alpha=0.05)
        self.assertEqual(rej, [True, True, True, True, False])

    def test_score_variant_uses_two_nulls(self):
        recs = [{"pnl": 0.05, "hold_pnl": 0.00, "market_pnl": 0.00,
                 "won": True, "cluster": str(i), "close_ms": i} for i in range(40)]
        s = sg.score_variant(recs)
        self.assertAlmostEqual(s["delta_vs_hold"], 0.05, places=6)
        self.assertIn("delta_vs_market", s)
        self.assertGreaterEqual(s["clustered"]["effective_n"], 30)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_strategy_grid -k Stats -v`
Expected: FAIL — no attribute `clustered_mean`.

- [ ] **Step 3: Write the statistics**

Append to `strategy_grid.py`:

```python
def clustered_mean(values, clusters):
    n = len(values)
    if n == 0:
        return {"n": 0, "effective_n": 0.0, "mean": None, "se": None, "t": None, "ci95": [None, None]}
    mean = sum(values) / n
    sizes = collections.Counter(clusters)
    eff = (n * n) / sum(s * s for s in sizes.values())
    groups = collections.defaultdict(float)
    for v, c in zip(values, clusters):
        groups[c] += (v - mean)
    cr_var = sum(g * g for g in groups.values()) / (n * n)
    se = math.sqrt(cr_var)
    t = mean / se if se > 0 else None
    half = 1.96 * se
    return {"n": n, "effective_n": eff, "mean": mean, "se": se, "t": t,
            "ci95": [mean - half, mean + half]}

def _normal_sf(z):
    return 0.5 * math.erfc(z / math.sqrt(2.0))

def _two_sided_p(t):
    return 2.0 * _normal_sf(abs(t)) if t is not None else 1.0

def benjamini_hochberg(pvals, alpha=0.05):
    m = len(pvals)
    order = sorted(range(m), key=lambda i: pvals[i])
    rejected = [False] * m
    max_k = -1
    for rank, idx in enumerate(order, start=1):
        if pvals[idx] <= alpha * rank / m:
            max_k = rank
    for rank, idx in enumerate(order, start=1):
        if rank <= max_k:
            rejected[idx] = True
    return rejected

def _fold_bounds(sorted_close, folds=5):
    m = len(sorted_close)
    return [sorted_close[min(m - 1, (m * k) // folds)] for k in range(1, folds + 1)]

def walkforward_delta(records, key):
    if not records:
        return None
    closes = sorted(r["close_ms"] for r in records)
    last_train_close = _fold_bounds(closes)[-2] if len(closes) >= 5 else closes[0]
    test = [r for r in records if r["close_ms"] > last_train_close]
    if not test:
        return None
    return sum(r["pnl"] - r[key] for r in test) / len(test)

def score_variant(records):
    if not records:
        return {"n": 0}
    dh = [r["pnl"] - r["hold_pnl"] for r in records]
    dm = [r["pnl"] - r["market_pnl"] for r in records]
    clusters = [r["cluster"] for r in records]
    ch = clustered_mean(dh, clusters)
    return {
        "n": len(records),
        "delta_vs_hold": sum(dh) / len(dh),
        "delta_vs_market": sum(dm) / len(dm),
        "clustered": ch,
        "p_value": _two_sided_p(ch["t"]),
        "walkforward_delta": walkforward_delta(records, "hold_pnl"),
        "win_rate": sum(1 for r in records if r["won"]) / len(records),
    }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_strategy_grid -k Stats -v`
Expected: PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
git add openmarketterminal-qt/scripts/research/strategy_grid.py openmarketterminal-qt/tests/test_strategy_grid.py
git commit -m "feat(strategy-grid): clustered effective-n, walk-forward, BH correction"
```

---

### Task 5: Glue + evidence artifacts (full JSON, latest survivors, report) + end-to-end test

**Files:**
- Modify: `scripts/research/strategy_grid.py`
- Test: `tests/test_strategy_grid.py`

**Interfaces:**
- Consumes: all prior functions; `kalshi_edge_common` loaders (`load_brti`, `load_settlements`, `read_jsonl`, `parse_ticker`, `derive_outcome`); `q1_quote_lag.load_quote_series`.
- Produces:
  - `Signals` class (physics from BRTI/vol at entry; calibrator from `kalshi-bot-decisions.jsonl` nearest `calibrated_p` within 60 s) with `physics_ok`/`calibrator_ok`.
  - `run(evidence) -> dict` — the full grid result (schema below).
  - `latest_summary(full) -> dict` — the compact survivors file (`schema_version`, `as_of_utc`, `headline`, `survivors[]` with `trust`).
  - `main()` — writes `kalshi-strategy-grid.json`, `kalshi-strategy-grid-latest.json` to the evidence dir and prints the report to stdout.
- Survivor rule (pre-registered): `survives_correction` (BH over all variants' p-values on `delta_vs_hold`) **and** `effective_n >= 30` **and** `sign(walkforward_delta) == sign(delta_vs_hold)`. `trust`: `measured` if survivor; `insufficient_sample` if `effective_n < 30`; else `rejected`.

- [ ] **Step 1: Write the failing end-to-end test**

Add to `tests/test_strategy_grid.py` (seed a temp evidence dir with a handful of KXBTCD tickers + BRTI + settlements, mirroring `test_kalshi_lag_series.py`'s env pattern):

```python
class EndToEndTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self._prev = os.environ.get("OPENTERMINAL_EVIDENCE_DIR")
        os.environ["OPENTERMINAL_EVIDENCE_DIR"] = self.dir
    def tearDown(self):
        if self._prev is None: os.environ.pop("OPENTERMINAL_EVIDENCE_DIR", None)
        else: os.environ["OPENTERMINAL_EVIDENCE_DIR"] = self._prev

    def _write(self, name, rows):
        with open(os.path.join(self.dir, name), "w") as f:
            for r in rows: f.write(json.dumps(r) + "\n")

    def test_run_emits_versioned_artifacts_with_humility_fields(self):
        # one KXBTCD ticker, a short two-sided quote path, a recorded settlement, BRTI
        close = datetime.datetime(2026, 7, 27, 19, 0, tzinfo=datetime.timezone.utc)
        cms = int(close.timestamp() * 1000)
        tk = "KXBTCD-26JUL2719-T63999.99"
        self._write("kalshi-tickers.jsonl", [
            {"event": "kalshi_ticker", "market_ticker": tk, "ts_ms": cms - 600_000,
             "yes_bid_dollars": "0.20", "yes_ask_dollars": "0.22"},
            {"event": "kalshi_ticker", "market_ticker": tk, "ts_ms": cms - 300_000,
             "yes_bid_dollars": "0.40", "yes_ask_dollars": "0.42"}])
        self._write("kalshi-settlements.jsonl", [
            {"kalshi_market_id": tk, "result": "yes", "expiration_value": 64100.0}])
        self._write("kalshi-cf-benchmarks.jsonl", [
            {"id": "BRTI", "time": cms - 600_000, "value": 64000.0},
            {"id": "BRTI", "time": cms, "value": 64100.0}])
        full = sg.run(sg.common.evidence_dir())
        self.assertEqual(full["schema_version"], sg.SCHEMA_VERSION)
        self.assertIn("variants", full)
        # every variant record carries the humility fields
        v = full["variants"][0]
        for key in ("variant_id", "n_contracts", "effective_n", "delta_vs_hold",
                    "delta_vs_market", "survives_correction", "trust"):
            self.assertIn(key, v)
        latest = sg.latest_summary(full)
        self.assertEqual(latest["schema_version"], sg.SCHEMA_VERSION)
        self.assertIn("headline", latest)
        self.assertIsInstance(latest["survivors"], list)
        # tiny sample -> nothing can be a "measured" survivor
        self.assertTrue(all(s["trust"] == "measured" for s in latest["survivors"]))
        self.assertTrue(all(s["effective_n"] >= 30 for s in latest["survivors"]))
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_strategy_grid -k EndToEnd -v`
Expected: FAIL — no attribute `run`.

- [ ] **Step 3: Write Signals, run, latest_summary, main**

Append to `strategy_grid.py`. Key points: build the per-(contract) quote path once, then for each variant find entry + gate + simulate; collect per-variant records; score; BH across variants; assemble records with `trust`. (Physics uses `common.implied_probability` / `common.realized_vol_per_min_bps`; calibrator reads `kalshi-bot-decisions.jsonl` via `common.read_jsonl`.) Cluster key = the contract's settlement hour (`close_ms // 3_600_000`).

```python
CALIBRATOR_FRESHNESS_MS = 60_000
CALIBRATOR_EDGE = 0.03
SURVIVOR_MIN_EFF_N = 30
BAND_FAMILY_PREFIX = ("KXBTCD", "KXBTC15M")

class Signals:
    """Backward-looking gate signals. physics: recomputed from BRTI at entry;
    calibrator: nearest logged calibrated_p at/<= entry within freshness."""
    def __init__(self, brti, calib_by_ticker):
        self.brti = brti
        self.calib = calib_by_ticker            # ticker -> sorted [(ts_ms, calibrated_p, mid)]

    def physics_ok(self, ticker, ts_ms, side):
        parsed = common.parse_ticker(ticker)
        if parsed is None or parsed["strike"] is None:
            return None
        sample = self.brti.nearest(ts_ms)
        if sample is None:
            return None
        _t, spot, _avg = sample
        window = self.brti.window(ts_ms - 1_800_000, ts_ms)
        sigma = common.realized_vol_per_min_bps(window)
        minutes = (parsed["close_ms"] - ts_ms) / 60000.0
        p = common.implied_probability(spot, parsed["strike"], sigma or 0.0, minutes)
        if p is None:
            return None
        edge = (p - 0.5) if side == "YES" else (0.5 - p)   # physics favours the side
        return edge > 0.0

    def calibrator_ok(self, ticker, ts_ms, side):
        rows = self.calib.get(ticker)
        if not rows:
            return None
        ts = [r[0] for r in rows]
        idx = bisect.bisect_right(ts, ts_ms) - 1
        if idx < 0 or ts_ms - rows[idx][0] > CALIBRATOR_FRESHNESS_MS:
            return None
        _t, cp, mid = rows[idx]
        signed = (cp - mid) if side == "YES" else (mid - cp)
        return signed >= CALIBRATOR_EDGE
```

Then `run(evidence)`: load quotes (`q1_quote_lag.load_quote_series`), BRTI, settlements, calibrator rows; resolve each ticker's outcome (recorded first, derived fallback for KXBTCD only, families gated to `BAND_FAMILY_PREFIX`); build `Signals`; for each variant, walk each contract → entry (find_entry on bet-side path) → gate_ok (skip `None` as ungateable, count them) → `contract_pnl` → record `{pnl, hold_pnl, market_pnl, won, cluster, close_ms}` where `market_pnl` = holding at the mid (a market-priced baseline: `won - mid - fee(mid)`); `score_variant`; collect p-values; `benjamini_hochberg` across variants; set `survives_correction`, `trust`. Return `{schema_version, as_of_utc, data:{...}, method:{...}, variants:[...]}`.

`latest_summary(full)`: survivors = variants with `trust == "measured"`; `headline` states the count or "no variant beats hold+market after correction"; each survivor carries `variant_id, side, band, gate, exit, delta_vs_hold, delta_vs_market, effective_n, ci95, walkforward_delta, trust`.

`main()`: `full = run(common.evidence_dir()); common` writes both JSON files to the evidence dir (atomic) and prints a markdown report; `return 0`.

*(The implementer writes the full bodies; the interfaces, cluster key, market_pnl definition, survivor rule, and schema keys above are exact and must match Task 4's `score_variant` output and the test's asserted keys.)*

- [ ] **Step 4: Run the end-to-end test + full module**

Run: `cd openmarketterminal-qt && python3 -m unittest tests.test_strategy_grid -v`
Expected: PASS (all tasks' tests; the E2E asserts versioned artifacts + humility fields + that a tiny sample yields no `measured` survivor).

- [ ] **Step 5: Smoke-run on real evidence (read-only) and eyeball the headline**

Run: `cd openmarketterminal-qt && python3 scripts/research/strategy_grid.py > /tmp/grid.json 2>&1; python3 -c "import json;d=json.load(open('/tmp/grid.json'));print(d['variants'][0].keys())" 2>/dev/null || tail -5 /tmp/grid.json`
Expected: emits a full grid over real retained contracts; the run writes `kalshi-strategy-grid.json` and `-latest.json` to the evidence dir. Confirm `-latest.json` headline is honest (likely "no variant beats hold+market after correction" on current data).

- [ ] **Step 6: Commit**

```bash
git add openmarketterminal-qt/scripts/research/strategy_grid.py openmarketterminal-qt/tests/test_strategy_grid.py
git commit -m "feat(strategy-grid): signals + run + versioned evidence artifacts (full + latest survivors)"
```

---

## Self-Review

**Spec coverage:** pre-registered grid + stable ids (Task 1) ✓; taker fills + cash-out-at-bid + fees + two-null hold baseline (Tasks 2,4) ✓; first-touch entry + physics + calibrator gates, no look-ahead, ungateable counted (Task 3) ✓; clustered effective_n + walk-forward + BH correction + survivor floor `effective_n≥30` (Task 4,5) ✓; families KXBTCD+KXBTC15M, band excluded, recorded/derived outcomes (Task 5) ✓; versioned full JSON + compact `-latest.json` with `trust`/`ci95`/`effective_n` humility fields (Task 5) ✓; read-only, stdlib-only, reproducible (constraints + all tasks) ✓. **Deferred by design (separate plan):** CLI reader verb, bot advisory panel line, live preview, maker variants, MCP verb.

**Placeholder scan:** Tasks 1–4 have complete code + tests. Task 5's `run`/`main` bodies are described rather than fully transcribed (the module is large and composes prior tasks); this is the one place an implementer writes original glue — the interfaces, cluster key, `market_pnl` formula, survivor rule, and every schema key the E2E test asserts are given exactly, so the contract is unambiguous even though the body is not transcribed. Flagged honestly rather than padded with fake code.

**Type consistency:** `variant_id`/`build_grid` record shape is identical in Tasks 1 and 5; `contract_pnl` return keys (`pnl`,`exited`,`hold_pnl`) match Tasks 2 and 5; `score_variant` output (`clustered.effective_n`, `delta_vs_hold`, `walkforward_delta`, `p_value`) matches Task 4 and the survivor rule in Task 5; `clustered_mean` keys (`effective_n`,`ci95`,`t`) match their uses.
