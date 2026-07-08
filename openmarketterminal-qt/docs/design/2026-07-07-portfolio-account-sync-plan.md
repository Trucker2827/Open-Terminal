# Portfolio Account Sync — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One "Sync accounts" action mirrors every connected account (equity brokers + crypto exchanges) into read-only per-account portfolios, plus a live "All Accounts" aggregate, all in the portfolio base currency.

**Architecture:** A new `AccountSyncService` enumerates connected accounts and, per account, calls an `IAccountSource` (equity via `BrokerRegistry`/`AccountManager`, crypto via `ExchangeService`) to fetch normalized holdings, then a pure mirror-reconcile writes them into that account's synced portfolio via `PortfolioRepository`. Summaries reuse the existing pure `build_summary` (FX conversion + `snapshot_safe()` gate) and `PortfolioHoldingDisplay` badge; a pure aggregate-merge unions synced portfolios for the virtual "All Accounts" view.

**Tech Stack:** Qt6/C++17, CMake+Ninja, SQLite via `Database`/repositories, QtTest+ctest. Tests build under `/tmp/ot-build-test` with `-DOPENMARKETTERMINAL_BUILD_TESTS=ON`; app builds under `openmarketterminal-qt/build`.

## Global Constraints

- **Read-only.** The feature calls only `get_holdings` / `get_positions` / `get_funds` / ccxt `fetch_balance`. It NEVER places/cancels orders and touches none of the live-trading gates.
- **Manual portfolios are never modified** — only portfolios whose `sync_source != ''` are mirror-managed.
- **Mirror only on success:** a failed/errored fetch leaves the synced portfolio intact (never wipe on transient error); an empty-but-successful fetch DOES mirror to empty.
- **Every fix ships a neuter-verified regression test** (confirm it FAILS without the change). `assert()` is a no-op under NDEBUG — use QtTest `QVERIFY`/`QCOMPARE`.
- **Stale-object trap:** after restoring a neutered header via copy, `touch` it before rebuilding so Ninja recompiles.
- **Surgical edits only.** Match surrounding style. Unity builds concatenate `.cpp`s — prefix migration statics `vNNN_*` (see v022).
- Next migration version is **v060**.

---

### Task 1: Storage migration v060 — sync columns + `has_cost_basis`

**Files:**
- Create: `src/storage/sqlite/migrations/v060_portfolio_account_sync.cpp`
- Modify: `src/storage/sqlite/migrations/MigrationRunner.h` (add `void register_migration_v060();` after the v059 line)
- Modify: `src/storage/sqlite/migrations/RegisterAllMigrations.cpp` (add `register_migration_v060();` after the v059 call)
- Modify: `src/CMakeLists.txt` and `tests/CMakeLists.txt` if migration `.cpp`s are listed explicitly (grep `v059_sandbox_position_dedup_repair.cpp`; add the new file next to it in every list that names it)
- Test: `tests/tst_portfolio_account_sync_migration.cpp`

**Interfaces:**
- Produces columns: `portfolios.sync_source TEXT DEFAULT ''`, `portfolios.synced_at TEXT DEFAULT ''`, `portfolios.sync_error TEXT DEFAULT ''`, `portfolio_assets.has_cost_basis INTEGER DEFAULT 1`, index `idx_portfolios_sync_source`.

- [ ] **Step 1: Write the failing test** — `tests/tst_portfolio_account_sync_migration.cpp`. Mirror `tst_pm_paper.cpp` bring-up (QTemporaryDir HOME + `HeadlessRuntime rt_; rt_.init("default")`), then assert the four columns exist:

```cpp
#include "core/headless/HeadlessRuntime.h"
#include "storage/sqlite/Database.h"
#include <QtTest>
#include <QTemporaryDir>
using namespace openmarketterminal;
class TstAccountSyncMigration : public QObject {
    Q_OBJECT
    QTemporaryDir home_;
    headless::HeadlessRuntime rt_;
  private slots:
    void initTestCase() {
        QVERIFY(home_.isValid());
        qputenv("HOME", home_.path().toUtf8());
        auto r = rt_.init("default");
        QVERIFY2(r.ok, qPrintable(r.error));
    }
    void columnsExist() {
        struct C { const char* table; const char* col; };
        for (auto c : {C{"portfolios","sync_source"}, C{"portfolios","synced_at"},
                       C{"portfolios","sync_error"}, C{"portfolio_assets","has_cost_basis"}}) {
            auto r = Database::instance().execute(QString("PRAGMA table_info(%1)").arg(c.table));
            QVERIFY2(r.is_ok(), c.table);
            bool found = false;
            auto q = r.value();
            while (q.next()) if (q.value(1).toString() == c.col) found = true;
            QVERIFY2(found, qPrintable(QString("%1.%2 missing").arg(c.table, c.col)));
        }
    }
};
QTEST_APPLESS_MAIN(TstAccountSyncMigration)
#include "tst_portfolio_account_sync_migration.moc"
```

Register in `tests/CMakeLists.txt` next to another `HeadlessRuntime`-linked test (copy the link libs from the `tst_pm_paper` block — it links the core lib, not just Qt6::Core).

- [ ] **Step 2: Run test to verify it fails** — Configure+build:
`cmake -S . -B /tmp/ot-build-test -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENMARKETTERMINAL_BUILD_TESTS=ON && cmake --build /tmp/ot-build-test --target tst_portfolio_account_sync_migration`
Run: `/tmp/ot-build-test/tests/tst_portfolio_account_sync_migration`
Expected: FAIL — `columnsExist` cannot find the new columns.

- [ ] **Step 3: Write the migration** — copy `v022_portfolio_broker_link.cpp` structure exactly (the `vNNN_sql`, `vNNN_column_exists`, `apply_vNNN`, `register_migration_vNNN` shape and the unity-namespace comment). Body:

```cpp
Result<void> apply_v060(QSqlDatabase& db) {
    struct Add { const char* table; const char* col; const char* type_default; };
    const Add adds[] = {
        {"portfolios", "sync_source", "TEXT DEFAULT ''"},
        {"portfolios", "synced_at", "TEXT DEFAULT ''"},
        {"portfolios", "sync_error", "TEXT DEFAULT ''"},
        {"portfolio_assets", "has_cost_basis", "INTEGER DEFAULT 1"},
    };
    for (const auto& a : adds) {
        if (!v060_column_exists(db, a.table, a.col)) {
            auto r = v060_sql(db, QString("ALTER TABLE %1 ADD COLUMN %2 %3")
                                      .arg(a.table, a.col, a.type_default).toUtf8().constData());
            if (r.is_err()) return r;
        }
    }
    return v060_sql(db, "CREATE INDEX IF NOT EXISTS idx_portfolios_sync_source "
                        "ON portfolios(sync_source)");
}
```

Register: `MigrationRunner::register_migration({60, "portfolio_account_sync", apply_v060});`. Add the `register_migration_v060();` call and header declaration. Add the file to any explicit source lists.

- [ ] **Step 4: Run test to verify it passes** — rebuild the target and run it. Expected: PASS (2 tests: initTestCase + columnsExist).

- [ ] **Step 5: Neuter-verify** — temporarily comment out the `has_cost_basis` entry in `adds[]`, rebuild, confirm `columnsExist` FAILS, then restore.

- [ ] **Step 6: Commit**

```bash
git add src/storage/sqlite/migrations/v060_portfolio_account_sync.cpp \
        src/storage/sqlite/migrations/MigrationRunner.h \
        src/storage/sqlite/migrations/RegisterAllMigrations.cpp \
        src/CMakeLists.txt tests/CMakeLists.txt tests/tst_portfolio_account_sync_migration.cpp
git commit -m "feat(portfolio): v060 migration — sync_source/synced_at/sync_error + has_cost_basis"
```

---

### Task 2: `has_cost_basis` through the model, repository, and pure `build_summary`

**Files:**
- Modify: `src/screens/portfolio/PortfolioTypes.h` (add `bool has_cost_basis = true;` to `PortfolioAsset` after `exchange`; add `bool has_cost_basis = true;` to `HoldingWithQuote` after `fx_resolved`)
- Modify: `src/storage/repositories/PortfolioRepository.cpp` (persist + read `has_cost_basis`; extend `add_asset` to accept it — see below)
- Modify: `src/storage/repositories/PortfolioRepository.h` (add trailing `bool has_cost_basis = true` param to `add_asset`)
- Modify: `src/services/portfolio/PortfolioSummaryBuild.h` (exclude no-cost-basis holdings from P&L totals; carry the flag)
- Test: `tests/tst_portfolio_summary.cpp` (extend)

**Interfaces:**
- Consumes: `portfolio::build_summary(...)`, `BuiltSummary{ summary, unpriced_count, fx_unresolved_count, snapshot_safe() }` (Task uses existing).
- Produces: `PortfolioAsset.has_cost_basis`, `HoldingWithQuote.has_cost_basis`; `build_summary` excludes `has_cost_basis == false` holdings from `total_cost_basis` and `total_unrealized_pnl` while still counting their `market_value` in `total_market_value`.

- [ ] **Step 1: Write the failing test** — add to `tests/tst_portfolio_summary.cpp`. The `asset()` helper needs a `has_cost_basis` param (default true); thread it into the returned `PortfolioAsset`. New slot:

```cpp
void TestPortfolioSummary::noCostBasisExcludedFromPnlButCountsNav() {
    QVector<PortfolioAsset> assets{asset("AAPL", 10, 100.0), asset("BTC-USD", 1, 0.0)};
    assets[1].has_cost_basis = false; // crypto: qty known, cost basis not
    QHash<QString, QuoteData> quotes;
    quotes.insert("AAPL", quote("AAPL", 150.0, 0.0, 0.0));
    quotes.insert("BTC-USD", quote("BTC-USD", 60000.0, 0.0, 0.0));
    const auto built = build_summary(assets, {}, quotes, stub_sector);
    // NAV includes both: 10*150 + 1*60000 = 61500
    QCOMPARE(built.summary.total_market_value, 61500.0);
    // Cost basis / P&L exclude the no-cost-basis holding: only AAPL (cost 1000).
    QCOMPARE(built.summary.total_cost_basis, 1000.0);
    QCOMPARE(built.summary.total_unrealized_pnl, 500.0); // 1500 - 1000
    // The crypto holding carries the flag for the display badge.
    QVERIFY(!built.summary.holdings[1].has_cost_basis);
    QVERIFY(built.snapshot_safe()); // priced + fx-resolved -> still snapshots
}
```

- [ ] **Step 2: Run test to verify it fails** — `cmake --build /tmp/ot-build-test --target tst_portfolio_summary && /tmp/ot-build-test/tests/tst_portfolio_summary`. Expected: FAIL (`has_cost_basis` not a member / totals wrong).

- [ ] **Step 3: Implement** — add the fields (Task Files), then in `PortfolioSummaryBuild.h` inside the per-asset loop:

```cpp
h.has_cost_basis = asset.has_cost_basis;
```
and change the totals accumulation so cost basis / P&L skip no-cost-basis holdings:
```cpp
total_mv += h.market_value;
if (h.has_cost_basis) {
    total_cost += h.cost_basis;
} else {
    // No cost basis (e.g. crypto exchange balance): market value counts toward
    // NAV, but P&L is undefined — zero it and exclude from the cost/P&L totals.
    h.cost_basis = 0;
    h.unrealized_pnl = 0;
    h.unrealized_pnl_percent = 0;
}
```
Keep `total_unrealized_pnl = total_mv - total_cost` — WRONG for this case (it would fold crypto MV into P&L). Instead accumulate P&L explicitly: add `double total_pnl = 0;`, and only `if (h.has_cost_basis) total_pnl += h.unrealized_pnl;`. Set `summary.total_unrealized_pnl = total_pnl;` and `summary.total_unrealized_pnl_percent = (total_cost > 0) ? (total_pnl / total_cost) * 100.0 : 0;`. Leave `gainers/losers` counting only `has_cost_basis` holdings (`if (!h.has_cost_basis) continue;` before the gain/loss tally).

In `PortfolioRepository.cpp`: persist `has_cost_basis` in the `add_asset` INSERT and read it in `get_assets` (`COALESCE(has_cost_basis, 1)`), mapping to the struct. Extend `add_asset` signature with a trailing `bool has_cost_basis = true`.

- [ ] **Step 4: Run test to verify it passes** — rebuild + run `tst_portfolio_summary`. Expected: all PASS.

- [ ] **Step 5: Neuter-verify** — change the P&L accumulation back to `summary.total_unrealized_pnl = total_mv - total_cost;`, rebuild, confirm `noCostBasisExcludedFromPnlButCountsNav` FAILS (P&L would be 60500), restore.

- [ ] **Step 6: Commit**

```bash
git add src/screens/portfolio/PortfolioTypes.h src/services/portfolio/PortfolioSummaryBuild.h \
        src/storage/repositories/PortfolioRepository.h src/storage/repositories/PortfolioRepository.cpp \
        tests/tst_portfolio_summary.cpp
git commit -m "feat(portfolio): has_cost_basis — exclude no-cost-basis holdings from P&L, keep in NAV"
```

---

### Task 3: Display badge — dash P&L columns only for no-cost-basis holdings

**Files:**
- Modify: `src/screens/portfolio/PortfolioHoldingDisplay.h`
- Test: `tests/tst_portfolio_display.cpp` (extend)

**Interfaces:**
- Consumes: `HoldingWithQuote{ priced, fx_resolved, has_cost_basis, current_price, market_value, unrealized_pnl, ... }`.
- Produces: `price_dependent_cells(h)` — when `has_cost_basis == false` (but priced + fx_resolved), LAST/MKT VAL/CHG% format normally and only `pnl`/`pnl_pct` become the em-dash, with `reason == no_cost_basis_reason()`. `muted` stays false (row not fully muted); a new `bool pnl_muted` flags the P&L-only case.

- [ ] **Step 1: Write the failing test** — extend `tests/tst_portfolio_display.cpp`. The `holding()` helper needs a `has_cost_basis` param (default true). New slot:

```cpp
void TestPortfolioDisplay::noCostBasisDashesPnlOnly() {
    auto h = holding(60000.0, 60000.0, 0.0, 0.0, 1.5, /*priced=*/true, /*fx_resolved=*/true);
    h.has_cost_basis = false;
    const auto c = price_dependent_cells(h);
    const QString dash = unpriced_cell_placeholder();
    QVERIFY(!c.muted);                 // price/market value are real
    QVERIFY(c.pnl_muted);
    QCOMPARE(c.last, QString("60000.00"));
    QCOMPARE(c.market_value, QString("60000.00"));
    QCOMPARE(c.day_change_pct, QString("+1.50%"));
    QCOMPARE(c.pnl, dash);
    QCOMPARE(c.pnl_pct, dash);
    QCOMPARE(c.reason, no_cost_basis_reason());
}
```

- [ ] **Step 2: Run test to verify it fails** — `cmake --build /tmp/ot-build-test --target tst_portfolio_display && /tmp/ot-build-test/tests/tst_portfolio_display`. Expected: FAIL (`pnl_muted`/`no_cost_basis_reason` undefined).

- [ ] **Step 3: Implement** — in `PortfolioHoldingDisplay.h`:
  - Add `inline QString no_cost_basis_reason() { return QStringLiteral("No cost basis from exchange — P&L unavailable"); }`.
  - Add `bool pnl_muted = false;` to `HoldingDisplayCells`.
  - In `price_dependent_cells`, after the existing `!priced || !fx_resolved` early-return that dashes everything, add: format the real cells (unchanged priced path), then:

```cpp
    if (!h.has_cost_basis) {
        const QString dash = unpriced_cell_placeholder();
        c.pnl = dash;
        c.pnl_pct = dash;
        c.pnl_muted = true;
        c.reason = no_cost_basis_reason();
    }
    return c;
```

- [ ] **Step 4: Blotter wiring** — in `src/screens/portfolio/PortfolioBlotter.cpp` populate loop, when `cells.pnl_muted` render the P&L (col 6) and P&L% (col 7) cells in `ui::colors::TEXT_TERTIARY` and set the tooltip on those two items to `cells.reason` (mirror the existing `cells.muted` tooltip loop but scoped to `pnl_item`/`pnl_pct_item`). This is presentation-only; the Task 3 unit test covers the string logic.

- [ ] **Step 5: Run test to verify it passes** — rebuild + run `tst_portfolio_display`. Expected: all PASS.

- [ ] **Step 6: Neuter-verify** — remove the `if (!h.has_cost_basis)` block, rebuild, confirm `noCostBasisDashesPnlOnly` FAILS, restore.

- [ ] **Step 7: Commit**

```bash
git add src/screens/portfolio/PortfolioHoldingDisplay.h src/screens/portfolio/PortfolioBlotter.cpp \
        tests/tst_portfolio_display.cpp
git commit -m "feat(portfolio): badge P&L-only for no-cost-basis (crypto) holdings"
```

---

### Task 4: `SyncedHolding` + pure mirror-reconcile

