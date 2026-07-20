# LLM Advisory Scoring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Capture, firewall, resolve, and score LLM (and other forecaster) probability predictions on Kalshi crypto settlement, so we can measure — out-of-sample and after fees — whether a named forecaster beats the market and the deterministic daemon.

**Architecture:** Additive CLI surface under `kalshi auto advise …` plus one durable DB table. A 5-state blind/reveal/post challenge protocol seals a price-free context at `open`, records a blind estimate (`p_pre`) before revealing the market, then a market-informed estimate (`p_post`). Predictions land in the existing `edge_decision_journal` (`source='llm-advisory'`), are resolved by the existing settlement backfill (widened to see this source), and scored by a new paired-OOS scoring module. Nothing touches the deterministic execution path.

**Tech Stack:** C++17, Qt6 (Core/Sql/Test), SQLite via `Database::instance().execute(sql, {params}) -> Result<QSqlQuery>`, migrations via `MigrationRunner`, CLI dispatch in `src/cli/CommandDispatch.cpp`, QtTest suites in `tests/`.

## Global Constraints

- **Spec:** `openmarketterminal-qt/docs/design/2026-07-20-llm-advisory-scoring-design.md` (authoritative).
- **Off the execution path:** never call `prepare_order`/`submit_order`; never grant the LLM order authority. Every advisory row carries `authority:advisory_only, execution_eligible:false, gate:measurement_only, call:LLM_ADVISORY`.
- **Blind allowlist (verified):** blind packet INCLUDES strike floor/cap, distance, `required_move_bps`, `seconds_left`, settlement band + definition, horizon, spot level, realized recent move, realized-vol estimate, and `spot_microstructure` (Coinbase/Gemini/Kraken aggressor). It EXCLUDES (reveal-only) Kalshi `yes/no bid/ask/depth`, `market_implied_probability`, `fair_yes/no`, `divergence`, Kalshi contract flow, daemon `calibrated_probability`/`model_probability`/`model_weight`, and any fee/cost derived from the quote.
- **Verification discipline:** QtTest `QVERIFY`/`QCOMPARE` (fail => non-zero exit; never bare `assert`, a no-op under NDEBUG). Every task's test must FAIL before its implementation exists. `touch` a modified unity-chunk source and confirm rebuild before trusting a pass.
- **Migration number:** next free is **v067** (latest registered is v066).
- **Build (tests):** `cmake -S openmarketterminal-qt -B /tmp/ot-build-test -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENMARKETTERMINAL_BUILD_TESTS=ON` then `cmake --build /tmp/ot-build-test --target <tst_target>`; run via `ctest --test-dir /tmp/ot-build-test -R <name> -V`.
- **TTL:** `ttl = min(configured_max=60s, horizon_limit)`; horizon table ≤60s→10s(or no-open), 1–5m→15s, 5–15m→30s, 15–60m→45s, >60m→60s, abs cap 120s. Two clocks: `prediction_ttl` (commit deadline) and shorter `execution_relevance_ttl` (past it: scored, never trade-influencing).

## File Structure

- Create `src/storage/sqlite/migrations/v067_edge_advisory_challenge.cpp` — durable challenge ledger table.
- Modify `src/storage/sqlite/migrations/RegisterAllMigrations.cpp` — register v067.
- Create `src/services/edge_radar/AdvisoryProtocol.h/.cpp` — canonical JSON + SHA256 hashing, TTL policy, blind-packet builder (strict allowlist), pure functions (no DB, no Qt Widgets) so they unit-test in a Core-only harness.
- Create `src/services/edge_radar/AdvisoryChallengeRepository.h/.cpp` — DB CRUD + atomic state transitions + idempotent commits over `edge_advisory_challenge`, and the `edge_decision_journal` advisory-row writer.
- Create `src/services/edge_radar/AdvisoryScoring.h/.cpp` — pure paired-OOS math (Brier, log loss, reliability, paired improvement, bootstrap CI, participation stats).
- Modify `src/cli/CommandDispatch.cpp` — thin `kalshi_auto_advise_command(...)` dispatcher + `open/commit-blind/reveal/commit-post/score/ledger` handlers; widen the resolver SELECT; add `advise` to usage strings + the `kalshi auto` router.
- Create/modify `tests/tst_advisory_protocol.cpp`, `tests/tst_advisory_repository.cpp`, `tests/tst_advisory_scoring.cpp`; modify `tests/CMakeLists.txt`.
- Modify `src/cli/CMakeLists.txt`/service globs only if new `src/services/edge_radar/*.cpp` are not auto-globbed (verify at Task 2).

---

### Task 1: Migration v067 — `edge_advisory_challenge` table

