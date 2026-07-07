# Strategy Sandbox Core (P1+P2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the Strategy Sandbox per `docs/design/2026-07-06-strategy-sandbox-design.md` §4-§9: registry, Paper Executor with exits, Outcome Resolver, Scorer, Leaderboard/Eligibility CLI, daemon jobs — plus the P0-deferred locking and test-polish residuals.

**Architecture:** New GUI-free service layer `src/services/sandbox/` (compiled into `openterminal_core`), one SQLite migration (v056), a `sandbox` CLI command group in `CommandDispatch.cpp`, and two daemon jobs that invoke the CLI (`sandbox tick` every 30 s, `sandbox score-now` every 6 h) — the same subprocess-job pattern the daemon already uses. The executor's market data comes from the rotated `scalp_ticks.jsonl` tail (rows carry `symbol/price/best_bid/best_ask/received_ts_ms` — verified sufficient; the spec's §10 SQLite-tick-cache option is deferred).

**Tech Stack:** Qt 6 / C++17, QtTest + ctest, CMake (Ninja), SQLite via the existing `Database` wrapper. Build dir: `openmarketterminal-qt/build`.

## Global Constraints

- Spec: `docs/design/2026-07-06-strategy-sandbox-design.md`. Binding values: strategy_id = hash of (kind, symbols, params) — param change mints a NEW id; fixed notional per book from params, default $50; fill = post-only limit fills only if recorded ticks trade at/through the limit inside the entry window, in doubt NO fill; ambiguous stop+target in one interval → STOP wins; expiry closes as taker at last trade price; eligibility bars: ≥28 days active, ≥30 resolved, net PnL > 0, max drawdown ≤ 10% of cumulative gross notional, degraded share < 10% — constants in ONE header, changing them is a code change.
- **Power boundary (spec §2):** no sandbox translation unit may reference `ExchangeService`, `place_exchange_order`, or `crypto_submit_order`. Enforced by a ctest `nm` scan (Task 11). The sandbox never arms, never trades.
- **Plan-level deviation from spec §4.2 (state it in the final PR):** `data_quality` is NOT a new journal column written by every producer; the executor derives it at consume time from the row's existing `freshness_json` (`freshest_age_ms > 5000 || live_sources < 2` → `degraded`) and stores it on `sandbox_position`. Same eligibility effect (§6), zero producer churn.
- Deterministic-lane sources only (LLM books are P4, out of scope).
- Surgical edits; every behavior ships with a neuter-verified test; tests use temp-`$HOME` isolation (`QTemporaryDir home; qputenv("HOME", home.path().toUtf8());`); no network in tests.
- GUI-consumed JSON keys: additive only.
- Branch: `feat/strategy-sandbox`. Do NOT touch concurrent uncommitted work (ProfileScreen.*, SecuritySection.cpp) — stage explicit paths only.
- Existing helpers to reuse, never duplicate: `openmarketterminal::cli::automation::{read_tail, horizon_seconds, read_json_object, write_json_object}` (`src/cli/automation/AutomationState.h`), `profile_root_for()` (`src/cli/BridgeDiscoveryFile.h`), migration pattern of `v055_edge_decision_journal.cpp`.

---

### Task 0: Branch

- [ ] **Step 1:**
```bash
cd /Users/haydarevich/src/Open-Terminal && git checkout -b feat/strategy-sandbox && git status --short
```
Expected: new branch, only the concurrent session's ProfileScreen/SecuritySection modifications listed (leave them).

---

### Task 1: Migration v056 — sandbox tables

