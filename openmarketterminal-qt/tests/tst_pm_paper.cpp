// tst_pm_paper.cpp — storage + constitution readers for the prediction-market
// paper book (Phase B, Task 1). Verifies the v050 migration creates the
// pm_paper_account / pm_paper_positions tables, that PmPaperRepository
// round-trips cash + positions, and that the SettingsGate venue helpers read
// cli.allowed_venues correctly.
//
// Bring-up mirrors tst_order_flow.cpp: a HeadlessRuntime init("default")
// registers all migrations (incl. v050) and opens the DB under a QTemporaryDir
// HOME before any repo call.

#include <QtTest>
#include <QTemporaryDir>

#include "core/headless/HeadlessRuntime.h"
#include "mcp/tools/SettingsGate.h"
#include "storage/repositories/PmPaperRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/sqlite/Database.h"

using namespace openmarketterminal;
using namespace openmarketterminal::headless;

class TstPmPaper : public QObject {
    Q_OBJECT
  private:
    QTemporaryDir home_;
    HeadlessRuntime rt_;

  private slots:
    void initTestCase() {
        QVERIFY(home_.isValid());
        qputenv("HOME", home_.path().toUtf8());
        auto r = rt_.init("default");
        QVERIFY2(r.ok, qPrintable(r.error));
    }

    // The v050 migration must have created both PM paper tables.
    void tables_exist() {
        for (const QString& tbl : {QStringLiteral("pm_paper_account"), QStringLiteral("pm_paper_positions")}) {
            auto r = Database::instance().execute(
                "SELECT name FROM sqlite_master WHERE type='table' AND name=?", {tbl});
            QVERIFY2(r.is_ok(), qPrintable("sqlite_master query failed for " + tbl));
            QVERIFY2(r.value().next(), qPrintable("table missing: " + tbl));
        }
    }

    // cash() seeds the account to 100000 on first read; adjust_cash(-500) debits.
    void cash_seeds_and_adjusts() {
        auto& repo = PmPaperRepository::instance();

        auto c = repo.cash();
        QVERIFY2(c.is_ok(), qPrintable("cash() failed: " + QString::fromStdString(c.is_err() ? c.error() : "")));
        QCOMPARE(c.value(), 100000.0);

        auto adj = repo.adjust_cash(-500.0);
        QVERIFY2(adj.is_ok(), "adjust_cash failed");

        auto c2 = repo.cash();
        QVERIFY2(c2.is_ok(), "cash() after adjust failed");
        QCOMPARE(c2.value(), 99500.0);
    }

    // insert_open → get_open round-trips every field; open_stake_in_category sums
    // cost_basis across all OPEN rows in a category.
    void position_round_trip_and_category_stake() {
        auto& repo = PmPaperRepository::instance();

        PmPosition p;
        p.venue = "polymarket";
        p.market_id = "mkt-1";
        p.asset_id = "asset-yes-1";
        p.outcome = "YES";
        p.category = "elections";
        p.contracts = 100;
        p.avg_price = 0.42;
        p.cost_basis = 42.0;
        p.opened_at = "2026-06-15T10:00:00Z";
        p.status = "open";

        auto ins = repo.insert_open(p);
        QVERIFY2(ins.is_ok(), qPrintable("insert_open failed: " + QString::fromStdString(ins.is_err() ? ins.error() : "")));
        QVERIFY2(ins.value() > 0, "insert_open should return a positive row id");

        auto got = repo.get_open(p.venue, p.asset_id);
        QVERIFY2(got.is_ok(), "get_open failed");
        QVERIFY2(got.value().has_value(), "get_open should find the just-inserted open position");
        const PmPosition& g = got.value().value();
        QCOMPARE(g.id, ins.value());
        QCOMPARE(g.venue, p.venue);
        QCOMPARE(g.market_id, p.market_id);
        QCOMPARE(g.asset_id, p.asset_id);
        QCOMPARE(g.outcome, p.outcome);
        QCOMPARE(g.category, p.category);
        QCOMPARE(g.contracts, p.contracts);
        QCOMPARE(g.avg_price, p.avg_price);
        QCOMPARE(g.cost_basis, p.cost_basis);
        QCOMPARE(g.opened_at, p.opened_at);
        QCOMPARE(g.status, p.status);

        // get_open on an unknown asset returns nullopt (not an error).
        auto none = repo.get_open(p.venue, "no-such-asset");
        QVERIFY2(none.is_ok(), "get_open(unknown) should be ok");
        QVERIFY2(!none.value().has_value(), "get_open(unknown) should be nullopt");

        // A second OPEN position in the same category: stake sums both cost_basis.
        PmPosition p2 = p;
        p2.asset_id = "asset-no-1";
        p2.outcome = "NO";
        p2.cost_basis = 58.0;
        auto ins2 = repo.insert_open(p2);
        QVERIFY2(ins2.is_ok(), "second insert_open failed");

        auto stake = repo.open_stake_in_category("elections");
        QVERIFY2(stake.is_ok(), "open_stake_in_category failed");
        QCOMPARE(stake.value(), 100.0); // 42 + 58

        // A category with no open rows sums to 0.
        auto empty = repo.open_stake_in_category("sports");
        QVERIFY2(empty.is_ok(), "open_stake_in_category(empty) failed");
        QCOMPARE(empty.value(), 0.0);
    }

    // The venue constitution reader: default-deny, then allow after the human
    // (modelled by a direct repo write) names venues in cli.allowed_venues.
    void venue_gate_default_deny_then_allow() {
        // Default (unset) — no venue is allowed.
        QVERIFY2(!mcp::cli_venue_allowed("polymarket"), "polymarket must be denied by default");

        auto set = SettingsRepository::instance().set("cli.allowed_venues", "polymarket,kalshi", "cli");
        QVERIFY2(set.is_ok(), "set cli.allowed_venues failed");

        // Case-insensitive + whitespace-tolerant membership.
        QVERIFY2(mcp::cli_venue_allowed("polymarket"), "polymarket must be allowed");
        QVERIFY2(mcp::cli_venue_allowed("KALSHI"), "KALSHI (case-insensitive) must be allowed");
        QVERIFY2(mcp::cli_venue_allowed(" Polymarket "), "' Polymarket ' (trimmed) must be allowed");
        QVERIFY2(!mcp::cli_venue_allowed("foo"), "foo must be denied");

        const QStringList venues = mcp::cli_allowed_venues();
        QCOMPARE(venues.size(), 2);
        QVERIFY2(venues.contains("polymarket") && venues.contains("kalshi"),
                 "allowed venues must be normalised to lowercase");
    }

    void cleanupTestCase() { rt_.shutdown(); }
};

QTEST_MAIN(TstPmPaper)
#include "tst_pm_paper.moc"