**Files:**
- Create: `src/services/portfolio/AccountSyncTypes.h` (`SyncedHolding`, `FetchResult`, `MirrorPlan`, `reconcile_mirror`)
- Test: `tests/tst_account_sync_reconcile.cpp`

**Interfaces:**
- Produces:
```cpp
namespace openmarketterminal::portfolio {
struct SyncedHolding {
    QString canonical_symbol;   // yfinance format: "AAPL", "BTC-USD", or "$CASH:USD"
    double quantity = 0;
    double avg_cost = 0;
    bool has_cost_basis = true;
    QString native_currency;    // ISO code, e.g. "USD","CAD","EUR"
    QString broker_symbol;      // native ticker / ccxt pair (e.g. "AAPL","BTC/USD")
    QString exchange;           // exchange code / ccxt id
};
struct FetchResult {           // an account source returns this
    bool ok = false;           // false => fetch failed; DO NOT mirror (keep old holdings)
    QString error;
    QVector<SyncedHolding> holdings;
};
struct MirrorAction { enum Kind { Add, Update, Remove } kind; SyncedHolding holding; QString symbol; };
struct MirrorPlan { QVector<SyncedHolding> to_add; QVector<SyncedHolding> to_update; QStringList to_remove; };
// Pure: compare current assets (from get_assets) to fetched holdings, keyed by
// canonical symbol. Returns the add/update/remove plan. Update = symbol present
// in both AND (quantity or avg_cost or has_cost_basis differs).
MirrorPlan reconcile_mirror(const QVector<PortfolioAsset>& current, const QVector<SyncedHolding>& fetched);
}
```

- [ ] **Step 1: Write the failing test** — `tests/tst_account_sync_reconcile.cpp` (APPLESS, Qt6::Core+Test, include `AccountSyncTypes.h` + `PortfolioTypes.h`). Helper builders like the summary test. Slots + assertions:

```cpp
// new fetched symbol -> add; qty change -> update; vanished -> remove; identical -> no-op.
void addsUpdatesRemoves() {
    QVector<PortfolioAsset> current{mkAsset("AAPL",8,100.0), mkAsset("TSLA",3,200.0)};
    QVector<SyncedHolding> fetched{mkHold("AAPL",10,100.0), mkHold("MSFT",5,50.0)};
    auto plan = reconcile_mirror(current, fetched);
    QCOMPARE(plan.to_add.size(), qsizetype{1});    QCOMPARE(plan.to_add[0].canonical_symbol, QString("MSFT"));
    QCOMPARE(plan.to_update.size(), qsizetype{1}); QCOMPARE(plan.to_update[0].canonical_symbol, QString("AAPL"));
    QCOMPARE(plan.to_remove, QStringList{"TSLA"});
}
void identicalIsNoop() {
    QVector<PortfolioAsset> current{mkAsset("AAPL",10,100.0)};
    QVector<SyncedHolding> fetched{mkHold("AAPL",10,100.0)};
    auto plan = reconcile_mirror(current, fetched);
    QVERIFY(plan.to_add.isEmpty()); QVERIFY(plan.to_update.isEmpty()); QVERIFY(plan.to_remove.isEmpty());
}
void emptyFetchRemovesAll() {   // successful empty fetch mirrors to empty
    QVector<PortfolioAsset> current{mkAsset("AAPL",10,100.0)};
    auto plan = reconcile_mirror(current, {});
    QCOMPARE(plan.to_remove, QStringList{"AAPL"});
}
```
(`mkAsset` sets `symbol/quantity/avg_buy_price`; `mkHold` sets `canonical_symbol/quantity/avg_cost`. Match on `PortfolioAsset.symbol == SyncedHolding.canonical_symbol`.)

- [ ] **Step 2: Run test to verify it fails** — register in `tests/CMakeLists.txt` (Qt6::Core+Test, APPLESS). Build+run; Expected: FAIL (`reconcile_mirror` undefined).

- [ ] **Step 3: Implement `reconcile_mirror`** in `AccountSyncTypes.h`:

```cpp
inline MirrorPlan reconcile_mirror(const QVector<PortfolioAsset>& current,
                                   const QVector<SyncedHolding>& fetched) {
    MirrorPlan plan;
    QHash<QString, const PortfolioAsset*> cur;
    for (const auto& a : current) cur.insert(a.symbol, &a);
    QSet<QString> seen;
    for (const auto& h : fetched) {
        seen.insert(h.canonical_symbol);
        auto it = cur.find(h.canonical_symbol);
        if (it == cur.end()) { plan.to_add.append(h); continue; }
        const auto* a = it.value();
        const bool changed = qAbs(a->quantity - h.quantity) > 1e-9
                          || qAbs(a->avg_buy_price - h.avg_cost) > 1e-9
                          || a->has_cost_basis != h.has_cost_basis;
        if (changed) plan.to_update.append(h);
    }
    for (const auto& a : current)
        if (!seen.contains(a.symbol)) plan.to_remove.append(a.symbol);
    return plan;
}
```

- [ ] **Step 4: Run test to verify it passes** — build+run. Expected: all PASS.

- [ ] **Step 5: Neuter-verify** — delete the `to_remove` loop, rebuild, confirm `addsUpdatesRemoves` + `emptyFetchRemovesAll` FAIL, restore.

- [ ] **Step 6: Commit**

```bash
git add src/services/portfolio/AccountSyncTypes.h tests/tst_account_sync_reconcile.cpp tests/CMakeLists.txt
git commit -m "feat(portfolio): SyncedHolding + pure reconcile_mirror plan"
```

---

### Task 5: Pure aggregate merge (All Accounts)

**Files:**
- Modify: `src/services/portfolio/AccountSyncTypes.h` (add `aggregate_holdings`)
- Test: `tests/tst_account_sync_reconcile.cpp` (extend)

