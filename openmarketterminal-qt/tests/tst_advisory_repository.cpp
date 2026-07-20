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
#include "services/edge_radar/AdvisoryChallengeRepository.h"
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
};

QTEST_MAIN(TstAdvisoryRepository)
#include "tst_advisory_repository.moc"
