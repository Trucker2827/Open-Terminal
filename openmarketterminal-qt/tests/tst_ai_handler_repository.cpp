// tst_ai_handler_repository.cpp — AiHandlerRepository: CRUD over migration
// v063's ai_handler table (named AI-handler configs, disarmed by default).
//
// DB bring-up mirrors tst_sandbox_registry.cpp's open_profile_database_for_test():
// select the "default" profile, create its datadir tree, register migrations,
// then open the DB (which runs them). Do not invent a new bootstrap.

#include <QtTest>
#include <QTemporaryDir>
#include <QDir>

#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "storage/repositories/AiHandlerRepository.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"

using namespace openmarketterminal;

namespace {

bool open_profile_database_for_test() {
    ProfileManager::instance().set_active("default");
    QDir().mkpath(AppPaths::root());
    AppPaths::ensure_all();
    register_all_migrations();
    auto db = Database::instance().open(AppPaths::data() + "/openmarketterminal.db");
    return db.is_ok();
}

} // namespace

class TstAiHandlerRepository : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        static QTemporaryDir home;
        QVERIFY(home.isValid());
        qputenv("HOME", home.path().toUtf8());
        QVERIFY(open_profile_database_for_test());
    }

    void save_get_list_enable_remove_roundtrip() {
        AiHandler h;
        h.name = QStringLiteral("crypto-scout");
        h.strategy = QStringLiteral("claude");
        h.provider = QStringLiteral("anthropic");
        h.symbols = QStringLiteral("BTC-USD,ETH-USD");
        h.market = QStringLiteral("crypto");
        h.interval_sec = 30;
        h.allowed_venues = QStringLiteral("coinbase_advanced");
        h.max_notional = 250.0;
        h.max_position = 100.0;
        h.notes = QStringLiteral("paper only");
        auto saved = AiHandlerRepository::instance().save(h);
        QVERIFY2(saved.is_ok(), saved.is_err() ? saved.error().c_str() : "");

        auto got = AiHandlerRepository::instance().get(QStringLiteral("crypto-scout"));
        QVERIFY(got.is_ok());
        QCOMPARE(got.value().strategy, QStringLiteral("claude"));
        QCOMPARE(got.value().symbols, QStringLiteral("BTC-USD,ETH-USD"));
        QCOMPARE(got.value().market, QStringLiteral("crypto"));
        QCOMPARE(got.value().interval_sec, 30);
        QVERIFY(qAbs(got.value().max_notional - 250.0) < 1e-9);
        QVERIFY(!got.value().enabled); // disarmed/disabled by default

        auto en = AiHandlerRepository::instance().set_enabled(QStringLiteral("crypto-scout"), true);
        QVERIFY(en.is_ok());
        QVERIFY(AiHandlerRepository::instance().get(QStringLiteral("crypto-scout")).value().enabled);

        QCOMPARE(AiHandlerRepository::instance().list().value().size(), 1);
        QVERIFY(AiHandlerRepository::instance().remove(QStringLiteral("crypto-scout")).is_ok());
        QCOMPARE(AiHandlerRepository::instance().list().value().size(), 0);
    }
};
QTEST_GUILESS_MAIN(TstAiHandlerRepository)
#include "tst_ai_handler_repository.moc"
