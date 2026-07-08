// tst_account_sync_service.cpp — Portfolio Account Sync, Task 7:
// AccountSyncService::sync_account() over a fake IAccountSource. Covers:
//   1. Initial sync creates a synced portfolio mirroring the fetch.
//   2. Re-sync mirrors EXACTLY (qty updated, vanished removed, new added).
//   3. A FAILED fetch does NOT wipe holdings and records sync_error.
//   4. A has_cost_basis-ONLY diff (qty/price unchanged) still converges —
//      regression test for the update_asset(has_cost_basis) convergence fix
//      (reconcile_mirror's update predicate includes has_cost_basis; without
//      the fix, update_asset couldn't write it and the symbol would be
//      flagged to_update forever).
//
// Bring-up mirrors tst_pm_paper.cpp / tst_portfolio_account_sync_migration.cpp:
// a HeadlessRuntime init("default") registers all migrations (incl. v060)
// and opens the DB under a QTemporaryDir HOME before any repo call.

#include <QtTest>
#include <QTemporaryDir>

#include "core/headless/HeadlessRuntime.h"
#include "services/portfolio/AccountSyncService.h"
#include "services/portfolio/PortfolioService.h"
#include "storage/repositories/PortfolioRepository.h"

using namespace openmarketterminal;
using namespace openmarketterminal::services;

namespace {

portfolio::SyncedHolding hold(const QString& symbol, double qty, double avg_cost, bool has_cost_basis) {
    portfolio::SyncedHolding h;
    h.canonical_symbol = symbol;
    h.quantity = qty;
    h.avg_cost = avg_cost;
    h.has_cost_basis = has_cost_basis;
    h.native_currency = "USD";
    h.broker_symbol = symbol;
    h.exchange = "";
    return h;
}

portfolio::FetchResult ok(const QVector<portfolio::SyncedHolding>& holdings) {
    portfolio::FetchResult r;
    r.ok = true;
    r.holdings = holdings;
    return r;
}

portfolio::SyncedTransaction tx(const QString& external_id, const QString& symbol, const QString& type, double qty,
                                double price, const QString& date) {
    portfolio::SyncedTransaction t;
    t.external_id = external_id;
    t.symbol = symbol;
    t.type = type;
    t.quantity = qty;
    t.price = price;
    t.date = date;
    return t;
}

portfolio::FetchResult err(const QString& message) {
    portfolio::FetchResult r;
    r.ok = false;
    r.error = message;
    return r;
}

const portfolio::PortfolioAsset* findBySymbol(const QVector<portfolio::PortfolioAsset>& assets, const QString& symbol) {
    for (const auto& a : assets)
        if (a.symbol == symbol)
            return &a;
    return nullptr;
}

// Scriptable fake: list_accounts() always returns one account (sync_source
// configurable so distinct tests can use isolated keys in the shared DB);
// fetch() returns whatever `result` was last set to, regardless of `ref`.
class FakeSource : public IAccountSource {
  public:
    explicit FakeSource(QString sync_source = "broker:acct1") : sync_source_(std::move(sync_source)) {}

    portfolio::FetchResult result;
    QVector<portfolio::SyncedTransaction> transactions; // Task: trade-history sync
    int fetch_transactions_calls = 0;

    QVector<AccountRef> list_accounts() override { return {AccountRef{sync_source_, "Fake Broker", "USD"}}; }
    portfolio::FetchResult fetch(const AccountRef&) override { return result; }
    QVector<portfolio::SyncedTransaction> fetch_transactions(const AccountRef&,
                                                              const QVector<portfolio::SyncedHolding>&) override {
        ++fetch_transactions_calls;
        return transactions;
    }

  private:
    QString sync_source_;
};

// Scriptable fake for the sync_all() re-entrancy-guard test (Task 11): on its
// FIRST invocation, fetch() re-entrantly calls
// AccountSyncService::instance().sync_all() — guarded by `reentered_` so it
// only re-enters once (avoids infinite recursion). This mimics the real
// broker path: EquityAccountSource::fetch() -> BrokerHttp runs a nested
// QEventLoop::exec() while waiting on the network, which keeps delivering UI
// events, so a click on the portfolio selector mid-sweep can call
// sync_all() again before the first sweep finishes.
//
// Without the re-entrancy guard, that nested sync_all() call runs a second
// full sweep and invokes fetch() again for this account -> fetch_count
// reaches 2 (observed during RED verification: guard removed, recursion is
// bounded by `reentered_` but still produces a second sweep). With the
// guard, the nested sync_all() sees syncing_==true and no-ops -> fetch_count
// stays 1.
class FakeReentrantSource : public IAccountSource {
  public:
    int fetch_count = 0;

    QVector<AccountRef> list_accounts() override {
        return {AccountRef{"broker:reentrant", "Reentrant Fake", "USD"}};
    }