**Files:**
- Create: `src/storage/sqlite/migrations/v067_edge_advisory_challenge.cpp`
- Modify: `src/storage/sqlite/migrations/RegisterAllMigrations.cpp` (after line `register_migration_v066();`)
- Test: `tests/tst_advisory_repository.cpp` (first test only)

**Interfaces:**
- Produces: table `edge_advisory_challenge`; free function `void register_migration_v067();`

- [ ] **Step 1: Write the failing test** (`tests/tst_advisory_repository.cpp`)

```cpp
#include <QtTest>
#include "storage/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"
using namespace openmarketterminal;
void register_all_migrations();   // from RegisterAllMigrations.cpp

class TstAdvisoryRepository : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QVERIFY(Database::instance().open_in_memory().is_ok());   // helper used by other tst_ suites
        register_all_migrations();
        QVERIFY(MigrationRunner::run_all(Database::instance().handle()).is_ok());
    }
    void table_exists() {
        auto r = Database::instance().execute(
            "SELECT name FROM sqlite_master WHERE type='table' AND name='edge_advisory_challenge'", {});
        QVERIFY(r.is_ok());
        QVERIFY(r.value().next());
        QCOMPARE(r.value().value(0).toString(), QStringLiteral("edge_advisory_challenge"));
    }
};
QTEST_MAIN(TstAdvisoryRepository)
#include "tst_advisory_repository.moc"
```

> If `open_in_memory()`/`run_all` names differ, copy the exact bring-up from an existing DB test (e.g. `tests/tst_ai_ledger.cpp`) — match that suite's `initTestCase` verbatim.

- [ ] **Step 2: Add the CMake target and run to verify it fails**

Add to `tests/CMakeLists.txt`:
```cmake
add_executable(tst_advisory_repository tst_advisory_repository.cpp)
target_link_libraries(tst_advisory_repository PRIVATE openterminal_core Qt6::Test Qt6::Sql)
add_test(NAME tst_advisory_repository COMMAND tst_advisory_repository)
```
Run: `cmake -S openmarketterminal-qt -B /tmp/ot-build-test -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENMARKETTERMINAL_BUILD_TESTS=ON && cmake --build /tmp/ot-build-test --target tst_advisory_repository && ctest --test-dir /tmp/ot-build-test -R tst_advisory_repository -V`
Expected: FAIL — no such table `edge_advisory_challenge`.

- [ ] **Step 3: Write the migration** (`v067_edge_advisory_challenge.cpp`) — follow the exact shape of `v055_edge_decision_journal.cpp`:

```cpp
#include "storage/sqlite/migrations/MigrationRunner.h"
namespace openmarketterminal { namespace {
Result<void> exec(QSqlDatabase& db, const QString& sql) {
    QSqlQuery q(db);
    if (!q.exec(sql)) return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}
Result<void> apply_v067(QSqlDatabase& db) {
    auto r = exec(db,
        "CREATE TABLE IF NOT EXISTS edge_advisory_challenge ("
        " challenge_id TEXT PRIMARY KEY,"
        " state TEXT NOT NULL DEFAULT 'OPEN',"
        " created_at INTEGER NOT NULL,"
        " prediction_ttl_at INTEGER NOT NULL,"
        " execution_relevance_at INTEGER NOT NULL,"
        " ticker TEXT NOT NULL DEFAULT '',"
        " market_id TEXT NOT NULL DEFAULT '',"
        " horizon TEXT NOT NULL DEFAULT '',"
        " settlement_def TEXT NOT NULL DEFAULT '',"
        " context_json TEXT NOT NULL DEFAULT '{}',"
        " context_hash TEXT NOT NULL DEFAULT '',"
        " sealed_hash TEXT NOT NULL DEFAULT '',"
        " nonce TEXT NOT NULL DEFAULT '',"
        " market_at_open_json TEXT NOT NULL DEFAULT '{}',"
        " market_at_blind_json TEXT NOT NULL DEFAULT '{}',"
        " market_at_reveal_json TEXT NOT NULL DEFAULT '{}',"
        " market_at_post_json TEXT NOT NULL DEFAULT '{}',"
        " daemon_prob_at_open REAL NOT NULL DEFAULT -1,"
        " daemon_prob_at_reveal REAL NOT NULL DEFAULT -1,"
        " provider TEXT NOT NULL DEFAULT '', model TEXT NOT NULL DEFAULT '',"
        " prompt_version TEXT NOT NULL DEFAULT '', context_schema_version INTEGER NOT NULL DEFAULT 1,"
        " protocol_version INTEGER NOT NULL DEFAULT 1, agent_id TEXT NOT NULL DEFAULT '',"
        " run_id TEXT NOT NULL DEFAULT '', temperature REAL NOT NULL DEFAULT -1,"
        " p_pre REAL NOT NULL DEFAULT -1, p_post REAL NOT NULL DEFAULT -1,"
        " confidence_pre REAL NOT NULL DEFAULT -1, confidence_post REAL NOT NULL DEFAULT -1,"
        " rationale_pre TEXT NOT NULL DEFAULT '', rationale_post TEXT NOT NULL DEFAULT '',"
        " commit_id_blind TEXT NOT NULL DEFAULT '', commit_id_post TEXT NOT NULL DEFAULT '',"
        " journal_id TEXT NOT NULL DEFAULT '',"
        " ts_blind INTEGER NOT NULL DEFAULT 0, ts_reveal INTEGER NOT NULL DEFAULT 0,"
        " ts_post INTEGER NOT NULL DEFAULT 0)");
    if (r.is_err()) return r;
    r = exec(db, "CREATE INDEX IF NOT EXISTS idx_advisory_challenge_state "
                 "ON edge_advisory_challenge(state, created_at)");
    if (r.is_err()) return r;
    return exec(db, "CREATE INDEX IF NOT EXISTS idx_advisory_challenge_forecaster "
                    "ON edge_advisory_challenge(provider, model, created_at)");
}
} // namespace
void register_migration_v067() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    MigrationRunner::register_migration({67, "edge_advisory_challenge", apply_v067});
}
} // namespace openmarketterminal
```
Then in `RegisterAllMigrations.cpp` add `register_migration_v067();` after `register_migration_v066();` and add its forward declaration next to the others (match how v066 is declared).

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build /tmp/ot-build-test --target tst_advisory_repository && ctest --test-dir /tmp/ot-build-test -R tst_advisory_repository -V`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/storage/sqlite/migrations/v067_edge_advisory_challenge.cpp \
        src/storage/sqlite/migrations/RegisterAllMigrations.cpp \
        tests/tst_advisory_repository.cpp tests/CMakeLists.txt
git commit -m "feat(kalshi): v067 edge_advisory_challenge durable ledger"
```