**Interfaces:**
- Produces: `QVector<PortfolioAsset> aggregate_holdings(const QVector<QVector<PortfolioAsset>>& per_portfolio)` — union all; merge duplicate `symbol` across portfolios: summed quantity, quantity-weighted average `avg_buy_price`, `has_cost_basis` = AND across contributors, `sector`/`broker_symbol`/`exchange` from the first contributor. `$CASH:<CCY>` rows merge per-currency (same symbol) by summing quantity (avg 1).

- [ ] **Step 1: Write the failing test**:

```cpp
void aggregateMergesDuplicateSymbols() {
    QVector<PortfolioAsset> alpaca{mkAsset("AAPL",10,100.0)};
    QVector<PortfolioAsset> ibkr{mkAsset("AAPL",5,130.0), mkAsset("BND",4,70.0)};
    auto merged = aggregate_holdings({alpaca, ibkr});
    // AAPL merged: qty 15, weighted avg = (10*100 + 5*130)/15 = 110
    auto aapl = findBySymbol(merged, "AAPL");
    QVERIFY(aapl); QCOMPARE(aapl->quantity, 15.0); QCOMPARE(aapl->avg_buy_price, 110.0);
    QCOMPARE(merged.size(), qsizetype{2}); // AAPL + BND
}
void aggregateAndsCostBasis() {
    QVector<PortfolioAsset> a{mkAsset("BTC-USD",1,0.0)};  a[0].has_cost_basis=false;
    QVector<PortfolioAsset> b{mkAsset("BTC-USD",2,0.0)};  b[0].has_cost_basis=false;
    auto merged = aggregate_holdings({a,b});
    QCOMPARE(merged.size(), qsizetype{1});
    QCOMPARE(merged[0].quantity, 3.0);
    QVERIFY(!merged[0].has_cost_basis);
}
```

- [ ] **Step 2: Run test to verify it fails** — build+run. Expected: FAIL (`aggregate_holdings` undefined).

- [ ] **Step 3: Implement**:

```cpp
inline QVector<PortfolioAsset> aggregate_holdings(const QVector<QVector<PortfolioAsset>>& per_portfolio) {
    QVector<PortfolioAsset> out;
    QHash<QString, int> idx; // symbol -> index in out
    for (const auto& pf : per_portfolio) {
        for (const auto& a : pf) {
            auto it = idx.find(a.symbol);
            if (it == idx.end()) { idx.insert(a.symbol, out.size()); out.append(a); continue; }
            auto& m = out[it.value()];
            const double q = m.quantity + a.quantity;
            m.avg_buy_price = (q > 1e-12) ? (m.avg_buy_price * m.quantity + a.avg_buy_price * a.quantity) / q : 0.0;
            m.quantity = q;
            m.has_cost_basis = m.has_cost_basis && a.has_cost_basis;
        }
    }
    return out;
}
```

- [ ] **Step 4: Run test to verify it passes** — build+run. Expected: PASS.
- [ ] **Step 5: Neuter-verify** — change `m.has_cost_basis && a.has_cost_basis` to `true`, confirm `aggregateAndsCostBasis` FAILS, restore.
- [ ] **Step 6: Commit** — `git commit -m "feat(portfolio): pure aggregate_holdings merge for All Accounts"`

---

### Task 6: `IAccountSource` interface + `EquityAccountSource`

**Files:**
- Create: `src/services/portfolio/IAccountSource.h` (interface)
- Create: `src/services/portfolio/EquityAccountSource.{h,cpp}`
- Test: `tests/tst_equity_account_source.cpp` (with a fake `IBroker`)

