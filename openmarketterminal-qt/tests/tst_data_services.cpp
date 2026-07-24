// tst_data_services.cpp — Phase 2b Task 1.
//
// register_all_data_services() is the shared helper that wires the 10 headless-registerable
// data-producing services to the DataHub. It is called by HeadlessRuntime (and
// is the no-drift counterpart to register_all_migrations()). The load-bearing
// property is that registration is CHEAP and NON-BLOCKING: it must complete
// fast, never hang, never bind a socket / spawn a process. A blocking connect
// or a subprocess spawn at registration would defeat one-shot-safety for the
// headless CLI, so a watchdog thread aborts the process if the call ever hangs.
//
// This test binary links openterminal_core + Qt6::Core/Network/Sql/Test (NO
// Widgets), so QTEST_MAIN brings up a QCoreApplication. That is exactly the
// headless mode: AgentService's constructor sees a non-QApplication and skips
// its TCP-bridge bootstrap (the guarded heavy side effect), so the whole set
// registers as pure DataHub producer-wiring.

#include <QtTest>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QDateTime>

#include <atomic>
#include <chrono>
#include <thread>

#include "services/DataServices.h"
#include "datahub/DataHub.h"
#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "mcp/TerminalMcpBridge.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"
#include "storage/LocalDataLake.h"

using namespace openmarketterminal;

