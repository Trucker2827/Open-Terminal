// tst_headless_runtime.cpp — e2e bring-up of HeadlessRuntime under QCoreApplication.
//
// With a QTemporaryDir HOME (so DB/cache land in a throwaway datadir), init()
// must succeed and real core tools must round-trip through the synchronous
// call_tool() path (which dispatches the handler on a worker thread while
// pumping a nested QEventLoop under the QTEST_MAIN app).
//
// IMPORTANT — singleton lifetime: Database/McpProvider/SecureStorage are
// process-global singletons that live for the whole test binary. init() opens
// the DB and registers core tools exactly once, so we init ONCE in
// initTestCase() against a single member HOME and assert against the shared
// runtime in each slot. Re-initing per slot would re-open an already-open DB
// and double-register tools.
//
// What each case proves:
//   • get_app_info          — full runtime path works (pure-compute, no network).
//   • settings table + tool — FIX 1: migrations actually ran (direct table probe
//                             is the discriminator; a tool round-trip backs it).
//   • get_quote returns     — FIX 2: a blocking sync handler (run_async_wait →
//                             main-thread service) does NOT deadlock. A watchdog
//                             thread aborts the process if it hangs (regression).
//   • set_setting denied    — FIX 3: the McpProvider auth-checker blocks a
//                             destructive tool headless.

#include <QtTest>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "core/headless/HeadlessRuntime.h"
#include "mcp/McpProvider.h"
#include "mcp/McpTypes.h"
#include "storage/cache/CacheManager.h"
#include "storage/sqlite/Database.h"
#include "storage/repositories/SettingsRepository.h"

using namespace openmarketterminal::headless;

class TstHeadlessRuntime : public QObject {
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

    // A pure-compute / no-network core tool round-trips through the worker-thread
    // dispatch path.
    void get_app_info_round_trips() {
        auto res = rt_.call_tool("get_app_info", {});
        QVERIFY2(res.success, qPrintable(res.error));
    }

    // FIX 1 — migrations ran. The direct table probe is the rock-solid
    // discriminator: without register_all_migrations() the `settings` table does
    // not exist. The tool round-trip (seed via repo on the main connection, read
    // back via the get_setting tool on a worker connection) backs it up.
    void migrations_ran_db_schema_present() {
        // Discriminator: the migrated schema must contain a real app table.
        QStringList tables = openmarketterminal::Database::instance().connection().tables();
        QVERIFY2(tables.contains("settings"),
                 qPrintable("settings table missing — migrations did not run; tables: "
                            + tables.join(", ")));

        // Round-trip: seed a row (writes to the migrated table; fails without it),
        // then read it back through a DB-backed core tool.
        auto seed = openmarketterminal::SettingsRepository::instance().set(
            "tst_headless_key", "tst_headless_val", "general");
        QVERIFY2(seed.is_ok(), "SettingsRepository::set failed — settings table absent");

        auto res = rt_.call_tool("get_setting", QJsonObject{{"key", "tst_headless_key"}});
        QVERIFY2(res.success, qPrintable(res.error));
    }

    // FIX 2 (necessity) — call_tool MUST run the handler off the main thread.
    // This is the property that prevents the deadlock: blocking handlers (e.g.
    // get_quote → run_async_wait) marshal to a main-thread service and wait, so
    // the handler itself must not occupy the main thread. We register a probe
    // tool that records the thread it runs on and assert it is NOT the main
    // (test) thread. This fails deterministically if call_tool is reverted to a
    // main-thread dispatch — unlike the cached get_quote case below, which would
    // short-circuit and pass even on the main thread.
    void call_tool_dispatches_off_main_thread() {
        QThread* const main_thread = QThread::currentThread();
        auto handler_thread = std::make_shared<std::atomic<QThread*>>(nullptr);

        openmarketterminal::mcp::ToolDef probe;
        probe.name = "tst_thread_probe";
        probe.description = "test-only: records the thread its handler runs on";
        probe.category = "system";
        probe.handler =
            [handler_thread](const QJsonObject&) -> openmarketterminal::mcp::ToolResult {
            handler_thread->store(QThread::currentThread());
            return openmarketterminal::mcp::ToolResult::ok("ok");
        };
        openmarketterminal::mcp::McpProvider::instance().register_tool(std::move(probe));

        auto res = rt_.call_tool("tst_thread_probe", {});
        openmarketterminal::mcp::McpProvider::instance().unregister_tool("tst_thread_probe");

        QVERIFY2(res.success, qPrintable(res.error));
        QVERIFY2(handler_thread->load() != nullptr, "probe handler never ran");
        QVERIFY2(handler_thread->load() != main_thread,
                 "FIX 2 regression: tool handler ran on the main thread — call_tool "
                 "must dispatch off-main or blocking handlers (get_quote) deadlock");
    }