**Files:**
- Create: `src/storage/sqlite/migrations/v056_strategy_sandbox.cpp`
- Modify: `src/storage/sqlite/migrations/MigrationRunner.h` (add `void register_migration_v056();` after line 103)
- Modify: `src/storage/sqlite/migrations/RegisterAllMigrations.cpp` (call `register_migration_v056();` after v055)
- Modify: `CMakeLists.txt` (add the new .cpp beside v055's entry)
- Test: `tests/tst_sandbox_schema.cpp` (+ `tests/CMakeLists.txt` target `tst_sandbox_schema`, linking `openterminal_core Qt6::Core Qt6::Sql Qt6::Test`, pattern of `tst_crypto_risk`)

**Interfaces:**
- Produces: tables `sandbox_strategy`, `sandbox_position`, `sandbox_fill`, `sandbox_score` exactly as below; later tasks depend on every column name.

- [ ] **Step 1: Write the failing test**

```cpp
#include <QtTest>
#include <QTemporaryDir>
#include "storage/sqlite/Database.h"

class TstSandboxSchema : public QObject {
    Q_OBJECT
  private slots:
    void tables_and_columns_exist() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        // open the profile DB through the normal migration path
        // (follow tst_crypto_risk / tst_headless_runtime for the exact open call;
        //  register_all_migrations() + Database::instance().open() is the pattern)
        QVERIFY(open_profile_database_for_test());
        auto& db = Database::instance();
        for (const QString& t : {QStringLiteral("sandbox_strategy"), QStringLiteral("sandbox_position"),
                                 QStringLiteral("sandbox_fill"), QStringLiteral("sandbox_score")}) {
            auto r = db.execute(QStringLiteral("SELECT count(*) FROM %1").arg(t), {});
            QVERIFY2(r.is_ok(), qUtf8Printable(t));
        }
        // spot-check load-bearing columns
        QVERIFY(db.execute("SELECT strategy_id, kind, symbols, params_json, status, created_at, notes FROM sandbox_strategy", {}).is_ok());
        QVERIFY(db.execute("SELECT position_id, strategy_id, decision_id, symbol, side, hypothetical, qty, limit_price,"
                           " target_price, stop_price, expires_at, state, opened_at, closed_at, entry_fee, exit_fee,"
                           " realized_pnl, close_reason, data_quality, notional_usd FROM sandbox_position", {}).is_ok());
        QVERIFY(db.execute("SELECT fill_id, position_id, ts, kind, price, fee, note FROM sandbox_fill", {}).is_ok());
        QVERIFY(db.execute("SELECT strategy_id, score_date, resolved_count, open_count, unfilled_count, net_pnl,"
                           " hit_rate, avg_win, avg_loss, max_drawdown, degraded_count, gross_notional FROM sandbox_score", {}).is_ok());
        // structural dedup
        auto dup = db.execute("INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side, qty,"
                              " limit_price, expires_at, state, notional_usd) VALUES ('p1','s','d1','BTC-USD','buy',1,1,1,'open',50)", {});
        QVERIFY(dup.is_ok());
        auto dup2 = db.execute("INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side, qty,"
                               " limit_price, expires_at, state, notional_usd) VALUES ('p2','s','d1','BTC-USD','buy',1,1,1,'open',50)", {});
        QVERIFY2(dup2.is_err(), "decision_id must be UNIQUE");
    }
};
QTEST_GUILESS_MAIN(TstSandboxSchema)
#include "tst_sandbox_schema.moc"
```

Implementer note: `open_profile_database_for_test()` is a small local static — copy the exact DB-open incantation from an existing schema-touching test (`tst_crypto_risk.cpp` or `tst_headless_runtime.cpp` shows the working pattern for registering migrations and opening the profile DB under a temp HOME). Do not invent a new bootstrap.

- [ ] **Step 2: Run to verify failure** — `cmake --build openmarketterminal-qt/build --target tst_sandbox_schema && ctest -R tst_sandbox_schema --output-on-failure` → FAIL (`no such table: sandbox_strategy`).

- [ ] **Step 3: Write the migration** (pattern of `v055_edge_decision_journal.cpp` — same `exec` helper, same registration shape):

```cpp
Result<void> apply_v056(QSqlDatabase& db) {
    auto r = exec(db,
        "CREATE TABLE IF NOT EXISTS sandbox_strategy ("
        " strategy_id TEXT PRIMARY KEY,"
        " kind TEXT NOT NULL,"                 // 'scalp'|'spot'|'btc5m'|'kalshi'|'long_short'
        " symbols TEXT NOT NULL,"              // CSV, normalized BTC-USD
        " params_json TEXT NOT NULL,"
        " status TEXT NOT NULL DEFAULT 'active',"  // 'active'|'paused'|'retired'
        " created_at INTEGER NOT NULL,"
        " notes TEXT NOT NULL DEFAULT '')");
    if (r.is_err()) return r;
    r = exec(db,
        "CREATE TABLE IF NOT EXISTS sandbox_position ("
        " position_id TEXT PRIMARY KEY,"
        " strategy_id TEXT NOT NULL,"
        " decision_id TEXT NOT NULL UNIQUE,"
        " symbol TEXT NOT NULL,"
        " side TEXT NOT NULL,"                 // 'buy'|'long'|'short'|'yes'|'no'
        " hypothetical INTEGER NOT NULL DEFAULT 0,"
        " qty REAL NOT NULL,"
        " limit_price REAL NOT NULL,"
        " target_price REAL,"
        " stop_price REAL,"
        " expires_at INTEGER NOT NULL,"        // ms epoch
        " state TEXT NOT NULL,"                // 'pending_fill'|'open'|'closed'|'unfilled'
        " opened_at INTEGER,"
        " closed_at INTEGER,"
        " entry_fee REAL NOT NULL DEFAULT 0,"
        " exit_fee REAL NOT NULL DEFAULT 0,"
        " realized_pnl REAL,"                  // net of all costs; NULL until closed
        " close_reason TEXT,"                  // 'target'|'stop'|'expiry'|'unfilled'|'resolved'
        " data_quality TEXT NOT NULL DEFAULT 'ok',"
        " notional_usd REAL NOT NULL,"
        " created_at INTEGER NOT NULL DEFAULT 0)");
    if (r.is_err()) return r;
    r = exec(db, "CREATE INDEX IF NOT EXISTS idx_sandbox_position_strategy ON sandbox_position (strategy_id, state, created_at)");
    if (r.is_err()) return r;
    r = exec(db,
        "CREATE TABLE IF NOT EXISTS sandbox_fill ("
        " fill_id TEXT PRIMARY KEY,"
        " position_id TEXT NOT NULL,"
        " ts INTEGER NOT NULL,"
        " kind TEXT NOT NULL,"                 // 'open'|'fill'|'target'|'stop'|'expiry'|'unfilled'|'resolved'
        " price REAL NOT NULL DEFAULT 0,"
        " fee REAL NOT NULL DEFAULT 0,"
        " note TEXT NOT NULL DEFAULT '')");
    if (r.is_err()) return r;
    r = exec(db, "CREATE INDEX IF NOT EXISTS idx_sandbox_fill_position ON sandbox_fill (position_id, ts)");
    if (r.is_err()) return r;
    r = exec(db,
        "CREATE TABLE IF NOT EXISTS sandbox_score ("
        " strategy_id TEXT NOT NULL,"
        " score_date TEXT NOT NULL,"           // UTC YYYY-MM-DD
        " resolved_count INTEGER NOT NULL DEFAULT 0,"
        " open_count INTEGER NOT NULL DEFAULT 0,"
        " unfilled_count INTEGER NOT NULL DEFAULT 0,"
        " net_pnl REAL NOT NULL DEFAULT 0,"
        " hit_rate REAL NOT NULL DEFAULT 0,"
        " avg_win REAL NOT NULL DEFAULT 0,"
        " avg_loss REAL NOT NULL DEFAULT 0,"
        " max_drawdown REAL NOT NULL DEFAULT 0,"
        " degraded_count INTEGER NOT NULL DEFAULT 0,"
        " gross_notional REAL NOT NULL DEFAULT 0,"
        " PRIMARY KEY (strategy_id, score_date))");
    return r;
}
```

Registration: `register_migration_v056()` mirroring v055's exact registration call shape (read the bottom of `v055_edge_decision_journal.cpp` and copy it with the version/name changed).

- [ ] **Step 4: Run to verify pass** — same command → PASS. Also run `ctest -R tst_headless_runtime --output-on-failure` (migration count assumptions elsewhere must not break).

- [ ] **Step 5: Commit** — `git add openmarketterminal-qt/src/storage openmarketterminal-qt/CMakeLists.txt openmarketterminal-qt/tests && git commit -m "Add strategy sandbox schema (migration v056)"`

---

### Task 2: SandboxRegistry — immutable param-hashed books

**Files:**
- Create: `src/services/sandbox/SandboxRegistry.h`, `src/services/sandbox/SandboxRegistry.cpp`
- Modify: `CMakeLists.txt` (add to `openterminal_core` sources — the `CORE_SOURCES`/services group where other `src/services/**` files live)
- Test: `tests/tst_sandbox_registry.cpp` (target links `openterminal_core Qt6::Core Qt6::Sql Qt6::Test`)

**Interfaces:**
- Produces (namespace `services::sandbox`):

```cpp
struct StrategyRow {
    QString strategy_id, kind, symbols, params_json, status, notes;
    qint64 created_at = 0;
};
// sha256 of canonical "kind|symbols|params_json(compact, sorted keys)" truncated to 16 hex chars
QString strategy_id_for(const QString& kind, const QString& symbols_csv, const QJsonObject& params);
// inserts if absent; NEVER updates an existing row's params (immutability); returns the id
Result<QString> register_strategy(const QString& kind, const QString& symbols_csv,
                                  const QJsonObject& params, const QString& notes = {});
Result<QList<StrategyRow>> list_strategies(const QString& status_filter = {});  // empty = all
Result<void> set_status(const QString& strategy_id, const QString& status);     // active|paused|retired only
// Season-1 defaults: scalp, spot, btc5m, kalshi, long_short(hypothetical) — idempotent
Result<QList<QString>> seed_default_strategies();
```

- Canonical params serialization: `QJsonDocument(params).toJson(QJsonDocument::Compact)` — QJsonObject already stores keys sorted, note this in a comment.
- Season-1 seeds (exact `params` objects, all with `"notional_usd": 50.0`):
  - `scalp` / `BTC-USD` / `{"notional_usd":50.0,"source":"scalp_decisions","max_age_sec":15,"entry_offset_bps":1.0,"target_bps":25.0,"stop_bps":15.0,"horizon_sec":900}`
  - `spot` / `BTC-USD,ETH-USD,SOL-USD` / `{"notional_usd":50.0,"source":"edge_journal","journal_source":"edge crypto-recommend","min_confidence":0.8,"min_horizon_sec":60,"max_age_sec":900,"target_move_pct":5.0,"stop_move_pct":2.5,"horizon_sec":86400}`
  - `btc5m` / `BTC-USD` / `{"notional_usd":50.0,"source":"edge_journal","journal_source":"edge journal-evaluate-btc5m-live","prediction":true}`
  - `kalshi` / `BTC-USD,ETH-USD,SOL-USD` / `{"notional_usd":50.0,"source":"edge_journal","journal_source":"kalshi","prediction":true}`
  - `long_short` / `BTC-USD` / `{"notional_usd":50.0,"source":"edge_journal","journal_source":"edge long-short-strategy","hypothetical":true,"target_bps":100.0,"stop_bps":45.0,"horizon_sec":300}`

- [ ] **Step 1: Write the failing test** — cases: (a) `strategy_id_for` is stable across calls and differs when any of kind/symbols/params changes; (b) `register_strategy` twice with same inputs → same id, one row; (c) register, then register with one param changed → two rows, first row's `params_json` byte-identical to before (immutability); (d) `set_status("...", "paused")` round-trips, `set_status(..., "bogus")` errs; (e) `seed_default_strategies()` idempotent → 5 rows both times, kinds exactly {scalp, spot, btc5m, kalshi, long_short}. Full test code written against the interface above (same DB bootstrap as Task 1's test).
- [ ] **Step 2: Run to verify failure** — `ctest -R tst_sandbox_registry` → FAIL (no such header).
- [ ] **Step 3: Implement** — SQL through `Database::instance().execute` with bound values; `strategy_id_for` uses `QCryptographicHash::Sha256`, `.toHex().left(16)`. `register_strategy` = `INSERT OR IGNORE` then `SELECT` (never `INSERT OR REPLACE` — immutability).
- [ ] **Step 4: Run to verify pass**; neuter-verify (c) by switching to `INSERT OR REPLACE`, confirm RED, restore.
- [ ] **Step 5: Commit** — `"Add sandbox strategy registry with immutable param-hashed books"`

