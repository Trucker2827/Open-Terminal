#include "core/headless/HeadlessRuntime.h"

#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "datahub/DataHub.h"
#include "mcp/McpInit.h"
#include "mcp/McpProvider.h"
#include "mcp/ToolConfirmationGate.h"
#include "storage/secure/SecureStorage.h"
#include "storage/sqlite/CacheDatabase.h"
#include "storage/sqlite/Database.h"

#include <QDir>
#include <QEventLoop>
#include <QFutureWatcher>

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

    // Open the main database — runs migrations internally.
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

    // Fail-closed gate for destructive AI/MCP tools: deny everything by default.
    // Task 6 will make this honor cli.allow_trading.
    mcp::ToolConfirmationGate::instance().set_presenter(
        [](const QString&, const QString&) { return false; });

    // Register the non-GUI ("core") tool set.
    mcp::register_core_tools();

    initialized_ = true;
    return {true, {}};
}

mcp::ToolResult HeadlessRuntime::call_tool(const QString& name, const QJsonObject& args) {
    // Synchronous dispatch over the async API: a QFutureWatcher defers delivery
    // of finished() to the event loop even when the future is already complete,
    // so this never deadlocks under the host's QCoreApplication.
    QFutureWatcher<mcp::ToolResult> w;
    QEventLoop loop;
    QObject::connect(&w, &QFutureWatcher<mcp::ToolResult>::finished, &loop, &QEventLoop::quit);
    w.setFuture(mcp::McpProvider::instance().call_tool_async(name, args));
    loop.exec();
    return w.future().resultCount() > 0 ? w.future().result()
                                        : mcp::ToolResult::fail("no result");
}

void HeadlessRuntime::shutdown() {
    // One-shot host: singletons own their own teardown; nothing to do.
}

}  // namespace openmarketterminal::headless
