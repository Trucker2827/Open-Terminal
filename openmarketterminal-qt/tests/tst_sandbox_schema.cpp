// tst_sandbox_schema.cpp — Strategy Sandbox migration v056.
//
// Proves the four sandbox tables (sandbox_strategy, sandbox_position,
// sandbox_fill, sandbox_score) exist with every load-bearing column verbatim,
// and that sandbox_position.decision_id is UNIQUE (structural dedup — later
// tasks rely on a duplicate decision_id insert failing rather than silently
// creating a second paper position for the same journal decision).
//
// DB bring-up mirrors tst_data_services.cpp's initTestCase(): select the
// "default" profile, create its datadir tree, register migrations, then open
// the DB (which runs them) — the same ordering HeadlessRuntime::init() uses.

#include <QtTest>
#include <QTemporaryDir>
#include <QDir>

#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"

using namespace openmarketterminal;

namespace {

// Local helper — copies the exact DB-open incantation from
// tst_data_services.cpp's initTestCase(). Do not invent a new bootstrap.
bool open_profile_database_for_test() {
    ProfileManager::instance().set_active("default");
    QDir().mkpath(AppPaths::root());
    AppPaths::ensure_all();
    register_all_migrations();
    auto db = Database::instance().open(AppPaths::data() + "/openmarketterminal.db");
    return db.is_ok();
}

} // namespace

class TstSandboxSchema : public QObject {
    Q_OBJECT
  private slots:
    void tables_and_columns_exist() {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        qputenv("HOME", home.path().toUtf8());
        // open the profile DB through the normal migration path
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