---

### Task 2: Protocol primitives — canonical JSON, SHA256, TTL, blind-packet builder

**Files:**
- Create: `src/services/edge_radar/AdvisoryProtocol.h`, `src/services/edge_radar/AdvisoryProtocol.cpp`
- Test: `tests/tst_advisory_protocol.cpp`
- Verify: if `src/services/edge_radar/*.cpp` is not glob-picked into `openterminal_core`, add the two new sources explicitly where `KalshiAutoEngine.cpp` is listed.

**Interfaces:**
- Produces:
  - `QByteArray adv::canonical_json(const QJsonObject&)` — sorted-key, compact, fixed 6-dp for doubles.
  - `QString adv::sha256_hex(const QByteArray&)` — `QCryptographicHash::Sha256` hex.
  - `struct adv::TtlPolicy { qint64 prediction_ttl_ms; qint64 execution_relevance_ms; bool may_open; };`
  - `adv::TtlPolicy adv::ttl_for(qint64 seconds_left, qint64 configured_max_ms = 60000);`
  - `QJsonObject adv::build_blind_packet(const QJsonObject& snapshot);` — copies ONLY allowlist fields.
  - `QStringList adv::kBlindForbiddenKeys();` — the reveal-only field names (used by the leak test).

- [ ] **Step 1: Write the failing tests** (`tests/tst_advisory_protocol.cpp`)

```cpp
#include <QtTest>
#include "services/edge_radar/AdvisoryProtocol.h"
using namespace openmarketterminal;
class TstAdvisoryProtocol : public QObject {
    Q_OBJECT
private slots:
    void canonical_json_is_key_order_stable() {
        QJsonObject a{{"b",2},{"a",1}}; QJsonObject b{{"a",1},{"b",2}};
        QCOMPARE(adv::canonical_json(a), adv::canonical_json(b));
    }
    void sha256_detects_mutation() {
        QJsonObject o{{"x",1}};
        const QString h1 = adv::sha256_hex(adv::canonical_json(o));
        o["x"] = 2;
        QVERIFY(adv::sha256_hex(adv::canonical_json(o)) != h1);
    }
    void ttl_is_horizon_aware() {
        QVERIFY(!adv::ttl_for(30).may_open);                       // <=60s: don't open
        QCOMPARE(adv::ttl_for(180).prediction_ttl_ms, qint64(15000));   // 1-5m -> 15s
        QCOMPARE(adv::ttl_for(3600 * 3).prediction_ttl_ms, qint64(60000)); // >60m -> 60s (== max)
        QVERIFY(adv::ttl_for(3600 * 3).execution_relevance_ms < adv::ttl_for(3600*3).prediction_ttl_ms);
    }
    void blind_packet_excludes_every_market_field() {
        QJsonObject snap{
            {"strike_floor", 60000}, {"seconds_left", 900}, {"spot", 61000},
            {"spot_microstructure", QJsonObject{{"aggressor_pressure", 0.2}}},
            {"market_implied_probability", 0.55}, {"fair_yes", 0.54},
            {"divergence", QJsonObject{{"label","DIVERGENCE"}}},
            {"daemon_probability", 0.55}, {"yes_ask", 0.56}};
        const QJsonObject blind = adv::build_blind_packet(snap);
        for (const QString& forbidden : adv::kBlindForbiddenKeys())
            QVERIFY2(!blind.contains(forbidden), qUtf8Printable("leaked: " + forbidden));
        QVERIFY(blind.contains("spot_microstructure"));   // allowlisted
        QVERIFY(blind.contains("strike_floor"));
    }
};
QTEST_MAIN(TstAdvisoryProtocol)
#include "tst_advisory_protocol.moc"
```

