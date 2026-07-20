// tst_advisory_repository.cpp — edge_advisory_challenge table (migration v067).
//
// DB bring-up mirrors tst_ai_handler_repository.cpp's open_profile_database_for_test():
// select the "default" profile, create its datadir tree, register migrations,
// then open the DB (which runs them). Do not invent a new bootstrap.

#include <QtTest>
#include <QTemporaryDir>
#include <QDir>

#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
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

class TstAdvisoryRepository : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        static QTemporaryDir home;
        QVERIFY(home.isValid());
        qputenv("HOME", home.path().toUtf8());
        QVERIFY(open_profile_database_for_test());
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