---

### Task 3: TickTail — market data for the fill model

**Files:**
- Create: `src/services/sandbox/TickTail.h`, `src/services/sandbox/TickTail.cpp` (+ core CMake entry)
- Test: `tests/tst_sandbox_ticktail.cpp`

**Interfaces:**
- Produces (namespace `services::sandbox`):

```cpp
struct TickRow { QString symbol; double price = 0, best_bid = 0, best_ask = 0; qint64 ts_ms = 0; };
// Reads the tail of <profile daemon>/scalp_ticks.jsonl (+ .1 fallback exactly like
// automation::latest_candidate: prev generation prepended when active file is short).
// Returns ticks with ts_ms > since_ms for the given symbol, ascending by ts_ms.
// tail_bytes default 2 MiB (kTickTailBytes) — ~8k ticks, covers a 30 s cycle with margin.
QVector<TickRow> ticks_since(const QString& profile, const QString& symbol,
                             qint64 since_ms, qint64 tail_bytes = kTickTailBytes);
```

- Tick jsonl row shape (verified live): `{"symbol":"ETH-USD","price":1783.02,"best_bid":1783.01,"best_ask":1783.02,"received_ts_ms":"1783392310122","source":"coinbase",...}` — `received_ts_ms` is a JSON **string**. Skip rows with price ≤ 0 or unparseable ts. Use `automation::read_tail` — do not reimplement tailing.