    // FIX 2 (integration) — a blocking sync handler must not deadlock. get_quote's handler
    // bridges to the main-thread MarketDataService via detail::run_async_wait,
    // which blocks the *calling* thread on a QWaitCondition that only the main
    // event loop can release. If call_tool ran the handler on the main thread,
    // that loop could never pump → deadlock. With FIX 2 the handler runs on a
    // worker while a QEventLoop pumps the main thread.
    //
    // To make this deterministic and network-free, we pre-seed the quote cache
    // so MarketDataService::fetch_quotes hits its all-cached branch and invokes
    // its callback synchronously *on the main thread* (the exact path that
    // deadlocked before FIX 2) — no yfinance/QProcess, no Yahoo rate-limit
    // flakiness. A watchdog thread aborts the process if the call ever hangs, so
    // a real deadlock regression FAILS loudly instead of hanging CI forever.
    void get_quote_returns_without_deadlock() {
        // Seed the cache MarketDataService::fetch_quotes reads (key/shape mirror
        // its store_quote()). peek_quote() only consults the DataHub (empty
        // here), so get_quote falls through to the fetch path we want to probe.
        const QJsonObject co{{"symbol", "AAPL"}, {"name", "Apple"}, {"price", 100.0},
                             {"change", 1.0},    {"change_pct", 1.0}, {"high", 101.0},
                             {"low", 99.0},      {"volume", 1000.0}};
        openmarketterminal::CacheManager::instance().put(
            "market:AAPL",
            QVariant(QString::fromUtf8(QJsonDocument(co).toJson(QJsonDocument::Compact))),
            /*ttl_seconds=*/300, "market_data");

        std::atomic<bool> done{false};
        std::thread watchdog([&done]() {
            for (int i = 0; i < 300 && !done.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!done.load())
                qFatal("get_quote hung > 30s — call_tool deadlock regression");
        });

        auto res = rt_.call_tool("get_quote", QJsonObject{{"symbol", "AAPL"}});
        done.store(true);
        watchdog.join();

        // The cached quote round-trips through the run_async_wait main-thread
        // marshaling and back to the worker — proving no deadlock.
        QVERIFY2(res.success, qPrintable(res.error));
    }

    // FIX 3 — the auth-checker denies destructive tools. set_setting is flagged
    // is_destructive=true, so the McpProvider auth-checker installed by init()
    // must block it and return a failure result.
    void destructive_tool_denied() {
        auto res = rt_.call_tool(
            "set_setting",
            QJsonObject{{"key", "should_be_blocked"}, {"value", "x"}, {"category", "general"}});
        QVERIFY2(!res.success, "set_setting (destructive) should have been denied headless");

        // And it must NOT have been written. get() returns ok(default) on miss,
        // so probe with a sentinel default: if the handler never ran, we get the
        // sentinel back; if it wrote, we'd get "x".
        auto check =
            openmarketterminal::SettingsRepository::instance().get("should_be_blocked", "__missing__");
        QVERIFY2(check.is_ok() && check.value() == "__missing__",
                 qPrintable("destructive tool was denied but still wrote the setting: "
                            + (check.is_ok() ? check.value() : QString("<err>"))));
    }

    void cleanupTestCase() { rt_.shutdown(); }
};

QTEST_MAIN(TstHeadlessRuntime)
#include "tst_headless_runtime.moc"