- [ ] **Step 2: Add CMake target + run to verify fail**

`tests/CMakeLists.txt`:
```cmake
add_executable(tst_advisory_protocol tst_advisory_protocol.cpp)
target_link_libraries(tst_advisory_protocol PRIVATE openterminal_core Qt6::Test)
add_test(NAME tst_advisory_protocol COMMAND tst_advisory_protocol)
```
Run: `cmake -S openmarketterminal-qt -B /tmp/ot-build-test -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENMARKETTERMINAL_BUILD_TESTS=ON && cmake --build /tmp/ot-build-test --target tst_advisory_protocol`
Expected: FAIL to link/compile — `AdvisoryProtocol.h` not found.

- [ ] **Step 3: Implement `AdvisoryProtocol.{h,cpp}`**

Header declares the namespace `openmarketterminal::adv` with the Interfaces above. Implementation notes (write full bodies):
```cpp
// canonical_json: recurse, sort object keys, doubles via QString::number(v,'f',6),
//   emit compact bytes. (Deterministic string so hashes are stable.)
// sha256_hex: QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()
// ttl_for(seconds_left, max): implement the Global-Constraints table; may_open=false when <=60s;
//   execution_relevance_ms = min(prediction_ttl_ms/2, 15000).
// kBlindForbiddenKeys(): {"yes_bid","yes_ask","no_bid","no_ask","yes_depth","no_depth",
//   "market_implied_probability","market_curve_probability","fair_yes","fair_no",
//   "divergence","daemon_probability","calibrated_probability","model_probability",
//   "model_weight","cost_net_edge","execution"}
// build_blind_packet: start empty; copy ONLY allowlist keys if present:
//   {"strike_floor","strike_cap","distance_bps","required_move_bps","seconds_left",
//    "settlement_band","settlement_def","horizon","spot","realized_move_bps",
//    "realized_vol","spot_microstructure"}.  Never copy by iterating the snapshot.
```

- [ ] **Step 4: Run to verify pass**

Run: `cmake --build /tmp/ot-build-test --target tst_advisory_protocol && ctest --test-dir /tmp/ot-build-test -R tst_advisory_protocol -V`
Expected: PASS (all four slots).

- [ ] **Step 5: Commit**

```bash
git add src/services/edge_radar/AdvisoryProtocol.h src/services/edge_radar/AdvisoryProtocol.cpp \
        tests/tst_advisory_protocol.cpp tests/CMakeLists.txt
git commit -m "feat(kalshi): advisory protocol primitives (canonical hash, TTL, blind packet)"
```

---

### Task 3: Challenge repository — open + atomic/idempotent transitions

**Files:**
- Create: `src/services/edge_radar/AdvisoryChallengeRepository.h/.cpp`
- Test: extend `tests/tst_advisory_repository.cpp`