**Interfaces:**
```cpp
struct AccountRef { QString sync_source; QString display_name; QString base_currency; };
class IAccountSource {
  public:
    virtual ~IAccountSource() = default;
    virtual QVector<AccountRef> list_accounts() = 0;              // connected accounts of this kind
    virtual portfolio::FetchResult fetch(const AccountRef&) = 0;  // read-only holdings + cash
};
```
- `EquityAccountSource::list_accounts()` maps `trading::AccountManager::instance().list_accounts()` → `AccountRef{ sync_source="broker:"+account_id, display_name, base_currency=account.currency or "USD" }`.
- `EquityAccountSource::fetch(ref)` resolves the broker via `BrokerRegistry`/creds (reuse `try_broker_quotes`'s resolution pattern), calls `get_holdings` → one `SyncedHolding` per `BrokerHolding{symbol,exchange,quantity,avg_price}` (`canonical_symbol=symbol`, `avg_cost=avg_price`, `has_cost_basis=true`, `native_currency=ref.base_currency`, `broker_symbol=symbol`, `exchange`), and `get_funds` → a `$CASH:<base_currency>` holding with `quantity=available_balance`, `avg_cost=1`, `has_cost_basis=false`. Any broker error → `FetchResult{ok=false, error}`.

**To keep `fetch` testable, `EquityAccountSource` takes a broker-resolver callback in its constructor** (`std::function<trading::IBroker*(const QString& account_id, trading::BrokerCredentials&)>`), defaulting to the real `BrokerRegistry` path. Tests inject a fake returning a stub `IBroker`.

- [ ] **Step 1: Write the failing test** — `tests/tst_equity_account_source.cpp`: a `FakeBroker : trading::IBroker` overriding `get_holdings` to return two `BrokerHolding`s and `get_funds` to return `available_balance=2500`, everything else returning empty ok. Inject via the resolver callback. Assert:

```cpp
EquityAccountSource src([&](const QString&, trading::BrokerCredentials&){ return &fake; });
auto r = src.fetch(AccountRef{"broker:acct1","Alpaca","USD"});
QVERIFY(r.ok);
// 2 holdings + 1 cash line
QCOMPARE(r.holdings.size(), qsizetype{3});
auto cash = findHold(r.holdings, "$CASH:USD"); QVERIFY(cash); QCOMPARE(cash->quantity, 2500.0);
QVERIFY(!cash->has_cost_basis);
auto aapl = findHold(r.holdings, "AAPL"); QVERIFY(aapl); QVERIFY(aapl->has_cost_basis); QCOMPARE(aapl->avg_cost, 150.0);
```
And a failure case: fake `get_holdings` returns an error `ApiResponse` → `r.ok == false`, `r.holdings` empty.

- [ ] **Step 2: Run test to verify it fails** — register in CMake (links core lib for `AccountManager`/`BrokerRegistry`/trading types; copy libs from a trading-linked test like `tst_live_trading`). Build+run. Expected: FAIL (`EquityAccountSource` undefined).

- [ ] **Step 3: Implement** `IAccountSource.h` + `EquityAccountSource.{h,cpp}` per the Interfaces block. Read `src/trading/BrokerInterface.h` for exact `get_holdings`/`get_funds`/`ApiResponse` shapes and `src/services/portfolio/PortfolioService_Summary.cpp:108-145` for the broker+creds resolution to reuse.

- [ ] **Step 4: Run test to verify it passes** — build+run. Expected: PASS (success + failure cases).
- [ ] **Step 5: Neuter-verify** — make `fetch` ignore the broker error and always set `ok=true`; confirm the failure-case test FAILS; restore.
- [ ] **Step 6: Commit** — `git commit -m "feat(portfolio): IAccountSource + EquityAccountSource (read-only holdings+cash)"`

---

### Task 7: `AccountSyncService` — orchestrate + mirror-write (equity path end-to-end)

**Files:**
- Create: `src/services/portfolio/AccountSyncService.{h,cpp}`
- Test: `tests/tst_account_sync_service.cpp` (HeadlessRuntime DB + a fake `IAccountSource`)

**Interfaces:**
```cpp
class AccountSyncService : public QObject {
    Q_OBJECT
  public:
    static AccountSyncService& instance();
    void register_source(IAccountSource* src);   // EquityAccountSource, later CryptoAccountSource
    void sync_all();                              // enumerate + sync every connected account
    void sync_account(const AccountRef& ref, IAccountSource* src);
  signals:
    void sync_started();
    void account_synced(QString sync_source, bool ok, QString error);
    void sync_finished();
};
```
- Sync one account: `ensure_portfolio(ref)` — find a `portfolios` row with `sync_source == ref.sync_source`; create one if absent (`create_portfolio(display_name, "", base_currency, "Synced from "+display_name, /*broker_account_id*/ equity? account_id : "")`, then set its `sync_source`). Then `auto res = src->fetch(ref);` — if `!res.ok`, write `sync_error`, emit `account_synced(...,false,...)`, return (DO NOT touch assets). If ok: `auto plan = reconcile_mirror(get_assets(pid), res.holdings);` apply via `PortfolioRepository` (`add_asset` for adds incl. `has_cost_basis`, `update_asset` for updates, `remove_asset` for removes), set `synced_at=now`, clear `sync_error`, emit success.
- Add `PortfolioRepository` helpers if missing: `set_sync_meta(portfolio_id, sync_source, synced_at, sync_error)` and `find_by_sync_source(sync_source) -> optional<Portfolio>`, plus `list_synced() -> QVector<Portfolio>` (for Task 8). Include reading `sync_source/synced_at/sync_error` in `get_portfolio(s)`.

- [ ] **Step 1: Write the failing test** — `tests/tst_account_sync_service.cpp` (HeadlessRuntime bring-up like Task 1). A `FakeSource : IAccountSource` returns one account and a scripted `FetchResult`. Assertions:

```cpp
// initial sync creates a synced portfolio mirroring the fetch
fake.result = ok({hold("AAPL",10,100.0,true), hold("$CASH:USD",2500,1,false)});
svc.sync_account(fake.list_accounts()[0], &fake);
auto pf = PortfolioRepository::instance().find_by_sync_source("broker:acct1");
QVERIFY(pf.has_value());
auto assets = PortfolioRepository::instance().get_assets(pf->id).value();
QCOMPARE(assets.size(), qsizetype{2});
// re-sync with AAPL qty changed + cash gone + new MSFT -> mirror exactly
fake.result = ok({hold("AAPL",12,100.0,true), hold("MSFT",5,50.0,true)});
svc.sync_account(fake.list_accounts()[0], &fake);
assets = PortfolioRepository::instance().get_assets(pf->id).value();
QCOMPARE(assets.size(), qsizetype{2});                 // AAPL + MSFT, cash removed
QCOMPARE(findBySymbol(assets,"AAPL")->quantity, 12.0);
// FAILED fetch must NOT wipe holdings
fake.result = err("rate limited");
svc.sync_account(fake.list_accounts()[0], &fake);
QCOMPARE(PortfolioRepository::instance().get_assets(pf->id).value().size(), qsizetype{2});
QVERIFY(!PortfolioRepository::instance().find_by_sync_source("broker:acct1")->sync_error.isEmpty());
```

- [ ] **Step 2: Run test to verify it fails** — register in CMake (core-lib linked, HeadlessRuntime). Build+run. Expected: FAIL.
- [ ] **Step 3: Implement** the service + repository helpers + the `Portfolio` struct fields (`sync_source`, `synced_at`, `sync_error` — add to `PortfolioTypes.h Portfolio` and read them in `get_portfolio(s)`).
- [ ] **Step 4: Run test to verify it passes** — build+run. Expected: PASS (create, re-mirror, failure-preserves).
- [ ] **Step 5: Neuter-verify** — make the service apply the mirror plan even when `!res.ok`; confirm the "FAILED fetch must NOT wipe" assertion FAILS; restore.
- [ ] **Step 6: Commit** — `git commit -m "feat(portfolio): AccountSyncService — mirror-write synced portfolios (equity e2e)"`

---

### Task 8: All Accounts aggregate in `PortfolioService::load_summary`

**Files:**
- Modify: `src/services/portfolio/PortfolioService.h` (add `static constexpr const char* kAllAccountsId = "__all_accounts__";`)
- Modify: `src/services/portfolio/PortfolioService_Summary.cpp` (`load_summary` special-case)
- Test: covered by an extension to `tst_account_sync_service.cpp` (build the aggregate assets via `aggregate_holdings` over `list_synced()` and assert the union) — the pure merge is already tested in Task 5, so here assert only the wiring: two synced portfolios → All Accounts assets == merged set.

**Interfaces:**
- Consumes: `aggregate_holdings`, `PortfolioRepository::list_synced()`.
- Produces: `load_summary("__all_accounts__")` builds a synthetic `Portfolio{ id=kAllAccountsId, name="All Accounts", currency=<base> }` whose assets are `aggregate_holdings(get_assets for each list_synced())`, then runs the existing quote+FX+build path (`build_summary`). Base currency = the first synced portfolio's currency, else "USD".

- [ ] **Step 1: Write the failing test** — extend `tst_account_sync_service.cpp`: create two synced portfolios (via two fake accounts) holding overlapping symbols; call a new pure/service helper `PortfolioService::aggregate_all_accounts_assets()` (extract the union+merge into a public method returning `QVector<PortfolioAsset>` so it is testable without the async quote fetch). Assert the merged asset set (qty summed, dup merged).

- [ ] **Step 2: Run test to verify it fails** — build+run. Expected: FAIL (`aggregate_all_accounts_assets` undefined).
- [ ] **Step 3: Implement** `aggregate_all_accounts_assets()` (calls `list_synced()` + `get_assets` + `aggregate_holdings`) and special-case `load_summary`/`build_summary` (the private orchestrator) to use it when `portfolio_id == kAllAccountsId`, constructing the synthetic `Portfolio`.
- [ ] **Step 4: Run test to verify it passes** — build+run. Expected: PASS.
- [ ] **Step 5: Neuter-verify** — make `aggregate_all_accounts_assets` return only the first portfolio's assets; confirm the union assertion FAILS; restore.
- [ ] **Step 6: Commit** — `git commit -m "feat(portfolio): All Accounts aggregate summary"`

---

### Task 9: `CryptoAccountSource`

**Files:**
- Create: `src/services/portfolio/CryptoAccountSource.{h,cpp}`
- Test: `tests/tst_crypto_account_source.cpp`

**Interfaces:**
- `list_accounts()` → for each id in `ExchangeSessionManager::supported_exchange_ids()` that has stored creds (`SecureStorage` key `crypto:<id>:apiKey` present), an `AccountRef{ sync_source="crypto:"+id, display_name=id (title-cased), base_currency="USD" }`.
- `fetch(ref)` → call the balance fetch for that exchange and map `{"balances": {CCY: {"free","used","total"}}}`: for each non-zero `total`, if `CCY` is a known fiat (USD, USDT, USDC, EUR, GBP, …) → `$CASH:<CCY>` cash line (`has_cost_basis=false`); else a `SyncedHolding{ canonical_symbol=CCY+"-USD" (normalize BTC→BTC-USD), quantity=total, avg_cost=0, has_cost_basis=false, native_currency="USD", broker_symbol=CCY+"/USD", exchange=id }`. Errors → `FetchResult{ok=false}`.
- Balance fetch behind a constructor callback `std::function<QJsonObject(const QString& exchange_id)>` (default: the real `ExchangeService` per-exchange balance) so tests inject a fake JSON. **Read `src/trading/ExchangeService.h` + `scripts/exchange/exchange_daemon.py:332` for the exact `fetch_balance` JSON shape and how to target a specific exchange (per-session).**

- [ ] **Step 1: Write the failing test** — inject a fake returning `{"balances":{"BTC":{"total":0.3},"ETH":{"total":2},"USD":{"total":1000}}}`. Assert: 3 holdings; `BTC-USD` qty 0.3 `has_cost_basis=false` `broker_symbol="BTC/USD"`; `$CASH:USD` qty 1000; error path → `ok=false`.
- [ ] **Step 2: Run test to verify it fails** — register in CMake. Build+run. Expected: FAIL.
- [ ] **Step 3: Implement** per Interfaces. Fiat set: `{"USD","USDT","USDC","EUR","GBP","CAD","AUD","JPY","CHF"}`.
- [ ] **Step 4: Run test to verify it passes** — build+run. Expected: PASS.
- [ ] **Step 5: Neuter-verify** — treat USD as a coin (not cash); confirm the `$CASH:USD` assertion FAILS; restore.
- [ ] **Step 6: Commit** — `git commit -m "feat(portfolio): CryptoAccountSource — ccxt balances -> holdings + cash"`

---

### Task 10: Register both sources + `$CASH:` handling in the summary pipeline

**Files:**
- Modify: `src/services/portfolio/PortfolioService_Summary.cpp` (FX resolution parses `$CASH:<CCY>`; cash priced=true @1.0)
- Modify: `src/services/portfolio/PortfolioSummaryBuild.h` (recognize `$CASH:` prefix → priced=true, price=1.0 native, never look up a quote)
- Modify: wherever `AccountSyncService` is constructed at startup (register `EquityAccountSource` + `CryptoAccountSource`) — likely `HeadlessRuntime` or the GUI `main`/portfolio screen init; grep for where `PortfolioService::instance()` is first touched.
- Test: `tests/tst_portfolio_summary.cpp` (cash pseudo-holding) + `tst_account_sync_service.cpp` (sources registered)

**Interfaces:**
- Produces: a `$CASH:<CCY>` asset renders as market value = quantity, converted `<CCY>→base` via FX, `priced=true`, `has_cost_basis=false` (P&L dashed), never sent to the quote fetch.

- [ ] **Step 1: Write the failing test** — in `tst_portfolio_summary.cpp`:

```cpp
void cashHoldingIsPricedAtOneInBase() {
    QVector<PortfolioAsset> assets{asset("$CASH:USD", 2500, 1.0)};
    assets[0].has_cost_basis = false;
    QHash<QString, QuoteData> quotes; // NO quote for $CASH:USD
    const auto built = build_summary(assets, {}, quotes, stub_sector); // base USD, rate 1.0
    QCOMPARE(built.unpriced_count, 0);              // cash is NOT unpriced
    QVERIFY(built.snapshot_safe());
    QVERIFY(built.summary.holdings[0].priced);
    QCOMPARE(built.summary.total_market_value, 2500.0);
    QCOMPARE(built.summary.total_unrealized_pnl, 0.0);
}
```

- [ ] **Step 2: Run test to verify it fails** — build+run `tst_portfolio_summary`. Expected: FAIL (`$CASH:USD` treated as unpriced → `unpriced_count == 1`).
- [ ] **Step 3: Implement** — in `build_summary`, before the `quote_map.find`, add: `const bool is_cash = asset.symbol.startsWith("$CASH:");` and if cash, set `h.current_price = 1.0 * rate; h.day_change = 0; h.day_change_percent = 0; h.priced = true;` and skip the quote lookup (`else` branch). (The `rate` already converts native→base; for `$CASH:USD` in a USD portfolio rate=1.0.) Then in `PortfolioService_Summary.cpp` FX resolution, when `asset.symbol.startsWith("$CASH:")`, take the currency from the suffix and compute its `<CCY><base>=X` rate (append the FX pair to the fetch), instead of `currency_code(symbol)`. Register both sources at startup.
- [ ] **Step 4: Run test to verify it passes** — build+run. Expected: PASS.
- [ ] **Step 5: Neuter-verify** — remove the `is_cash` branch; confirm `cashHoldingIsPricedAtOneInBase` FAILS (unpriced_count 1); restore.
- [ ] **Step 6: Commit** — `git commit -m "feat(portfolio): $CASH pseudo-holding pricing + register account sources"`

---

### Task 11: Portfolio screen UI — Sync button, selector entries, read-only guard, badges

**Files:**
- Modify: `src/screens/portfolio/PortfolioScreen.{h,cpp}`, `PortfolioScreen_Layout.cpp`, `PortfolioScreen_Handlers.cpp`, `PortfolioCommandBar.*` (or the toolbar owner — grep the "LOAD DEMO" button to find the toolbar), and the portfolio selector population.
- Test: manual (GUI) — no unit test; the logic pieces (`aggregate`, `reconcile`, `build_summary`, badge) are already unit-tested. Verify via a scripted CLI sync if available, else document the manual check.

**Interfaces:**
- Consumes: `AccountSyncService::instance().sync_all()`, signals `sync_started/account_synced/sync_finished`; `PortfolioService::kAllAccountsId`; `PortfolioRepository::list_synced()` + `synced_at`/`sync_error`.

- [ ] **Step 1** — Add a `⟳ Sync accounts` button to the portfolio toolbar (next to the existing "LOAD DEMO"/command bar). On click → `AccountSyncService::instance().sync_all()`. Show a "last synced Xm ago" label bound to the newest `synced_at`.
- [ ] **Step 2** — Populate the portfolio selector with an `All Accounts` entry (id `kAllAccountsId`) at the top when any synced portfolio exists, then synced portfolios (labelled by account) with a small "synced" chip; a stale one (`sync_error` non-empty) shows a warning chip with the error tooltip.
- [ ] **Step 3** — On selecting a synced portfolio or All Accounts, auto-trigger a sync for it (or `sync_all()` for All Accounts) then `load_summary`. On `sync_finished`, `refresh_summary` the active view.
- [ ] **Step 4** — Read-only guard: when the active portfolio's `sync_source != ''`, disable Add/Sell/Edit/Delete-asset actions (they are account mirrors); keep view/analytics enabled.
- [ ] **Step 5: Build the GUI** — `cmake --build openmarketterminal-qt/build`. Expected: rc 0, links `OpenTerminal`.
- [ ] **Step 6: Manual verification** — launch, click Sync accounts, confirm per-account portfolios + All Accounts appear, foreign holdings convert to base, crypto P&L dashed, cash line present, synced portfolios read-only. Document the result.
- [ ] **Step 7: Commit** — `git commit -m "feat(portfolio): Sync accounts UI — button, All Accounts selector, read-only guard, badges"`

---

## Self-Review notes

- **Spec coverage:** data model (Tasks 7,8), mirror-exactly (Tasks 4,7), crypto MV-only/P&L-excluded (Tasks 2,3,9), trigger button+on-open (Task 11), cash line (Tasks 6,9,10), FX reuse (Tasks 2,10), snapshot gate (Tasks 2,10), read-only + isolation + never-wipe (Tasks 6,7,11), aggregate merge (Tasks 5,8), normalization (Task 9), storage (Task 1). All mapped.
- **Type consistency:** `SyncedHolding`/`FetchResult`/`MirrorPlan`/`AccountRef`/`IAccountSource` defined in Task 4/6 and reused verbatim in 7/9. `has_cost_basis` introduced in Task 2 and consumed everywhere after. `kAllAccountsId` defined in Task 8.
- **Testability:** every non-UI task has a pure or fake-injected unit test with a neuter step. UI (Task 11) is the only manual-verified task, by design — its logic is already covered by unit tests.
