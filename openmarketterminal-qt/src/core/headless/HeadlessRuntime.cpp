#include "core/headless/HeadlessRuntime.h"

#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "datahub/DataHub.h"
#include "mcp/McpInit.h"
#include "mcp/McpProvider.h"
#include "mcp/ToolConfirmationGate.h"
#include "services/markets/MarketDataService.h"
#include "storage/secure/SecureStorage.h"
#include "storage/sqlite/CacheDatabase.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QDir>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QtConcurrent>

namespace openmarketterminal::headless {

InitResult HeadlessRuntime::init(const QString& profile) {
    if (initialized_)
        return {true, {}};

    // Profile selects the datadir — must be set BEFORE AppPaths resolves any
    // path (same ordering rule as the GUI's main.cpp).
    ProfileManager::instance().set_active(profile);

    // Create the per-profile directory tree. A headless host may run against a
    // fresh HOME (e.g. a CI temp dir) where none of these exist yet, so
    // Database::open() would otherwise fail trying to create the .db file.
    QDir().mkpath(AppPaths::root());
    AppPaths::ensure_all();

    // Register the schema migrations BEFORE opening the DB — Database::open()
    // only runs *registered* migrations, and the registration list lives in the
    // shared register_all_migrations() so the GUI and this headless host can
    // never drift. Without this the headless DB comes up with no app schema.
    register_all_migrations();

    // Open the main database — runs the registered migrations internally.
    const QString db_path = AppPaths::data() + "/openmarketterminal.db";
    auto db = Database::instance().open(db_path);
    if (db.is_err())
        return {false, QString::fromStdString(db.error())};

    // Resolve the machine-derived master key (local-first; never touches the OS
    // keychain, so no prompt — safe headless).
    SecureStorage::instance().init();

    // Cache DB is non-fatal (mirrors main.cpp's verified path).
    CacheDatabase::instance().open(AppPaths::data() + "/cache.db");

    // DataHub uses its default owner-active hook (everything active), which is
    // exactly the correct policy for a headless host with no screens/owners.

    // Wire MarketDataService as the `market:quote:*` producer so quote
    // peek/fetch tools (get_quote, etc.) are backed by the hub (mirrors
    // main.cpp). Must happen before tools are dispatched.
    openmarketterminal::services::MarketDataService::instance().ensure_registered_with_hub();

    // REAL enforcement of the destructive-tool gate: McpProvider consults the
    // installed AuthChecker for any tool with auth_required != None OR
    // is_destructive=true, and fails the call closed when the checker returns
    // false. Headless default: allow non-destructive tools, deny destructive
    // ones. Task 6 will refine this to honor cli.allow_trading.
    //
    // NOTE: the ToolConfirmationGate presenter below is NOT the enforcement
    // floor for MCP tool calls — McpProvider never consults it on this path.
    // It only matters to call sites that explicitly query the gate. The
    // auth-checker is what actually blocks destructive tools headless.
    mcp::McpProvider::instance().set_auth_checker(
        [](const QString& /*tool*/, const QJsonObject& /*args*/, mcp::AuthLevel /*required*/,
           bool is_destructive) { return !is_destructive; });

    // Belt-and-suspenders deny-all presenter for any code path that DOES consult
    // the gate directly. Not the MCP enforcement floor (see note above).
    mcp::ToolConfirmationGate::instance().set_presenter(
        [](const QString&, const QString&) { return false; });

    // Register the non-GUI ("core") tool set.
    mcp::register_core_tools();

    initialized_ = true;
    return {true, {}};
}

mcp::ToolResult HeadlessRuntime::call_tool(const QString& name, const QJsonObject& args) {
    // Tool handlers must run OFF the main thread: sync handlers like get_quote
    // bridge to a main-thread service via detail::run_async_wait, which blocks
    // the calling thread on a QWaitCondition that only the main event loop can
    // release. If we ran the handler on the main thread, that loop could never
    // pump → deadlock. So dispatch the (synchronous) handler on a worker via
    // QtConcurrent::run and keep a QEventLoop on the main thread to service the
    // worker's marshaled calls and signal its completion.
    QFutureWatcher<mcp::ToolResult> w;
    QEventLoop loop;
    QObject::connect(&w, &QFutureWatcher<mcp::ToolResult>::finished, &loop, &QEventLoop::quit);
    w.setFuture(QtConcurrent::run(
        [name, args]() { return mcp::McpProvider::instance().call_tool(name, args); }));
    loop.exec();
    return w.future().resultCount() > 0 ? w.future().result()
                                        : mcp::ToolResult::fail("no result");
}

void HeadlessRuntime::shutdown() {
    // One-shot host: singletons own their own teardown; nothing to do.
}

}  // namespace openmarketterminal::headless