**Interfaces (produce exact signatures — later tasks depend on these names):**
```cpp
namespace openmarketterminal::adv {
struct OpenParams { QString ticker, market_id, horizon, settlement_def;
                    QJsonObject blind_context, withheld_market; double daemon_prob;
                    qint64 seconds_left, now_ms;
                    QString provider, model, prompt_version, agent_id, run_id; double temperature; };
struct OpenResult  { QString challenge_id, context_hash; qint64 prediction_ttl_at, execution_relevance_at; };
struct CommitParams { QString challenge_id, commit_id; double probability, confidence; QString rationale; qint64 now_ms; };
class AdvisoryChallengeRepository {
public:
    Result<OpenResult> open(const OpenParams&);                 // inserts state=OPEN, computes hashes+nonce
    Result<QString>    commit_blind(const CommitParams&);       // OPEN->COMMITTED_BLIND, writes journal row, returns journal_id
    Result<QJsonObject> reveal(const QString& challenge_id, qint64 now_ms);   // COMMITTED_BLIND->REVEALED, returns sealed_hash+withheld
    Result<void>       commit_post(const CommitParams&);        // REVEALED->COMMITTED_POST, finalizes journal features_json
    Result<int>        expire_stale(qint64 now_ms);             // OPEN past prediction_ttl_at -> EXPIRED
};
}
```
- **Idempotency:** each transition first checks whether the incoming `commit_id` already matches the stored `commit_id_blind`/`commit_id_post`; if so, return the original result without a second write. All transitions run inside `Database::instance().transaction()/commit()` (match the transaction helper other repositories use; if none, wrap `BEGIN`/`COMMIT` via `execute`). Reject wrong-state transitions with `Result::err`.

- [ ] **Step 1: Write failing tests** (append slots to `tst_advisory_repository.cpp`):

```cpp
void open_then_commit_blind_is_idempotent() {
    adv::AdvisoryChallengeRepository repo;
    adv::OpenParams p; p.ticker="KXBTC"; p.market_id="M1"; p.horizon="hourly";
    p.blind_context=QJsonObject{{"strike_floor",60000}}; p.withheld_market=QJsonObject{{"market_implied_probability",0.55}};
    p.daemon_prob=0.55; p.seconds_left=900; p.now_ms=1000; p.model="opus";
    auto o = repo.open(p); QVERIFY(o.is_ok());
    adv::CommitParams c; c.challenge_id=o.value().challenge_id; c.commit_id="K1";
    c.probability=0.62; c.confidence=0.7; c.now_ms=1500;
    auto j1 = repo.commit_blind(c); QVERIFY(j1.is_ok());
    auto j2 = repo.commit_blind(c);                       // retry same commit_id
    QVERIFY(j2.is_ok()); QCOMPARE(j2.value(), j1.value()); // same journal id, no duplicate
    auto n = Database::instance().execute(
        "SELECT COUNT(*) FROM edge_decision_journal WHERE source='llm-advisory'", {});
    QVERIFY(n.is_ok() && n.value().next()); QCOMPARE(n.value().value(0).toInt(), 1);
}
void reveal_before_blind_is_rejected() {
    adv::AdvisoryChallengeRepository repo;
    adv::OpenParams p; p.ticker="KXBTC"; p.market_id="M2"; p.seconds_left=900; p.now_ms=1000;
    p.blind_context=QJsonObject{}; p.withheld_market=QJsonObject{}; p.daemon_prob=0.5;
    auto o = repo.open(p); QVERIFY(o.is_ok());
    QVERIFY(repo.reveal(o.value().challenge_id, 1200).is_err());   // still OPEN
}
void expired_challenge_cannot_commit() {
    adv::AdvisoryChallengeRepository repo;
    adv::OpenParams p; p.ticker="KXBTC"; p.market_id="M3"; p.seconds_left=180; p.now_ms=1000;
    p.blind_context=QJsonObject{}; p.withheld_market=QJsonObject{}; p.daemon_prob=0.5;
    auto o = repo.open(p); QVERIFY(o.is_ok());
    QCOMPARE(repo.expire_stale(o.value().prediction_ttl_at + 1).value(), 1);
    adv::CommitParams c; c.challenge_id=o.value().challenge_id; c.commit_id="K"; c.probability=0.6; c.now_ms=o.value().prediction_ttl_at + 2;
    QVERIFY(repo.commit_blind(c).is_err());
}
```

- [ ] **Step 2: Run to verify fail** — `cmake --build /tmp/ot-build-test --target tst_advisory_repository` → FAIL (repo header missing).

- [ ] **Step 3: Implement the repository.** The journal insert in `commit_blind` writes:
```cpp
// INSERT INTO edge_decision_journal(id, created_at, updated_at, venue, symbol, horizon,
//   market_id, side, call, gate, market_probability, model_probability, confidence,
//   seconds_left, features_json, source, outcome)
// VALUES(?, now, now, 'kalshi', ticker, horizon, market_id, side, 'LLM_ADVISORY',
//   'measurement_only', <market@open>, p_pre, confidence, seconds_left, <features>, 'llm-advisory', -1)
// side = p_pre >= (market@open) ? 'yes' : 'no'
// features_json = { model_version:'llm-advisory-v1', protocol_version:1, context_schema_version:1,
//   challenge_id, context_hash, sealed_hash, ts_opened, ts_blind, horizon, settlement_def,
//   p_pre, market_at_open, daemon_probability, forecaster:{...},
//   authority:'advisory_only', execution_eligible:false, gate:'measurement_only', call:'LLM_ADVISORY' }
```
`commit_post` reads the journal row, merges `p_post`, `market_at_post`, `ts_post` into `features_json`, and updates the row (pre fields untouched). All within one transaction; nonce via `QUuid::createUuid()`.