    portfolio::FetchResult fetch(const AccountRef&) override {
        ++fetch_count;
        if (!reentered_) {
            reentered_ = true;
            AccountSyncService::instance().sync_all();
        }
        portfolio::FetchResult r;
        r.ok = true;
        r.holdings = {hold("REENTRANT_SYM", 1, 1.0, true)};
        return r;
    }

  private:
    bool reentered_ = false;
};

} // namespace

class TstAccountSyncService : public QObject {
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

    void sync_account_mirrors_exactly_and_never_wipes_on_failure() {
        auto& svc = AccountSyncService::instance();
        auto& repo = PortfolioRepository::instance();
        FakeSource fake;

        // ── 1. Initial sync creates a synced portfolio mirroring the fetch ──
        fake.result = ok({hold("AAPL", 10, 100.0, true), hold("$CASH:USD", 2500, 1, false)});
        svc.sync_account(fake.list_accounts()[0], &fake);

        auto pf = repo.find_by_sync_source("broker:acct1");
        QVERIFY(pf.has_value());
        QCOMPARE(pf->sync_source, QStringLiteral("broker:acct1"));
        QVERIFY(pf->sync_error.isEmpty());
        QVERIFY(!pf->synced_at.isEmpty());

        auto assets = repo.get_assets(pf->id).value();
        QCOMPARE(assets.size(), qsizetype{2});
        QVERIFY(findBySymbol(assets, "AAPL"));
        QVERIFY(findBySymbol(assets, "$CASH:USD"));

        // ── 2. Re-sync: AAPL qty changed + cash gone + new MSFT -> mirror exactly ──
        fake.result = ok({hold("AAPL", 12, 100.0, true), hold("MSFT", 5, 50.0, true)});
        svc.sync_account(fake.list_accounts()[0], &fake);

        assets = repo.get_assets(pf->id).value();
        QCOMPARE(assets.size(), qsizetype{2}); // AAPL + MSFT, cash removed
        QVERIFY(!findBySymbol(assets, "$CASH:USD"));
        auto* aapl = findBySymbol(assets, "AAPL");
        QVERIFY(aapl);
        QCOMPARE(aapl->quantity, 12.0);
        QVERIFY(findBySymbol(assets, "MSFT"));

        // ── 3. A FAILED fetch must NOT wipe holdings ──
        fake.result = err("rate limited");
        svc.sync_account(fake.list_accounts()[0], &fake);

        QCOMPARE(repo.get_assets(pf->id).value().size(), qsizetype{2});
        auto pf_after_fail = repo.find_by_sync_source("broker:acct1");
        QVERIFY(pf_after_fail.has_value());
        QVERIFY(!pf_after_fail->sync_error.isEmpty());
        QCOMPARE(pf_after_fail->sync_error, QStringLiteral("rate limited"));
    }

    // Regression test for the update_asset(has_cost_basis) convergence fix:
    // a re-sync where ONLY has_cost_basis flips (qty/price unchanged) must
    // still persist. Uses a distinct sync_source so it's isolated from the
    // first test's rows in the shared DB.
    void resync_converges_has_cost_basis_only_diff() {
        auto& svc = AccountSyncService::instance();
        auto& repo = PortfolioRepository::instance();
        FakeSource fake("broker:acct2");

        fake.result = ok({hold("SYM", 10, 100.0, true)});
        svc.sync_account(fake.list_accounts()[0], &fake);

        auto pf = repo.find_by_sync_source("broker:acct2");
        QVERIFY(pf.has_value());
        auto assets = repo.get_assets(pf->id).value();
        auto* sym = findBySymbol(assets, "SYM");
        QVERIFY(sym);
        QVERIFY(sym->has_cost_basis);

        // Same qty/price, only has_cost_basis flips false -> reconcile_mirror
        // must flag it to_update, and update_asset must persist the flip.
        fake.result = ok({hold("SYM", 10, 100.0, false)});
        svc.sync_account(fake.list_accounts()[0], &fake);

        assets = repo.get_assets(pf->id).value();
        QCOMPARE(assets.size(), qsizetype{1});
        sym = findBySymbol(assets, "SYM");
        QVERIFY(sym);
        QCOMPARE(sym->quantity, 10.0);
        QVERIFY(!sym->has_cost_basis);
    }

