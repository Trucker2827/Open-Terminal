#include "core/headless/HeadlessRuntime.h"

#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "datahub/DataHub.h"
#include "mcp/McpInit.h"
#include "mcp/McpProvider.h"
#include "mcp/ToolConfirmationGate.h"
#include "mcp/tools/SettingsGate.h"
#include "services/DataServices.h"
#include "services/markets/MarketDataService.h"
#include "services/prediction/PredictionExchangeRegistry.h"
#include "services/prediction/kalshi/KalshiAdapter.h"
#include "services/prediction/polymarket/PolymarketAdapter.h"
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

    // Wire the headless-registerable data-producing services (MarketData, News,
    // Economics, MacroCalendar, Geopolitics, Maritime, MAAnalytics, DBnomics,
    // GovData, Agent) as DataHub producers so every data tool (get_quote,
    // get_news, the economics/geo/maritime/gov tools, etc.) is backed by the
    // hub. Must happen before tools are dispatched. Registration is cheap
    // producer-wiring only (no fetch/connect/spawn); see
    // register_all_data_services() (which documents why the GUI's 11th producer,
    // RelationshipMapService, is GUI-only and excluded here).
    openmarketterminal::services::register_all_data_services();

    // Register the prediction-market exchange adapters so the PM read tools can
    // resolve them headless (the GUI does the same in main.cpp). The
    // `if(!reg.adapter(...))` guard is idempotent AND lets a test pre-register a
    // fake adapter for the same id and win.
    {
        auto& reg = services::prediction::PredictionExchangeRegistry::instance();
        if (!reg.adapter("polymarket"))
            reg.register_adapter(std::make_unique<services::prediction::polymarket_ns::PolymarketAdapter>());
        if (!reg.adapter("kalshi"))
            reg.register_adapter(std::make_unique<services::prediction::kalshi_ns::KalshiAdapter>());
    }

    // REAL enforcement of the two CLI capability gates: McpProvider consults the
    // installed AuthChecker for any tool with auth_required != None OR
    // is_destructive=true, and fails the call CLOSED when the checker returns
    // false. When the checker returns true the tool RUNS. So the checker decision
    // is the single authoritative gate — flipping a setting genuinely enables the
    // corresponding tool headless.
    //
    // Classification (default-deny):
    //   • ELEVATED AUTH (auth_required >= Verified, i.e. verified-email /
    //     subscription / explicit-confirm) → ALWAYS deny. A headless host has no
    //     interactive human and no session, so it can never satisfy these — deny
    //     outright, regardless of the trading / settings-write toggles. This floor
    //     is checked FIRST so it cannot be bypassed by the gates below. (Every
    //     real core Verified+ tool today is also is_destructive, but a future
    //     non-destructive Verified+ tool would otherwise fall through to the
    //     `return true` below and run unauthenticated — this closes that gap.)
    //   • settings-WRITE tool (category "settings" && is_destructive, e.g.
    //     set_setting; Authenticated) → allow iff cli_settings_write_allowed().
    //   • any other destructive / trading-execution tool → DENY (matches the
    //     daemon ServeCommand checker). The AI's ONLY live-execution path is the
    //     gated `submit_order` carve-out above (kill-switch → armed →
    //     allowed-account → fresh risk floor → daily-loss → reserve). The direct
    //     `live_*` broker tools (live_place_order/live_smart_order/live_close_*/
    //     live_cancel_*) and other destructive tools must NOT bypass that stack —
    //     before Phase C this was `cli_trading_allowed()`, which (once a human set
    //     cli.allow_trading=true to enable live) would have fired real orders
    //     gated by that flag alone, sidestepping the kill switch / arm /
    //     allowed-account / daily-loss constitution.
    //   • everything else reaching the checker (non-destructive, incl. settings
    //     READS) → allowed (the user owns the read tool).
    //
    // NOTE: the ToolConfirmationGate presenter below is NOT the enforcement
    // floor for MCP tool calls — McpProvider never consults it on this path.
    mcp::McpProvider::instance().set_auth_checker(
        [](const QString& tool, const QJsonObject& args, mcp::AuthLevel required,
           bool is_destructive) {
            if (required >= mcp::AuthLevel::Verified)
                return false;
            if (tool == "submit_order") {
                // Normalize identically to the handler so a case/whitespace variant
                // can't take a different branch than the handler will.
                const QString mode = args.value("mode").toString().trimmed().toLower();
                if (mode == "paper")
                    return true; // reach the handler; it enforces the toggle + executes
                return mcp::cli_trading_allowed() && mcp::cli_live_armed(); // live: false in Phase A
            }
            if (mcp::is_settings_write_tool(tool))
                return mcp::cli_settings_write_allowed();
            if (is_destructive)
                return false; // direct destructive tools never bypass the submit_order gate stack
            return true;
        });

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