- [ ] **Step 4: Run to verify pass** — `ctest --test-dir /tmp/ot-build-test -R tst_advisory_repository -V` → PASS.

- [ ] **Step 5: Commit**
```bash
git add src/services/edge_radar/AdvisoryChallengeRepository.h src/services/edge_radar/AdvisoryChallengeRepository.cpp tests/tst_advisory_repository.cpp
git commit -m "feat(kalshi): advisory challenge repository (atomic idempotent transitions)"
```

---

### Task 4: CLI verbs — `advise open | commit-blind | reveal | commit-post`

**Files:**
- Modify: `src/cli/CommandDispatch.cpp` (new static handlers + router entry + usage strings)
- Test: `tests/tst_command_dispatch.cpp` (add a leak-integration slot)

**Interfaces:**
- Consumes: `kalshi_auto_current_snapshot(ticker, now)`, `take_string_option`, `GlobalOpts`, `adv::*`, `AdvisoryChallengeRepository`.
- Produces: `int kalshi_auto_advise_command(const GlobalOpts&, QStringList)`; router line `if (sub == "advise") return kalshi_auto_advise_command(opts, args);` in `kalshi_command`.

- [ ] **Step 1: Write failing test** (`tst_command_dispatch.cpp`) — the load-bearing firewall test at the CLI boundary:
```cpp
void advise_open_json_never_leaks_price() {
    // Build a snapshot fixture in the in-memory evidence path, then invoke the open handler
    // capturing stdout; parse JSON; assert none of adv::kBlindForbiddenKeys() appears anywhere
    // in the serialized "context" object (recursive key scan).
    const QByteArray out = run_cli_capture({"--json","--headless","kalshi","auto","advise","open","--ticker","KXBTC-..."});
    const QJsonObject ctx = QJsonDocument::fromJson(out).object().value("context").toObject();
    for (const QString& k : adv::kBlindForbiddenKeys())
        QVERIFY2(!json_contains_key_deep(ctx, k), qUtf8Printable("leaked " + k));
}
```
> Reuse this suite's existing stdout-capture harness; if none exists, factor the handler body into a testable `QJsonObject build_advise_open(...)` and assert on that object directly (preferred — avoids process capture).

- [ ] **Step 2: Run to verify fail** — build `tst_command_dispatch` → FAIL (`advise` unknown / handler missing).

- [ ] **Step 3: Implement handlers.** `advise open`: fetch snapshot (reuse `kalshi_auto_snapshot` unavailable-path for no-fresh-data), compute `seconds_left`, `adv::ttl_for(...)`; if `!may_open` return error "settlement too near — do not open"; build blind packet via `adv::build_blind_packet`; extract withheld `{market_implied_probability, fair_yes/no, bid/ask/depth, divergence}` and daemon prob from the snapshot; `repo.open(...)`; print `{challenge_id, ticker, context, context_hash, prediction_ttl_ms, execution_relevance_ms, ts_opened, PRICE_WITHHELD:true}`. `commit-blind/reveal/commit-post`: parse `--challenge/--commit-id/--probability[0..1]/--confidence/--rationale`, call repo, print state result. Add `advise` to both `kalshi auto …` usage strings and the router.

- [ ] **Step 4: Run to verify pass** — build + `ctest -R tst_command_dispatch -V` → PASS. Manually: `openterminalcli --json --headless kalshi auto advise open --ticker <live>` shows `PRICE_WITHHELD:true` and no price keys.

- [ ] **Step 5: Commit**
```bash
git add src/cli/CommandDispatch.cpp tests/tst_command_dispatch.cpp
git commit -m "feat(kalshi): advise open/commit-blind/reveal/commit-post CLI verbs"
```

---

### Task 5: Widen settlement resolver to score advisory rows

**Files:**
- Modify: `src/cli/CommandDispatch.cpp:26501-26503` (the `edge_resolve_kalshi_decisions_command` SELECT)
- Test: extend `tests/tst_advisory_repository.cpp`

**Interfaces:** no new symbols; behavior change only.

- [ ] **Step 1: Write failing test** — insert a resolved-eligible `llm-advisory` row (via repo.commit_blind), then call the resolver path with a known settlement and assert the row's `outcome` flips from -1 to 0/1.
```cpp
void settlement_backfill_resolves_advisory_rows() {
    // commit_blind an advisory row for a market whose settlement is known,
    // drive the resolver (or its extracted core), assert:
    auto r = Database::instance().execute(
        "SELECT outcome FROM edge_decision_journal WHERE source='llm-advisory' AND market_id=?", {"M_SETTLED"});
    QVERIFY(r.is_ok() && r.value().next());
    QVERIFY(r.value().value(0).toInt() != -1);
}
```

