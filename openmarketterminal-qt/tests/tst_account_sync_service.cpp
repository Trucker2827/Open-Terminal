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

    QVector<AccountRef> list_accounts() override { return {AccountRef{sync_source_, "Fake Broker", "USD"}}; }
    portfolio::FetchResult fetch(const AccountRef&) override { return result; }

  private:
    QString sync_source_;
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
};

QTEST_MAIN(TstAccountSyncService)
#include "tst_account_sync_service.moc"