- [ ] **Step 1: Write the failing test** — fixture file with: 3 ticks for BTC-USD (ascending ts), 1 for ETH-USD, 1 corrupt line, 1 tick with price 0; assert symbol filter, since_ms filter (exclusive), ascending order, corrupt/zero rows skipped. Second case: candidate tick only in `.1` file with short active file → found.
- [ ] **Step 2: RED** — `ctest -R tst_sandbox_ticktail` fails to build.
- [ ] **Step 3: Implement** (parse with `QJsonDocument` per line, `received_ts_ms` via `.toString().toLongLong()`).
- [ ] **Step 4: GREEN**; neuter the since_ms filter → RED → restore.
- [ ] **Step 5: Commit** — `"Add sandbox tick tail reader over rotated scalp ticks"`

---

### Task 4: PaperFillModel — pure fill/exit rules (the heart)

**Files:**
- Create: `src/services/sandbox/PaperFillModel.h`, `src/services/sandbox/PaperFillModel.cpp` (+ core CMake entry)
- Test: `tests/tst_sandbox_fillmodel.cpp`

**Interfaces:**
- Produces (namespace `services::sandbox`; all pure, no I/O):

```cpp
struct FeeModel { double maker_bps = 0, taker_bps = 0, slippage_bps = 0; };

struct FillResult { bool filled = false; double price = 0; qint64 ts_ms = 0; };
// Post-only limit BUY fills iff any tick in [entry_start, entry_deadline] trades at or
// below limit_price; fill price = limit_price (resting maker order), maker fee.
// SELL/short mirror: trades at or above. Empty/missing ticks -> not filled.
FillResult try_fill(const QString& side, double limit_price,
                    const QVector<TickRow>& ticks, qint64 entry_deadline_ms);

struct ExitResult { bool exited = false; QString reason; double price = 0; qint64 ts_ms = 0; };
// Checks ticks (ascending) against target/stop/expiry for an OPEN position.
// Long side: stop hit if price <= stop, target hit if price >= target.
// Short side mirrored. First hit in tick order wins; if a SINGLE tick satisfies both
// (can't order intra-tick) -> STOP wins (spec: conservative). now_ms >= expires_at with
// no prior hit -> reason "expiry" at the LAST known tick price (or 0 ticks -> price 0 +
// caller marks data-gap). Exits price as taker (fee applied by caller from FeeModel).
ExitResult check_exit(const QString& side, double target_price, double stop_price,
                      qint64 expires_at, const QVector<TickRow>& ticks, qint64 now_ms);

// Net realized PnL for a closed round trip, all costs in:
// buy/long: (exit - entry) * qty - entry_fee - exit_fee; short mirrored.
double realized_pnl(const QString& side, double entry_price, double exit_price, double qty,
                    double entry_fee, double exit_fee);
double fee_for(double notional, double bps);   // notional * bps / 10000
```