class TstDataServices : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir home_;
    int producers_after_first_ = -1;

  private slots:
    void initTestCase() {
        QVERIFY(home_.isValid());
        qputenv("HOME", home_.path().toUtf8());
        // Same bring-up ordering as HeadlessRuntime::init(): profile selects the
        // datadir, create the dir tree, register migrations, then open the DB
        // (which runs them). Some service singletons touch settings/cache on
        // construction, so a real open DB keeps the path honest.
        ProfileManager::instance().set_active("default");
        QDir().mkpath(AppPaths::root());
        AppPaths::ensure_all();
        register_all_migrations();
        auto db = Database::instance().open(AppPaths::data() + "/openmarketterminal.db");
        QVERIFY2(db.is_ok(), "Database::open failed");
    }

    // The helper must register all 10 producers FAST and without blocking. A
    // watchdog thread qFatal()s if the call hangs (e.g. a service that opened a
    // socket / spawned a process at registration), so a one-shot-safety
    // regression FAILS loudly instead of hanging CI forever.
    void registers_all_data_services_fast() {
        std::atomic<bool> done{false};
        std::thread watchdog([&done]() {
            for (int i = 0; i < 50 && !done.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!done.load())
                qFatal("register_all_data_services() hung > 5s — registration is "
                       "blocking/connecting/spawning (one-shot-safety regression)");
        });

        QElapsedTimer t;
        t.start();
        services::register_all_data_services();
        const qint64 elapsed = t.elapsed();

        done.store(true);
        watchdog.join();

        QVERIFY2(elapsed < 5000,
                 qPrintable(QString("register_all_data_services took %1ms (blocking/connecting?)")
                                .arg(elapsed)));

        // Pin the registered set at the hub level for the drift tripwire below.
        producers_after_first_ = datahub::DataHub::instance().producer_count();
    }

    // ONE-SHOT-SAFETY: registering all 10 in a headless (QCoreApplication)
    // process must NOT have started AgentService's TerminalMcpBridge — no TCP
    // listen, no bridge.json. Constructing AgentService (which the helper does)
    // would bind the bridge in its ctor were it not for the GUI-mode guard, so
    // this assertion FAILS if that guard is removed/broken. The real-process
    // proof is `pgrep python3|aisstream == 0` after a one-shot CLI; this is its
    // in-test counterpart.
    void agent_bridge_not_started_headless() {
        QVERIFY2(!mcp::TerminalMcpBridge::instance().is_active(),
                 "AgentService bound the MCP bridge in a headless process — the "
                 "one-shot-safety guard in AgentService's constructor is broken");
        QVERIFY2(mcp::TerminalMcpBridge::instance().endpoint().isEmpty(),
                 "MCP bridge has a live endpoint headless — guard broken");
    }

    // Idempotent: calling it again is a no-op (each service guards on its
    // hub_registered_ flag) and must stay fast — re-registration must never
    // double-wire or hang.
    void second_call_is_idempotent_and_fast() {
        QElapsedTimer t;
        t.start();
        services::register_all_data_services();
        QVERIFY2(t.elapsed() < 5000, "second register_all_data_services call was slow");

        // Hub-level idempotency: a second call must NOT re-register producers
        // (each service guards on hub_registered_), so the producer count is
        // unchanged — not doubled.
        QCOMPARE(datahub::DataHub::instance().producer_count(), producers_after_first_);
    }

    // DRIFT TRIPWIRE: a fresh QTEST process has no other producers, so after
    // register_all_data_services() the hub holds EXACTLY the helper's set. This
    // number is the canonical count of headless-registerable data services (10:
    // MarketData, News, Economics, MacroCalendar, Geopolitics, Maritime,
    // MAAnalytics, DBnomics, GovData, Agent — RelationshipMap is GUI-only).
    // Adding/removing a service in register_all_data_services() trips this and
    // forces a conscious update. (It pins the HELPER's set — the headless host's
    // single source of truth — not the GUI's separate main.cpp registration
    // list, which intentionally also registers GUI-only producers.)
    void helper_registers_expected_producer_set() {
        QCOMPARE(producers_after_first_, 10);
    }

    // A representative MarketData peek path is alive after registration: the hub
    // answers a peek for a market-data topic without hanging (no value is
    // published yet, so an invalid QVariant is the expected/healthy result —
    // the point is that the registered-producer peek path works).
    void marketdata_peek_path_works() {
        const QVariant v = datahub::DataHub::instance().peek("market:quote:AAPL");
        QVERIFY2(!v.isValid(),
                 "expected no cached value (nothing published) — peek path returned unexpectedly");
        // And stats() (the public DataHub introspection surface) is callable
        // without hanging after the full set is registered.
        const auto stats = datahub::DataHub::instance().stats();
        Q_UNUSED(stats);
    }

    void data_lake_reports_interrupted_replacement_artifacts_without_deleting_them() {
        auto& lake = storage::LocalDataLake::instance();
        QString error;
        QVERIFY2(lake.append_jsonl(QStringLiteral("raw_ticks"),
                                   QJsonObject{{"source", "test"}, {"price", 1.0}}, &error),
                 qPrintable(error));
        const QString final_path = lake.dataset_file(QStringLiteral("raw_ticks"));
        QFile artifact(final_path + QStringLiteral(".interrupted"));
        QVERIFY(artifact.open(QIODevice::WriteOnly));
        QCOMPARE(artifact.write("unfinished replacement\n"), qint64(23));
        artifact.close();

        const QJsonObject status = lake.status();
        QVERIFY(status.value("orphaned_temp_files").toInt() >= 1);
        QVERIFY(status.value("orphaned_temp_bytes").toString().toLongLong() >= 23);
        QVERIFY(status.value("total_bytes").toString().toLongLong() >
                 status.value("active_bytes").toString().toLongLong());
        QVERIFY(!status.value("storage_warning").toString().isEmpty());
        QVERIFY(QFileInfo::exists(artifact.fileName()));
    }

    void data_lake_cleanup_requires_confirmation_and_preserves_recent_artifacts() {
        auto& lake = storage::LocalDataLake::instance();
        const QString final_path = lake.dataset_file(QStringLiteral("model_outputs"));
        QVERIFY(QDir().mkpath(QFileInfo(final_path).absolutePath()));

        QFile old_artifact(final_path + QStringLiteral(".interrupted-old"));
        QVERIFY(old_artifact.open(QIODevice::WriteOnly));
        const QByteArray old_payload("old interrupted replacement\n");
        QCOMPARE(old_artifact.write(old_payload), qint64(old_payload.size()));
        old_artifact.close();
        QVERIFY(old_artifact.open(QIODevice::ReadWrite));
        QVERIFY(old_artifact.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-600),
                                         QFileDevice::FileModificationTime));
        old_artifact.close();

        QFile recent_artifact(final_path + QStringLiteral(".interrupted-recent"));
        QVERIFY(recent_artifact.open(QIODevice::WriteOnly));
        const QByteArray recent_payload("recent interrupted replacement\n");
        QCOMPARE(recent_artifact.write(recent_payload), qint64(recent_payload.size()));
        recent_artifact.close();

        QString error;
        const QJsonObject preview = lake.cleanup_interrupted_artifacts(false, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(preview.value("candidate_files").toInt() >= 1);
        QCOMPARE(preview.value("deleted_files").toInt(), 0);
        QVERIFY(QFileInfo::exists(old_artifact.fileName()));
        QVERIFY(QFileInfo::exists(recent_artifact.fileName()));

        const QJsonObject cleaned = lake.cleanup_interrupted_artifacts(true, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(cleaned.value("deleted_files").toInt() >= 1);
        QVERIFY(!QFileInfo::exists(old_artifact.fileName()));
        QVERIFY(QFileInfo::exists(recent_artifact.fileName()));
    }
};

QTEST_MAIN(TstDataServices)
#include "tst_data_services.moc"
