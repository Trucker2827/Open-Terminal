# Strategy Sandbox — Real-Horizon Reshape Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Make the sandbox compete only on horizons that map to real tradeable instruments, and let multiple horizon variants run off one signal feed without cannibalizing each other.

**Why:** btc5m (5-min BTC model), chronos2_5m, and scalp (sub-minute, fee-dead) have no venue / no edge. The real grid is Kalshi 15m/1h/1d (prediction strikes), Coinbase spot 1h/4h/1d (the app's `spot-swing-gate` supports exactly `1h|4h|1d`), and chronos 15m/1h/1d. long_short stays hypothetical.

**Architecture:** One migration (v058) relaxing the global `decision_id` UNIQUE to a per-strategy composite so N horizon books can each consume the same journal signal; a one-line executor anti-join change to match; a reshaped registry seed set + explicit retirement of the removed kinds; and updated daemon producer job specs so the real horizons are actually journaled.

**Tech Stack:** Qt6/C++17, QtTest+ctest, CMake (Ninja), SQLite via `Database`. Build dir `openmarketterminal-qt/build`. Branch `feat/sandbox-horizons`.

## Global Constraints

- strategy_id = sha256(kind|symbols|params) — every param change mints a new book; retiring a book = pausing it (reversible). Removed books MUST be explicitly retired (seed's stale-retire only covers kinds still in the seed set).
- Power boundary intact: no sandbox TU references mcp/trading/cli symbols (nm test still REQUIRED).
- Every behavior ships a neuter-verified test; temp-`$HOME` isolation; no network in tests.
- The live default-profile DB currently holds 0 sandbox positions — a table-rebuild migration is safe (no data to preserve), but the migration MUST still copy any existing rows for correctness on other profiles.
- Surgical edits; stage only sandbox files (a concurrent Codex session may dirty the tree — selective-stage, verify `git diff --cached --stat`).
- Real horizon map (bind exactly):
  - **spot** (kind spot, Coinbase): variants at horizon_sec 3600 (1h), 14400 (4h), 86400 (1d); target_move_pct 2/3/5 respectively; min_horizon_sec 3600; source 'edge crypto-recommend' — BUT see Task 3: the producer must emit ≥1h horizons for these to match.
  - **kalshi** (kind kalshi, prediction): variants at horizon_sec 900 (15m), 3600 (1h), 86400 (1d); source 'edge journal-kalshi-scan'; max_age_sec = horizon_sec.
  - **chronos** (Codex's kind chronos2_*): keep 15m/1h/1d + equity; drop chronos2_5m.
  - Retire entirely: **scalp**, **btc5m**, **chronos2_5m**.

---

### Task 0: Branch
- [ ] `cd /Users/haydarevich/src/Open-Terminal && git checkout -b feat/sandbox-horizons && git status --short`

---

### Task 1: Migration v058 — per-strategy dedup + executor anti-join

**Files:** Create `src/storage/sqlite/migrations/v058_sandbox_position_dedup.cpp`; modify `MigrationRunner.h` (+`register_migration_v058();`), `RegisterAllMigrations.cpp`, both `CMakeLists.txt` migration lists; modify `src/services/sandbox/PaperExecutor.cpp` (all anti-join queries); Test: `tests/tst_sandbox_schema.cpp` (extend), `tests/tst_sandbox_executor.cpp`.

**Interfaces:** After v058, `sandbox_position` has `UNIQUE(strategy_id, decision_id)` instead of `decision_id TEXT UNIQUE`. Executor anti-joins become `... AND id NOT IN (SELECT decision_id FROM sandbox_position WHERE strategy_id = ?)` (bind the current strategy_id) in every lane (spot, scalp, prediction, hypothetical); the scalp lane's in-memory consumed check similarly scopes by strategy_id.

- [ ] **Step 1 (test):** In tst_sandbox_schema, replace the "duplicate decision_id inserts fail" assertion with: same decision_id under the SAME strategy_id fails (composite UNIQUE), but the same decision_id under a DIFFERENT strategy_id SUCCEEDS. In tst_sandbox_executor, add: two active spot books (2 distinct strategy_ids, same source) + one gate-pass journal row → BOTH open a position from it (per-strategy dedup); running the cycle twice does not double-open for either book.
- [ ] **Step 2:** RED — build + `ctest -R "tst_sandbox_schema|tst_sandbox_executor"` fails (old global UNIQUE blocks the second book; old anti-join hides the row).
- [ ] **Step 3:** Write v058 as a table rebuild (SQLite can't drop a column UNIQUE in place): `CREATE TABLE sandbox_position_new (... same columns ..., UNIQUE(strategy_id, decision_id))`; `INSERT INTO sandbox_position_new SELECT ... FROM sandbox_position`; `DROP TABLE sandbox_position`; `ALTER TABLE sandbox_position_new RENAME TO sandbox_position`; recreate `idx_sandbox_position_strategy`. Register after v057 (verify the current highest is v057 from the concurrent session; if v057 absent, use v057). Update the executor anti-joins + scalp consumed-scope to filter by strategy_id.
- [ ] **Step 4:** GREEN both suites; neuter-verify by reverting the executor anti-join to global → the two-books-open test goes RED → restore.
- [ ] **Step 5:** Commit "Scope sandbox position dedup to per-strategy (migration v058)".

---

### Task 2: Reshape the seed set + retire unreal books

**Files:** modify `src/services/sandbox/SandboxRegistry.cpp` (seed list + a `retire_removed_kinds` helper), `SandboxRegistry.h`; Test: `tests/tst_sandbox_registry.cpp`.

**Interfaces:** `seed_default_strategies()` now seeds: spot_1h/spot_4h/spot_1d (kind 'spot', params differ by horizon_sec/target_move_pct/min_horizon_sec), kalshi_15m/1h/1d (kind 'kalshi'), long_short (unchanged hypothetical). It does NOT seed scalp or btc5m. After seeding, it calls a new step that retires any ACTIVE book whose kind is 'scalp' or 'btc5m' (removed kinds) — so a reseed durably kills them. The seed↔producer contract test (from the kalshi fix) extends to assert every seeded spot/kalshi variant's journal_source still matches its producer.

- [ ] **Step 1 (test):** registry test: after `seed_default_strategies()`, no ACTIVE book has kind 'scalp' or 'btc5m'; exactly 3 active 'spot' + 3 active 'kalshi' + 1 'long_short'; each spot variant has a distinct horizon_sec in {3600,14400,86400}; each kalshi variant in {900,3600,86400}; idempotent (twice → same set). Extend the seed↔producer contract test to the new variants (all spot → 'edge crypto-recommend', all kalshi → 'edge journal-kalshi-scan').
- [ ] **Step 2:** RED.
- [ ] **Step 3:** Rewrite the `Seeds` list per the horizon map; add `retire_removed_kinds()` (`UPDATE sandbox_strategy SET status='retired' WHERE status='active' AND kind IN ('scalp','btc5m')`) called at the end of `seed_default_strategies`. Keep chronos2_* seeds as-is EXCEPT remove the chronos2_5m entry (it lives in the same seed list from the concurrent session's merge).
- [ ] **Step 4:** GREEN; neuter-verify retire_removed_kinds (skip it → a pre-seeded scalp row stays active → test RED) → restore.
- [ ] **Step 5:** Commit "Reshape sandbox season-1 books to real tradeable horizons".

---

### Task 3: Producer jobs emit the real horizons

**Files:** modify `src/cli/ServeCommand.cpp` (sandbox `install-jobs` job specs — the `collector_job_specs`-style block that Codex expanded); Test: `tests/tst_command_dispatch.cpp` (install-jobs assertion).

**Interfaces:** the spot feed job changes from `edge crypto-universe --horizon-sec 60` to emit ≥1h horizons — use `edge spot-swing-gate --symbols BTC,ETH,SOL --horizon 1h` (and, if cheap, add 4h/1d variants or a single job the lane tolerates). Kalshi scan job unchanged (it journals all live contracts; the books filter by horizon). Remove/repoint the chronos2_5m job. Keep `sandbox tick` (30s) + `score-now`.

- [ ] **Step 1 (test):** extend the install-jobs dispatch test to assert the spot producer job's command contains `spot-swing-gate` (not `crypto-universe --horizon-sec 60`) and that no installed job runs a 5m chronos forecast. RED first.
- [ ] **Step 2-4:** Implement the job-spec change; verify `edge spot-swing-gate` is the right producer that writes `source='edge crypto-recommend'` rows with ≥1h `horizon` (CHECK in CommandDispatch.cpp — if spot-swing-gate journals under a different source, point the spot books' journal_source at whatever it actually writes, and update Task 2's contract test to match — the producer is the source of truth). GREEN.
- [ ] **Step 5:** Commit "Feed sandbox spot books real ≥1h horizons".

---

### Task 4: Regression gate + live reseed

- [ ] **Step 1:** Full `ctest --output-on-failure` green (incl. sandbox_boundary_check + e2e_sandbox_smoke REQUIRED); build openterminalcli + OpenTerminal.
- [ ] **Step 2:** Whole-branch review (sandbox-scoped).
- [ ] **Step 3:** On merge, re-run against the live default profile: `openterminalcli --profile default sandbox seed` (retires scalp/btc5m, registers the horizon variants), `sandbox install-jobs` (repoints producers), confirm `sandbox list` shows only real books active and the scheduler keeps ticking.

## Self-review

- Covers the user's correction: unreal books (scalp/btc5m/chronos2_5m) retired; real grid seeded (Kalshi 15m/1h/1d, Coinbase spot 1h/4h/1d, chronos 15m/1h/1d).
- Unblocks multi-horizon-per-signal via v058 per-strategy dedup (the final-review deferred item, now needed).
- Open verification in Task 3: confirm which `source` string `edge spot-swing-gate` actually journals; the producer is authoritative — adjust the spot books' journal_source + contract test to it, don't assume.
- Deliberately NOT touched: long_short (hypothetical, unchanged), chronos2 15m/1h/1d/equity (Codex's, real horizons), the scorer/eligibility/resolver (horizon-agnostic).