- [ ] **Step 1: Write the failing tests** — the spec-§4.3 matrix, one slot each, with hand-computed expectations:
  1. buy limit 100, ticks trade 100.0 → filled at 100 (maker).
  2. buy limit 100, ticks all ≥ 100.01 → NOT filled.
  3. tick at 99.9 but after entry_deadline → NOT filled.
  4. long open entry 100, target 105, stop 97: tick sequence 103, 105.2 → exit target@105.2's tick? NO — exit price is the target-crossing tick's PRICE (105.2, you get the traded price, conservative would be target price... **rule: exit at the tick's actual price**, which for a stop is worse-than-stop (gap-through) and for a target is the crossing trade price; assert exit price == 105.2, reason "target").
  5. stop gap-through: ticks 99, 92 → reason "stop", price 92 (worse than stop 97 — losses take the gap, wins don't get rounded up: also assert a target gap 108 exits at 108... both directions take the actual traded price; conservatism lives in rule 6).
  6. single tick that satisfies both (long, stop 97, target 105, tick price... impossible for one price to satisfy both on a long — encode the REAL ambiguity: two ticks in the same batch, first hits target, then stop: first-in-order wins → target. Then the intra-tick ambiguity case per spec: construct with stop==target? Not meaningful. **Resolution (state in a comment): spec's "both touched in one interval, ordering unknown" maps to tick-ORDER processing here because ticks carry timestamps; the stop-wins rule applies only when one tick's price satisfies both bounds simultaneously — only possible when stop >= target (misconfigured) → stop wins.** Test: stop 100, target 100, tick 100 → stop.)
  7. expiry: no target/stop hit, now_ms past expires_at, last tick 101 → reason "expiry", price 101.
  8. expiry with zero ticks → exited, reason "expiry", price 0 (caller handles data-gap).
  9. no exit: ticks within bounds, now < expiry → exited == false.
  10. `realized_pnl` long: entry 100, exit 105, qty 0.5, fees 0.05/0.05 → 2.40 exactly. Short mirror: entry 100, exit 95, qty 0.5 → 2.40.
  11. short side fill/exit mirrors (sell limit fills at/above; stop above entry, target below).
- [ ] **Step 2: RED**. **Step 3: Implement** (pure loops over ascending ticks; no floating tolerance games — exact comparisons per rules). **Step 4: GREEN**; neuter stop-wins rule → case 6 RED → restore.
- [ ] **Step 5: Commit** — `"Add sandbox paper fill and exit model"`

---

### Task 5: PaperExecutor — run_cycle

**Files:**
- Create: `src/services/sandbox/PaperExecutor.h`, `src/services/sandbox/PaperExecutor.cpp` (+ core CMake entry)
- Test: `tests/tst_sandbox_executor.cpp`

**Interfaces:**
- Produces (namespace `services::sandbox`):

```cpp
struct CycleReport { int opened = 0, filled = 0, unfilled = 0, closed = 0, skipped = 0; QStringList notes; };
// One executor cycle for one profile: (1) for each ACTIVE non-prediction strategy, pull
// fresh unconsumed journal candidates and open pending_fill positions; (2) advance
// pending_fill -> open|unfilled via try_fill on ticks since opened_at; (3) advance open ->
// closed via check_exit. Prediction/hypothetical books are opened here but CLOSED by the
// Resolver (Task 8). All state transitions also append a sandbox_fill row.
Result<CycleReport> run_cycle(const QString& profile, qint64 now_ms);
```

Candidate selection (per strategy, driven by `params_json`):
- `spot` kind: same query family as `automation_latest_spot_candidate` — `edge_decision_journal WHERE source=<journal_source> AND created_at >= now - max_age_sec*1000 AND side='buy' AND call='BUY CANDIDATE' AND gate='pass'`, plus `automation::horizon_seconds(horizon) >= min_horizon_sec` and `confidence >= min_confidence`, **plus anti-join** `AND id NOT IN (SELECT decision_id FROM sandbox_position)`. Reference price from `features_json.reference_price`.
- `scalp` kind: `automation::latest_candidate(profile, symbol, max_age_sec)` per symbol; decision_id = `automation::candidate_key(d)`.
- `btc5m`/`kalshi`/`long_short` kinds: journal rows with `gate='pass'` for their `journal_source`, opened as `hypothetical`/prediction positions at `features_json.reference_price` (or `market_probability` for prediction contracts — price = probability, qty = notional/price), closed later by the Resolver.
- Position opening: `limit_price = ref * (1 - entry_offset_bps/10000)` (buy), `target_price = limit*(1+target)/stop accordingly` from params (`target_move_pct` OR `target_bps`), `expires_at = created + horizon_sec*1000`, `qty = notional_usd / limit_price`, `data_quality` derived from `freshness_json` (see Global Constraints), `entry_fee = fee_for(notional, maker_bps)` on fill. Fees: reuse the venue fee profile the scalp engine uses — `scalp_fee_profile("coinbase_advanced")` values hardcoded into params at seed time is NOT acceptable; read maker/taker bps from a `FeeModel` built from params keys `maker_bps`/`taker_bps` defaulting to 40/60 with a comment citing coinbase_advanced; recalibration is a params change (new book) by design.
- Transactionality: each position's transition is its own `INSERT`/`UPDATE ... WHERE state='<expected>'` (optimistic guard — a row already advanced by a concurrent cycle is skipped, `skipped++`).
- Also `UPDATE edge_decision_journal SET updated_at=?, outcome=outcome WHERE id=?` is NOT needed — do not touch the journal (anti-join + UNIQUE is the dedup; spec's `consumed_at` intent is satisfied structurally; note this in the PR as part of the §4.2 deviation).

- [ ] **Step 1: Write the failing tests** — with the Task 1 DB bootstrap + fixture journal rows + fixture ticks file: (a) spot candidate → position opened `pending_fill` with correct limit/target/stop/expiry/qty; (b) same candidate on second cycle → skipped (anti-join), no second position; (c) ticks trading through limit → `open` with entry fee and a `fill` sandbox_fill row; (d) ticks never reaching limit until entry deadline → `unfilled`; (e) open position + target-crossing tick → `closed`, `realized_pnl` matches hand computation, `close_reason='target'`; (f) stop and expiry variants; (g) degraded freshness_json → `data_quality='degraded'` on the position; (h) paused strategy → no positions.
- [ ] **Step 2: RED**. **Step 3: Implement.** **Step 4: GREEN**; neuter the anti-join → (b) RED → restore.
- [ ] **Step 5: Commit** — `"Add sandbox paper executor with conservative fills and exits"`

---

### Task 6: `sandbox` CLI group (P1 surface)

**Files:**
- Modify: `src/cli/CommandDispatch.cpp` (new `sandbox_command(const GlobalOpts&, QStringList)` + dispatch hook next to the `automation` group hook; help text in `command_help`)
- Test: `tests/tst_command_dispatch.cpp` (new slots)

**Interfaces:**
- Produces CLI (all `--json`-aware via the existing emit patterns):
```
sandbox seed                      # seed_default_strategies -> {seeded:[ids]}
sandbox list [--status active]    # registry rows
sandbox pause <id> | resume <id> | retire <id>
sandbox tick                      # one PaperExecutor::run_cycle -> CycleReport
sandbox positions [--open|--closed] [--limit N]
```
- Every subcommand calls `init_headless_for_cli(opts, code)` first (DB needed). Emit objects with an `automation_emit_object`-style helper (reuse it — it is file-static; move its declaration up or forward-declare rather than duplicating).

- [ ] **Step 1: Failing dispatch tests** — temp HOME: `sandbox seed` → exit 0, JSON contains 5 ids; `sandbox list --json` → 5 rows; `sandbox pause <spot-id>` then `list --status active` → 4; `sandbox tick` on empty journal → exit 0, `{"opened":0,...}`; `sandbox positions --json` → empty array, exit 0.
- [ ] **Step 2: RED**. **Step 3: Implement.** **Step 4: GREEN.**
- [ ] **Step 5: Commit** — `"Add sandbox CLI command group"`

---

### Task 7: Daemon jobs + shared-state locking (P0 residuals)

**Files:**
- Modify: `src/cli/ServeCommand.cpp` (job specs + `sandbox install-jobs`/`remove-jobs` routing via `daemon_command`; QLockFile on jobs read-modify-write)
- Modify: `src/cli/automation/AutomationState.{h,cpp}` (state lock + `submitted_today_scan` `.1` fallback)
- Test: `tests/tst_automation_state.cpp`, `tests/tst_command_dispatch.cpp`

**Interfaces:**
- `sandbox install-jobs` installs two `managed_by:"strategy-sandbox"` command jobs (pattern of `make_automation_247_job`): `["sandbox","tick"]` interval 30 s timeout 25 s, and `["sandbox","score-now"]` interval 21600 s timeout 120 s. `sandbox remove-jobs` disables them (pattern of `disable_automation_247_jobs`).
- Locking (spec §2 boundary + final-review deferral): new `class StateLock` in AutomationState.h wrapping `QLockFile(state_dir(profile) + "/automation.lock")` with 5 s `tryLock`; taken inside `mark_consumed` and `record_live_attempt` (read-modify-write JSON), and around the daemon's `load_jobs_doc`→`save_jobs_doc` cycles (`jobs_save_update`, `update_job_by_id`). Lock failure → function returns false/error (fail-closed for the two automation writers; job-scheduler update retries next scan).
- `submitted_today_scan` gains the `.1` fallback (same combined-window shape as `latest_candidate` — extract the shared "tail with .1 fallback" buffer-builder into one static helper used by both; no duplication).

- [ ] **Step 1: Failing tests** — (a) `mark_consumed` under a held lock (acquire StateLock in the test, call mark_consumed with a 100 ms timeout override) → returns false, file unchanged; (b) `submitted_today_scan` finds a submitted line that lives only in `orders_path + ".1"`; (c) dispatch: `sandbox install-jobs` → jobs doc contains both jobs with `managed_by:"strategy-sandbox"`; `remove-jobs` disables them.
- [ ] **Step 2: RED**. **Step 3: Implement.** **Step 4: GREEN** + run `tst_serve_command`.
- [ ] **Step 5: Commit** — `"Install sandbox daemon jobs and lock shared automation state"`

---

### Task 8: Outcome Resolver — prediction and hypothetical books

**Files:**
- Create: `src/services/sandbox/SandboxResolver.h`, `src/services/sandbox/SandboxResolver.cpp` (+ core CMake entry)
- Modify: `src/services/sandbox/PaperExecutor.cpp` (`run_cycle` calls `resolve_pending(profile, now_ms)` at the end)
- Test: `tests/tst_sandbox_resolver.cpp`

**Interfaces:**
```cpp
struct ResolveReport { int resolved = 0, pending = 0; };
// For open prediction/hypothetical positions: look up the journal row by decision_id.
// Prediction contracts (btc5m/kalshi): journal outcome 1 -> payout 1.0/contract,
// outcome 0 -> payout 0; realized_pnl = (payout - entry_price) * qty - fees.
// outcome -1 (unresolved) and past expires_at + grace (24 h) -> close 'expiry' pnl 0
// with note 'never resolved' (excluded from hit-rate by close_reason).
// long_short hypothetical: resolve against features_json target/stop using ticks if
// available, else at expiry vs reference price — pnl marked hypothetical by the flag.
Result<ResolveReport> resolve_pending(const QString& profile, qint64 now_ms);
```
- Journal `outcome` column semantics: `-1` unresolved, `0` lose, `1` win (verify against one existing resolver call site — search `SET outcome` in CommandDispatch.cpp — and cite the file:line in the implementation comment; if semantics differ, follow the code, not this plan, and flag it in your report).

- [ ] **Step 1: Failing tests** — fixture journal rows with outcome 1/0/-1 + matching open positions: win pays out, loss zeroes, unresolved past grace closes neutral; hypothetical stays flagged.
- [ ] **Step 2: RED**. **Step 3: Implement.** **Step 4: GREEN.**
- [ ] **Step 5: Commit** — `"Resolve sandbox prediction and hypothetical books from journal outcomes"`

---

### Task 9: Scorer + Leaderboard

**Files:**
- Create: `src/services/sandbox/SandboxScorer.h`, `src/services/sandbox/SandboxScorer.cpp` (+ core CMake entry)
- Modify: `src/cli/CommandDispatch.cpp` (`sandbox score-now`, `sandbox leaderboard`, `sandbox book <id>`)
- Test: `tests/tst_sandbox_scorer.cpp`, dispatch tests

**Interfaces:**
```cpp
// Recomputes sandbox_score rows for every strategy for every UTC day that has activity
// (idempotent upsert; yesterday+today minimum). Aggregates per (strategy_id, day of
// closed_at): resolved_count (state closed, reason != unfilled), unfilled_count,
// open_count (as of now), net_pnl = sum(realized_pnl), hit_rate = wins/resolved
// (win = realized_pnl > 0), avg_win/avg_loss, gross_notional = sum(notional_usd),
// degraded_count, max_drawdown = peak-to-trough of the strategy's FULL cumulative
// realized-pnl curve ordered by closed_at (stored on the latest day's row).
Result<void> score_all(const QString& profile, qint64 now_ms);

struct LeaderboardRow { QString strategy_id, kind, status; int resolved = 0; double net_pnl = 0,
       hit_rate = 0, max_drawdown = 0, gross_notional = 0; int degraded = 0; bool hypothetical = false; };
Result<QList<LeaderboardRow>> leaderboard(const QString& profile);   // season-to-date rollup
```
- Display rules (CLI layer): books with `resolved < kMinResolvedSample` listed under `INSUFFICIENT SAMPLE`, never ranked; hypothetical books in a separate section; a flat book prints `no demonstrated edge` — that is a valid result, not an error.

- [ ] **Step 1: Failing tests** — fixture positions with hand-computed aggregates: 3 closed (pnl +2.40, −1.10, +0.30) + 1 unfilled + 1 open → resolved 3, net 1.60, hit 2/3, avg_win 1.35, avg_loss −1.10, drawdown 1.10; idempotency (run twice → same rows); degraded counted separately.
- [ ] **Step 2: RED**. **Step 3: Implement.** **Step 4: GREEN**; neuter drawdown (return 0) → RED → restore.
- [ ] **Step 5: Commit** — `"Add sandbox scorer and leaderboard"`

---

### Task 10: Live Eligibility Gate

**Files:**
- Create: `src/services/sandbox/SandboxEligibility.h` (header-only: constants + pure function), `src/services/sandbox/SandboxEligibility.cpp` if needed
- Modify: `src/cli/CommandDispatch.cpp` (`sandbox eligibility`)
- Test: `tests/tst_sandbox_eligibility.cpp`

**Interfaces:**
```cpp
// SPEC §4.7 — changing these is a reviewed code change, not a runtime knob.
inline constexpr int    kMinActiveDays      = 28;
inline constexpr int    kMinResolvedSample  = 30;
inline constexpr double kMaxDrawdownFrac    = 0.10;   // of cumulative gross notional
inline constexpr double kMaxDegradedShare   = 0.10;

struct EligibilityInput { int active_days = 0, resolved = 0, degraded = 0; double net_pnl = 0,
       max_drawdown = 0, gross_notional = 0; bool hypothetical = false; };
struct EligibilityVerdict { bool eligible = false; QStringList blockers; };
EligibilityVerdict evaluate_eligibility(const EligibilityInput& in);
// hypothetical -> never eligible ("hypothetical instrument"); every failed bar adds a
// named blocker; eligible only when blockers empty. REPORT-ONLY: no code path from this
// function or its CLI to any arm/setting (enforced by Task 11's boundary test).
```

- [ ] **Step 1: Failing tests** — the spec-§8 boundary matrix: 29 vs 30 resolved; 27 vs 28 days; net_pnl exactly 0 (blocked — bar is `> 0`); drawdown exactly at 10% (allowed — bar is `<=`); degraded exactly 10% (blocked — bar is `< 0.10`); hypothetical always blocked; all-pass case eligible with empty blockers.
- [ ] **Step 2: RED**. **Step 3: Implement + CLI** (`sandbox eligibility` builds inputs from registry + scorer rollup + first-position date). **Step 4: GREEN.**
- [ ] **Step 5: Commit** — `"Add evidence-only live eligibility gate"`

---

### Task 11: Power-boundary test + e2e smoke + regression gate

**Files:**
- Create: `tests/sandbox_boundary_check.sh`, `tests/e2e_sandbox_smoke.sh` (+ `add_test` entries in `tests/CMakeLists.txt`; boundary check REQUIRED, smoke non-blocking is NOT acceptable — both are required, they are deterministic and offline)

**Interfaces:**
- `sandbox_boundary_check.sh`: locate the build's object files for `PaperExecutor.cpp`, `SandboxScorer.cpp`, `SandboxResolver.cpp`, `SandboxEligibility*`, run `nm -u <obj> | grep -E "ExchangeService|place_exchange_order|crypto_submit_order"` → any hit = exit 1. (Object paths under `openmarketterminal-qt/build/CMakeFiles/openterminal_core.dir/src/services/sandbox/` — resolve with `find`, fail if zero objects found so the test can't pass vacuously.)
- `e2e_sandbox_smoke.sh`: temp HOME; `sandbox seed` → write one fixture spot journal row via `openterminalcli` … journal rows need the DB — instead: the smoke drives the binary end-to-end with a fixture SCALP candidate (file-based, no DB seeding needed): write a fresh `PAPER TRADE CANDIDATE` line + fixture ticks that trade through the limit and then through the target → `sandbox tick` twice → `sandbox positions --closed` shows one closed position with `close_reason=target` → `sandbox score-now && sandbox leaderboard --json` shows the scalp book with resolved 1 and the hand-computed pnl. Assert with `grep`/`python3 -c` on the JSON.

- [ ] **Step 1: Write both scripts + wire into ctest; run RED first** (boundary script against a deliberately-broken temp copy proves the grep bites: temporarily add `#include "trading/ExchangeService.h"` + a dummy reference in PaperExecutor.cpp, rebuild, script must FAIL; revert).
- [ ] **Step 2: Full gate** — `ctest --output-on-failure` (all suites), build `openterminalcli` + `OpenTerminal`, run the 9 GUI `--selftest-*` one-shots (all exit 0).
- [ ] **Step 3: Real-profile read-only smoke** — `./openterminalcli --json sandbox list` and `sandbox tick` on the default profile (daemon data present): exit 0, no exceptions, cycle report sane; confirm no file writes outside the profile dir.
- [ ] **Step 4: Commit** — `"Add sandbox power-boundary check and end-to-end smoke"`; push branch `feat/strategy-sandbox`.

---

## Self-review notes

- **Spec coverage:** §4.1→T2, §4.2→T1+T5 (with the stated consumed-at deviation: structural anti-join + UNIQUE instead of a journal column — flag in PR), §4.3→T4+T5, §4.4→T8, §4.5→T9 (+display rules), §4.7→T10, §5 flow→T5/T7 wiring, §6→derived data_quality (stated deviation), §7 error handling→T5 optimistic state guards + T8 grace path, §8 tests→every task + T11 e2e, §9 P1/P2 build order preserved. §4.6 Risk Review (P3) intentionally out of scope — it is schedule/ops config, not daemon code; separate follow-up.
- **P0 residuals folded in:** QLockFile (jobs + consumed + counter) and `submitted_today_scan` `.1` fallback → T7. M1/M9 test polish: fold M9's parser cases into T7's test additions if touching that file; otherwise leave — they were triaged defer.
- **Type consistency:** `TickRow` defined once in TickTail.h and consumed by PaperFillModel/PaperExecutor; `automation::horizon_seconds` reused, not redefined; eligibility constants live only in SandboxEligibility.h.
- **Judgment calls encoded:** exits take the actual traded price (gaps hurt you, crossing trades pay you — symmetric and honest); prediction entry price = market_probability; fees default 40/60 bps maker/taker as params (recalibration = new book by design); unresolved predictions close neutral after 24 h grace.