    // Trade-history sync: a successful sync_account additionally imports
    // src->fetch_transactions(ref, res.holdings) via
    // PortfolioRepository::import_transaction, and re-syncing the same fills
    // (same external_id) must NOT duplicate — the whole point of v061's
    // idx_ptx_external + INSERT OR IGNORE.
    void sync_account_imports_transactions_on_success_and_dedups_on_resync() {
        auto& svc = AccountSyncService::instance();
        auto& repo = PortfolioRepository::instance();
        FakeSource fake("broker:acctTx");

        fake.result = ok({hold("AAPL", 10, 100.0, true)});
        fake.transactions = {tx("broker:order-1", "AAPL", "BUY", 10, 100.0, "2026-01-01")};
        svc.sync_account(fake.list_accounts()[0], &fake);

        auto pf = repo.find_by_sync_source("broker:acctTx");
        QVERIFY(pf.has_value());
        auto txs = repo.get_transactions(pf->id);
        QVERIFY(txs.is_ok());
        QCOMPARE(txs.value().size(), qsizetype{1});
        QCOMPARE(txs.value().first().symbol, QStringLiteral("AAPL"));
        QCOMPARE(txs.value().first().quantity, 10.0);

        // Re-sync with the SAME transaction (same external_id) -> dedup, no
        // second row.
        svc.sync_account(fake.list_accounts()[0], &fake);
        txs = repo.get_transactions(pf->id);
        QVERIFY(txs.is_ok());
        QCOMPARE(txs.value().size(), qsizetype{1});
    }

    // A FAILED fetch must never reach fetch_transactions/import_transaction —
    // trade-history import is strictly additive to the ok path.
    void sync_account_skips_transactions_on_failed_fetch() {
        auto& svc = AccountSyncService::instance();
        FakeSource fake("broker:acctTxFail");

        fake.result = err("rate limited");
        fake.transactions = {tx("broker:order-9", "AAPL", "BUY", 1, 1.0, "2026-01-01")};
        svc.sync_account(fake.list_accounts()[0], &fake);

        QCOMPARE(fake.fetch_transactions_calls, 0);
    }

    // Task 8: PortfolioService::aggregate_all_accounts_assets() unions holdings
    // across every synced portfolio (via list_synced() + get_assets() +
    // aggregate_holdings) — wiring only, the merge math itself is covered by
    // Task 5's pure aggregate_holdings tests. Two synced portfolios with an
    // overlapping symbol (AAPL) and one distinct symbol each.
    void all_accounts_aggregate_unions_synced_portfolios() {
        auto& svc = AccountSyncService::instance();
        auto& repo = PortfolioRepository::instance();
        FakeSource fakeA("broker:acctA");
        FakeSource fakeB("broker:acctB");

        // Symbols distinct from every other slot's fixtures in this shared-DB
        // test class (AAPL/MSFT/SYM/$CASH:USD are already used elsewhere and
        // would silently accumulate into this aggregate across test runs).
        fakeA.result = ok({hold("GOOG", 10, 100.0, true), hold("NFLX", 3, 200.0, true)});
        svc.sync_account(fakeA.list_accounts()[0], &fakeA);

        fakeB.result = ok({hold("GOOG", 5, 120.0, true), hold("IBM", 8, 50.0, true)});
        svc.sync_account(fakeB.list_accounts()[0], &fakeB);

        QVERIFY(repo.find_by_sync_source("broker:acctA").has_value());
        QVERIFY(repo.find_by_sync_source("broker:acctB").has_value());

        auto& psvc = PortfolioService::instance();
        auto assets = psvc.aggregate_all_accounts_assets();

        auto* goog = findBySymbol(assets, "GOOG");
        QVERIFY(goog);
        QCOMPARE(goog->quantity, 15.0); // 10 (acctA) + 5 (acctB) summed
        QVERIFY(qAbs(goog->avg_buy_price - (1600.0 / 15.0)) < 1e-6); // qty-weighted avg cost

        QVERIFY(findBySymbol(assets, "NFLX")); // distinct to acctA
        QVERIFY(findBySymbol(assets, "IBM"));  // distinct to acctB
    }

    // Task 11: sync_all() must guard against re-entrant/overlapping sweeps.
    // The live broker fetch path runs a nested QEventLoop::exec() while
    // waiting on the network, which keeps delivering UI events — a click on
    // the portfolio selector mid-sweep can re-enter sync_all() before the
    // first sweep finishes. FakeReentrantSource.fetch() reproduces exactly
    // that: it calls sync_all() again from inside the first sweep's fetch
    // call. This slot is placed LAST because it registers a stack-local
    // source into the process-lifetime AccountSyncService singleton, and
    // sync_all() is not exercised by any other slot in this file.
    void sync_all_guards_against_reentrant_overlapping_sweep() {
        auto& svc = AccountSyncService::instance();
        FakeReentrantSource fake;
        svc.register_source(&fake);

        svc.sync_all();

        // GREEN (guard present): the nested sync_all() call sees
        // syncing_==true and no-ops, so fetch() ran exactly once.
        // RED (guard removed): the nested sync_all() call runs a second full
        // sweep, invoking fetch() a second time -> fetch_count == 2.
        QCOMPARE(fake.fetch_count, 1);
    }
};

QTEST_MAIN(TstAccountSyncService)
#include "tst_account_sync_service.moc"