- [ ] **Step 2: Run to verify fail** — advisory row stays `outcome=-1` (SELECT excludes the source). FAIL.

- [ ] **Step 3: Widen the SELECT** to:
```sql
SELECT id, market_id, side FROM edge_decision_journal
WHERE source IN ('edge journal-kalshi-scan','kalshi auto-plan','llm-advisory')
AND (gate='pass' OR source='kalshi auto-plan' OR source='llm-advisory')
AND outcome = -1
```
(advisory rows carry `gate='measurement_only'`, so they need the explicit `OR source='llm-advisory'`.)

- [ ] **Step 4: Run to verify pass** — advisory row now resolves. PASS. Also run `ctest -R tst_command_dispatch` to confirm no deterministic-path regression.

- [ ] **Step 5: Commit**
```bash
git add src/cli/CommandDispatch.cpp tests/tst_advisory_repository.cpp
git commit -m "feat(kalshi): resolve llm-advisory rows in settlement backfill"
```

---

### Task 6: Scoring math — paired OOS metrics

**Files:**
- Create: `src/services/edge_radar/AdvisoryScoring.h/.cpp`
- Test: `tests/tst_advisory_scoring.cpp`

**Interfaces:**
```cpp
namespace openmarketterminal::adv {
struct ScoredRow { double p_pre, p_post, market, daemon; int outcome; QString cohort; };
struct PairedResult { double brier_pre, brier_post, brier_market, brier_daemon;
                      double logloss_pre, improvement_vs_market_pre, improvement_vs_daemon_pre;
                      double ci_low, ci_high; int n; };
PairedResult score_paired(const QVector<ScoredRow>&, int bootstrap_iters = 2000, quint32 seed = 12345);
struct Participation { int opened, committed_blind, revealed, committed_post, expired, abandoned;
                       double open_to_commit_rate, expiration_rate; };
Participation participation(const QVector<QString>& states);
}
```
Bootstrap uses a **fixed seed** (`std::mt19937(seed)`) — deterministic, testable, and avoids the banned `Math.random`/wall-clock nondeterminism.

- [ ] **Step 1: Write failing tests** — synthetic rows with hand-computed Brier and a monotonic improvement:
```cpp
void brier_matches_hand_computation() {
    QVector<adv::ScoredRow> rows{{0.9,0.9,0.5,0.5,1,"all"},{0.2,0.2,0.5,0.5,0,"all"}};
    auto r = adv::score_paired(rows, 200);
    QVERIFY(qAbs(r.brier_pre - ((0.01+0.04)/2)) < 1e-9);
    QVERIFY(r.improvement_vs_market_pre > 0);   // pre beats a 0.5 market on these
    QVERIFY(r.ci_low <= r.ci_high);
}
void participation_counts_all_states() {
    auto p = adv::participation({"COMMITTED_POST","EXPIRED","ABANDONED","COMMITTED_BLIND"});
    QCOMPARE(p.opened, 4); QCOMPARE(p.expired, 1);
    QVERIFY(qAbs(p.expiration_rate - 0.25) < 1e-9);
}
```

- [ ] **Step 2: Run to verify fail** — header missing → FAIL.
- [ ] **Step 3: Implement** Brier `mean((p-o)^2)`, log loss with clamping to `[1e-6,1-1e-6]`, paired improvement `brier_baseline - brier_pre`, percentile bootstrap CI on the paired improvement, participation ratios. Pure functions, no DB.
- [ ] **Step 4: Run to verify pass** — `ctest -R tst_advisory_scoring -V` → PASS.
- [ ] **Step 5: Commit**
```bash
git add src/services/edge_radar/AdvisoryScoring.h src/services/edge_radar/AdvisoryScoring.cpp tests/tst_advisory_scoring.cpp
git commit -m "feat(kalshi): advisory paired OOS scoring math"
```

---

### Task 7: CLI verbs — `advise score` and `advise ledger`

**Files:**
- Modify: `src/cli/CommandDispatch.cpp`
- Test: extend `tests/tst_command_dispatch.cpp`

**Interfaces:** Consumes `adv::score_paired`, `adv::participation`, reads `edge_decision_journal` (source='llm-advisory', resolved) + `edge_advisory_challenge`.

- [ ] **Step 1: Write failing test** — seed 2 resolved advisory rows + a mix of challenge states, invoke `advise score --json` and `advise ledger --json`, assert the score JSON has `headline_improvement_vs_daemon`, `ci_low/ci_high`, `coverage`, `net_value_after_fees`, and `evidence:"exploratory"` for N<30; ledger JSON has `open_to_commit_rate`.
- [ ] **Step 2: Run to verify fail** — `advise score` unknown → FAIL.
- [ ] **Step 3: Implement.** `advise score`: read resolved rows (parse `features_json` for `p_pre/p_post/market_at_open/daemon_probability`, cohort by settlement-band×distance), filter by `--forecaster-id/--horizon`, call `score_paired`, compute net-of-fees using each row's `market_at_post` and the fee curve `ceil(0.07*C*p*(1-p))`, join participation from the challenge ledger, emit the headline object. `advise ledger`: group `edge_advisory_challenge` by `state`, emit `participation(...)`. Add both to the router + usage.
- [ ] **Step 4: Run to verify pass** — `ctest -R tst_command_dispatch -V` → PASS.
- [ ] **Step 5: Commit**
```bash
git add src/cli/CommandDispatch.cpp tests/tst_command_dispatch.cpp
git commit -m "feat(kalshi): advise score + ledger CLI surfaces"
```

---

### Task 8: End-to-end smoke + docs + regression gate

**Files:**
- Modify: `tests/e2e_headless_smoke.sh` (append an advise cycle)
- Modify: `docs/KALSHI_AUTOMATION_RUNBOOK.md` (operator section for `advise`)
- Verify: `.github/workflows/regression.yml` picks up the three new `tst_advisory_*` via ctest (they auto-register through `add_test`).

- [ ] **Step 1: Write the failing smoke** — append to `e2e_headless_smoke.sh`: run `advise open` on a demo ticker; `grep -vq` any price key in the JSON `context` (fail if present); run `commit-blind`, `reveal`, `commit-post`; assert exit 0 and `state:COMMITTED_POST`. Run the script; expect FAIL until verbs exist (they do after Task 7 — this step confirms the wiring end-to-end).
- [ ] **Step 2: Run to verify** — `bash tests/e2e_headless_smoke.sh` → the advise block passes; full script still green.
- [ ] **Step 3: Update the runbook** — document `advise open/commit-blind/reveal/commit-post/score/ledger`, the firewall guarantee + its honest limit, the TTL table, and that advisory output is `execution_eligible:false` and can never place an order.
- [ ] **Step 4: Full regression** — `ctest --test-dir /tmp/ot-build-test -V` (all suites incl. the 3 new) + the 9 GUI `--selftest-*` exit 0. Confirm no deterministic-path regression.
- [ ] **Step 5: Commit**
```bash
git add tests/e2e_headless_smoke.sh docs/KALSHI_AUTOMATION_RUNBOOK.md
git commit -m "test(kalshi): e2e advise cycle + runbook advisory section"
```

---

## Self-Review

**Spec coverage:** 5-state protocol → T3/T4; blind allowlist + leak exclusion → T2 (`build_blind_packet`, `kBlindForbiddenKeys`) + T4 leak test; durable ledger / selection bias → T1 table + T6 `participation` + T7 `ledger`; dual market baselines → T1 columns + T3 writes; crypto binding → T2 hashes + T3 seal; atomic/idempotent commit → T3; forecaster identity → T1 columns + T3 features; semantic separation flags → T3 journal write + T5 (advisory rows stay out of deterministic queries); TTL → T2 + T4; paired OOS scoring (Brier/logloss/reliability/improvement/bootstrap/coverage/net-fees/cohort) → T6/T7; resolution widening → T5; trial gate → reported by `advise score` headline (gate evaluation is a read of those metrics; no enforcement code needed since authority stays with the daemon). **Reliability/Murphy decomposition** is specified in the spec but only Brier/logloss/improvement are in T6 — *acceptable v1 scope*; reliability-curve buckets and Murphy terms are a fast-follow (note left in T6 for the implementer; not gating the trial-gate headline).

**Placeholder scan:** every code step has real SQL/signatures/tests; the two "reuse existing harness" notes point to named files (`tst_ai_ledger.cpp`, `tst_command_dispatch.cpp`) rather than TBDs.

**Type consistency:** `adv::` names (`build_blind_packet`, `kBlindForbiddenKeys`, `ttl_for`, `TtlPolicy`, `AdvisoryChallengeRepository::{open,commit_blind,reveal,commit_post,expire_stale}`, `score_paired`, `ScoredRow`, `PairedResult`, `participation`, `Participation`) are used identically across tasks. `source='llm-advisory'` and the four `features_json` probability keys (`p_pre`,`p_post`,`market_at_open`,`daemon_probability`) match between T3 (writer) and T7 (reader).
