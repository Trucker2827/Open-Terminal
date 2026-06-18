#include "algo_engine/AlgoEngineProducer.h"
#include "algo_engine/ScanMonitor.h"
#include "algo_engine/UniverseScanSelftest.h"
#include "services/llm/LlmService.h"
#include "storage/repositories/LlmConfigRepository.h"
#include "ui/notifications/DesktopNotifier.h"
#include "app/AiActivityNotifier.h"
#include "app/MonitorPickerDialog.h"
#include "app/WindowFrame.h"
#include "app/TerminalShell.h"
#include "core/window/WindowRegistry.h"
#include "auth/AuthManager.h"
#include "auth/InactivityGuard.h"
#include "auth/PinManager.h"
#include "core/config/AppConfig.h"
#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "core/components/ComponentCatalog.h"
#include "core/crash/CrashHandler.h"
#include "core/currency/CurrencyManager.h"
#include "core/i18n/LanguageManager.h"
#include "core/keys/KeyConfigManager.h"
#include "core/layout/DockLayoutSelftest.h"
#include "core/logging/Logger.h"
#include "core/session/ScreenStateManager.h"
#include "core/session/SessionManager.h"
#include "core/symbol/SymbolGroup.h"
#include "core/symbol/SymbolRef.h"
#include "datahub/DataHubMetaTypes.h"
#include "mcp/McpInit.h"
#include "mcp/TerminalMcpBridge.h"
#include "mcp/ToolConfirmationGate.h"
#include "mcp/ToolSelfTest.h"
#include "mcp/BridgeDiscoverySelftest.h"
#include "mcp/DataHubPeekSelftest.h"
#include "services/feeds/FeedSelfTest.h"
#include "trading/PaperTradingSelftest.h"
#include "trading/replication/PortfolioReplicationSelftest.h"
#include "services/workflow/WorkflowHonestySelftest.h"
#ifdef Q_OS_MACOS
#include "web/AppleSpeechTranscriber.h"
#endif
#include "network/http/HttpClient.h"
#include "python/PythonSetupManager.h"
#include "screens/launchpad/LaunchpadScreen.h"
#include "screens/recovery/CrashRecoveryDialog.h"
#include "screens/setup/SetupScreen.h"
#include "storage/workspace/CrashRecovery.h"
#include "storage/workspace/WorkspaceSnapshotRing.h"
#include "services/agents/AgentService.h"
#include "services/dbnomics/DBnomicsService.h"
#include "services/economics/EconomicsService.h"
#include "services/economics/MacroCalendarService.h"
#include "services/geopolitics/GeopoliticsService.h"
#include "services/gov_data/GovDataService.h"
#include "services/ma_analytics/MAAnalyticsService.h"
#include "services/maritime/MaritimeService.h"
#include "services/maritime/PortsCatalog.h"
#include "services/markets/MarketDataService.h"
#include "services/alpha_arena/AlphaArenaEngine.h"
#include "services/news/NewsService.h"
#include "services/notebooks/NotebookLibraryService.h"
#include "services/polymarket/PolymarketWebSocket.h"
#include "services/prediction/PredictionCredentialStore.h"
#include "services/prediction/PredictionExchangeRegistry.h"
#include "services/prediction/openmarketterminal_internal/OpenMarketTerminalInternalAdapter.h"
#include "services/prediction/kalshi/KalshiAdapter.h"
#include "services/prediction/polymarket/PolymarketAdapter.h"
#include "services/relationship_map/RelationshipMapService.h"
#include "services/report_builder/ReportBuilderService.h"
#include "datahub/DataHub.h"
#include "datahub/TopicPolicy.h"
#include "trading/AccountManager.h"
#include "trading/DataStreamManager.h"
#include "trading/ExchangeService.h"
#include "trading/PaperMarkService.h"
#include "trading/ExchangeSessionManager.h"
#include "storage/HistoricalDataStore.h"
#include "storage/repositories/NewsArticleRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/BackupService.h"
#include "storage/secure/SecureStorage.h"
#include "storage/sqlite/CacheDatabase.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include "web/HeadlessBrowser.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QGuiApplication>
#include <QLibrary>
#include <QMessageBox>
#include <QPushButton>
#include <QPointer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <memory>

#include "app/InstanceLock.h"

#ifdef Q_OS_WIN
#    include <Windows.h>
#endif

// Wire the two app-level lifecycle handlers that fire after the primary
// window exists: InstanceLock::message_received (a re-launch of the exe
// asks us to bring the running instance to the front — args ignored, the
// request itself is the trigger) and QApplication::lastWindowClosed (surface the Launchpad
// instead of quitting; the Launchpad's own close handler quits explicitly).
// Called from both the post-setup-screen path and the no-setup path so the
// two branches stay in sync.
static void wire_app_lifecycle(QApplication& app, openmarketterminal::InstanceLock& lock) {
    QObject::connect(&lock, &openmarketterminal::InstanceLock::message_received,
                     [](const QStringList& /*args*/) {
                         // Re-launching the exe while an instance is already
                         // running means "bring the running instance forward" —
                         // the standard single-instance behaviour — NOT "open a
                         // new window". Opening a new window (and the monitor
                         // picker that goes with it) stays an EXPLICIT action:
                         // the toolbar "New Window", Ctrl+Shift+N, the Launchpad
                         // button, and tear-off. Routing relaunches through the
                         // picker surprised users by prompting for a monitor on
                         // every open even when they never asked for a new window.
                         const auto frames = openmarketterminal::WindowRegistry::instance().frames();
                         if (!frames.isEmpty()) {
                             // Lowest window_id (the primary) is the predictable
                             // target. Activating one window pulls the whole app
                             // forward on every platform we support.
                             openmarketterminal::WindowFrame* target = frames.first();
                             if (target->isMinimized())
                                 target->showNormal();
                             target->raise();
                             target->activateWindow();
                             LOG_INFO("App", "Secondary instance request — raised existing window");
                         } else {
                             // No live frames (e.g. the user closed to the
                             // Launchpad). Surface it instead of silently no-op'ing.
                             openmarketterminal::screens::LaunchpadScreen::instance()->surface();
                             LOG_INFO("App", "Secondary instance request — surfaced Launchpad");
                         }
                     });
    QObject::connect(&app, &QApplication::lastWindowClosed, &app, []() {
        // Settings → General → "On last window close" controls behaviour.
        // Default = "quit" so closing the last window quits the app like
        // every normal desktop app. Power users opt in to the Launchpad.
        const auto r = openmarketterminal::SettingsRepository::instance().get(
            QStringLiteral("general.on_last_window_close"), QStringLiteral("quit"));
        const QString choice = r.is_ok() ? r.value() : QStringLiteral("quit");

        if (choice == QStringLiteral("show_launchpad")) {
            openmarketterminal::screens::LaunchpadScreen::instance()->surface();
        } else {
            // "quit" or any unknown value → quit (the safe default).
            QCoreApplication::quit();
        }
    });
}

int main(int argc, char* argv[]) {
    // ── TLS backend selection (must happen before any Qt plugin loading) ────
    // Force QtNetwork to use the OpenSSL TLS backend across platforms.
    //   - macOS: Apple SecureTransport (qtls_st.cpp) double-frees inside
    //     SSLWrite when QWebSocket sends a close frame after a protocol
    //     error. Reproducible by opening the crypto tab — Kraken/Polymarket
    //     WS reconnect path crashes the main thread. SecureTransport was
    //     deprecated by Apple in 10.15.
    //   - Windows: Schannel has had similar instability around WS close
    //     and certificate revocation paths in Qt 6.8. OpenSSL is the
    //     consistent backend on every platform Qt supports.
    // QT_TLS_BACKEND is read by QTlsBackendFactory at plugin-loader time, so
    // it has to be set BEFORE QCoreApplication/QApplication is constructed —
    // setActiveBackend() after the fact does not always take effect because
    // singleton factories may already be bound.
    qputenv("QT_TLS_BACKEND", "openssl");

#ifdef Q_OS_MACOS
    // Pre-load OpenSSL from Homebrew so the openssl plugin's runtime dlopen
    // succeeds. Qt's QLibrary search defaults don't include /opt/homebrew/...
    // Order matters: libcrypto first (libssl depends on it).
    {
        const QStringList crypto_candidates = {
            QStringLiteral("/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib"),
            QStringLiteral("/usr/local/opt/openssl@3/lib/libcrypto.3.dylib"),
        };
        const QStringList ssl_candidates = {
            QStringLiteral("/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib"),
            QStringLiteral("/usr/local/opt/openssl@3/lib/libssl.3.dylib"),
        };
        for (const auto& p : crypto_candidates) {
            if (QFile::exists(p)) { QLibrary(p).load(); break; }
        }
        for (const auto& p : ssl_candidates) {
            if (QFile::exists(p)) { QLibrary(p).load(); break; }
        }
    }
#endif

    // ── Parse --profile <name> from argv before Qt initialises ───────────────
    // This must happen first so that:
    //   1. AppPaths returns the correct per-profile directories
    //   2. InstanceLock uses a profile-scoped IPC key so two different
    //      profiles can run simultaneously as independent primary instances
    {
        for (int i = 1; i < argc - 1; ++i) {
            if (qstrcmp(argv[i], "--profile") == 0) {
                openmarketterminal::ProfileManager::instance().set_active(QString::fromUtf8(argv[i + 1]));
                break;
            }
        }
        // AppPaths::root() must exist before ensure_all() so ProfileManager can
        // write the manifest. Create root now (single mkdir, idempotent).
        QDir().mkpath(openmarketterminal::AppPaths::root());
    }

    // Install the unhandled-exception filter BEFORE any Qt object is
    // constructed. On Windows this writes a minidump to AppPaths::crashdumps()
    // when the process dies from an access violation, stack overflow, or GS
    // cookie check failure (STATUS_STACK_BUFFER_OVERRUN — see issue #215).
    // Do it early so a crash in Qt's own startup still produces a dump.
    openmarketterminal::crash::install();

    // Required before QApplication when any dock panel contains an OpenGL widget
    // (Qt Charts, QOpenGLWidget) — prevents black rendering in floating windows.
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    // QApplication first — InstanceLock's QLocalServer needs an event loop
    // owner. Then the lock probes for an existing primary; if found, it
    // ships our argv to the primary (which opens a new WindowFrame in
    // response) and we exit cleanly.
    //
    // Why not SingleApplication? Its QSharedMemory + QSystemSemaphore lock
    // leaks on macOS Qt 6.6+ (QTBUG-111855), causing silent exits on Finder
    // launch. See InstanceLock.h for the fuller story (issues #234, #252).
    //
    // The instance key is scoped to the active profile name, so
    // "OpenTerminal --profile work" and "OpenTerminal --profile personal"
    // run as two independent primaries.
    QApplication app(argc, argv);

    // Restore widget-visibility work-gating for the GUI (headless leaves the
    // default all-active hook). Walks the owner to its top-level window and
    // reads the `openmarketterminal.active_for_work` dynamic property that
    // WindowFrame maintains (== !isMinimized()). This is the exact decision
    // DataHub used to make inline before the QtWidgets decoupling — note we
    // deliberately read the property rather than isVisible(): isVisible() is
    // false during window construction (WindowStateChange fires before show()
    // returns) and would starve newly-subscribed dashboard widgets. See
    // WindowFrame::is_active_for_work().
    openmarketterminal::datahub::DataHub::set_owner_active_hook([](QObject* owner) -> bool {
        auto* w = qobject_cast<QWidget*>(owner);
        if (!w) return true; // non-widget owner (a service) — always active
        QWidget* top = w->window();
        if (!top) return true;
        const QVariant v = top->property("openmarketterminal.active_for_work");
        if (!v.isValid()) return true; // top isn't a frame that reports — assume active
        return v.toBool();
    });

    app.setApplicationName("Open Terminal");
    app.setOrganizationName("Open Terminal");
#ifndef OPENMARKETTERMINAL_VERSION_STRING
#    define OPENMARKETTERMINAL_VERSION_STRING "0.0.0-dev"
#endif
    app.setApplicationVersion(QStringLiteral(OPENMARKETTERMINAL_VERSION_STRING));

    // Quit only when LaunchpadScreen::closeEvent calls QCoreApplication::quit().
    // Default Qt behaviour fires lastWindowClosed AND schedules an auto-quit;
    // we connect a slot to surface() the Launchpad on lastWindowClosed, and
    // the auto-quit would race that slot — sometimes killing the app while
    // the launchpad is mid-show. With this off, the only quit path is the
    // explicit one in LaunchpadScreen::closeEvent.
    app.setQuitOnLastWindowClosed(false);

    // Belt-and-braces: if QT_TLS_BACKEND wasn't honoured for some reason
    // (e.g. plugin load order on a particular platform), retry the switch
    // explicitly. This is a no-op if openssl is already active.
    {
        const auto backends = QSslSocket::availableBackends();
        if (QSslSocket::activeBackend() != QStringLiteral("openssl")
            && backends.contains(QStringLiteral("openssl"))) {
            QSslSocket::setActiveBackend(QStringLiteral("openssl"));
        }
    }

    // ── On-device speech transcription CLI test ──────────────────────────────
    // Usage: OpenTerminal --transcribe-test <audio-file>
    // Transcribes the file with Apple Speech (on-device) and prints the result
    // to stdout, then exits.  Never affects normal startup.
#ifdef Q_OS_MACOS
    for (int i = 1; i < argc - 1; ++i) {
        if (qstrcmp(argv[i], "--transcribe-test") == 0) {
            const QString audio_path = QString::fromLocal8Bit(argv[i + 1]);
            QEventLoop loop;
            openmarketterminal::web::AppleSpeechTranscriber::transcribe(
                audio_path,
                [&loop](bool ok, QString transcript, QString error) {
                    if (ok)
                        fprintf(stdout, "TRANSCRIPT: %s\n",
                                transcript.toUtf8().constData());
                    else
                        fprintf(stdout, "ERROR: %s\n",
                                error.toUtf8().constData());
                    fflush(stdout);
                    loop.quit();
                });
            loop.exec();
            return 0;
        }
    }
#endif

    // Diagnostic: OpenTerminal --fetch-test <url> → runs the READ FULL fetch
    // (fetch_article_best) through the real headless browser and prints the
    // extracted char count + a sample, then exits.
    for (int i = 1; i < argc - 1; ++i) {
        if (qstrcmp(argv[i], "--fetch-test") == 0) {
            const QString url = QString::fromLocal8Bit(argv[i + 1]);
            QEventLoop loop;
            QFutureWatcher<QString> w;
            QObject::connect(&w, &QFutureWatcher<QString>::finished, [&]() {
                const QString text = w.result();
                fprintf(stdout, "CHARS: %d\nSAMPLE: %s\n", static_cast<int>(text.size()),
                        text.left(500).toUtf8().constData());
                fflush(stdout);
                loop.quit();
            });
            w.setFuture(QtConcurrent::run(
                [url]() { return openmarketterminal::web::HeadlessBrowser::fetch_article_best(url); }));
            loop.exec();
            return 0;
        }
    }

    // ── Single-instance lock + new-window IPC ────────────────────────────────
    const QString profile_key = QString("OpenTerminal-%1").arg(openmarketterminal::ProfileManager::instance().active());
    openmarketterminal::InstanceLock instance_lock;
    const auto lock_status = instance_lock.acquire(profile_key, QCoreApplication::arguments());

    // ── Secondary instance: argv was already shipped to the primary. Exit. ──
    if (lock_status == openmarketterminal::InstanceLock::Status::Secondary) {
#ifdef Q_OS_WIN
        // Grant the primary process permission to bring its new window to
        // the foreground — Windows blocks focus-steal without this. Pre-
        // SingleApplication this used app.primaryPid(); without that we
        // call AllowSetForegroundWindow(ASFW_ANY) which whitelists the
        // whole foreground request from this process.
        AllowSetForegroundWindow(ASFW_ANY);
#endif
        return 0;
    }

    // ── Primary instance from here on ────────────────────────────────────────

    // Bring up the TerminalShell. This is the multi-window refactor's
    // process-level coordinator. Phase 1 ships a skeleton: it bootstraps
    // ProfilePaths, resolves the active ProfileId, and warms the registries
    // (WindowRegistry, ActionRegistry) so WindowFrame constructors don't race
    // their singleton init.
    //
    // Must run BEFORE any service init so future phases that lift services
    // into the shell can rely on it being present.
    openmarketterminal::TerminalShell::instance().initialise();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, []() {
        openmarketterminal::mcp::TerminalMcpBridge::instance().stop();
        openmarketterminal::TerminalShell::instance().shutdown();
    });

    // Register DataHub payload meta-types (QuoteData, HistoryPoint, InfoData,
    // NewsArticle, EconomicsResult) so they can flow through QVariant-keyed
    // topics and cross-thread queued signals. Phase 0 — see
    // openmarketterminal-qt/DATAHUB_ARCHITECTURE.md.
    // Phase 2: register MarketDataService as the `market:quote:*` producer.
    openmarketterminal::datahub::register_metatypes();
    // SymbolContext payload types — signals cross threads when a producer
    // service (not just UI) publishes a group change.
    qRegisterMetaType<openmarketterminal::SymbolRef>("openmarketterminal::SymbolRef");
    qRegisterMetaType<openmarketterminal::SymbolGroup>("openmarketterminal::SymbolGroup");
    // Phase 6: load the Component Browser catalogue. Try the build-side copy
    // first (present after cmake configure copies resources) and fall back to
    // the source-tree path for local dev runs without install step.
    openmarketterminal::ComponentCatalog::instance().load_with_fallbacks({
        // macOS canonical (.app/Contents/Resources/...) first, then dev/build layout.
        QCoreApplication::applicationDirPath() + "/../Resources/resources/component_catalog.json",
        QCoreApplication::applicationDirPath() + "/resources/component_catalog.json",
        QCoreApplication::applicationDirPath() + "/component_catalog.json",
        "resources/component_catalog.json",
    });
    openmarketterminal::services::MarketDataService::instance().ensure_registered_with_hub();

    // ── Sync services needed by the default dashboard ─────────────────────────
    // Anything a default dashboard widget subscribes to during its first show
    // must be registered with the hub before the window paints. Everything
    // else is deferred to a single QTimer::singleShot(0) below — the event
    // loop runs that batch immediately after the first paint, so cold-start
    // perceived latency drops without changing functional behavior.
    openmarketterminal::services::NewsService::instance().ensure_registered_with_hub();
    openmarketterminal::services::EconomicsService::instance().ensure_registered_with_hub();
    openmarketterminal::services::MacroCalendarService::instance().ensure_registered_with_hub();
    openmarketterminal::trading::DataStreamManager::instance().ensure_registered_with_hub();
    openmarketterminal::services::geo::GeopoliticsService::instance().ensure_registered_with_hub();
    openmarketterminal::services::maritime::MaritimeService::instance().ensure_registered_with_hub();
    openmarketterminal::services::maritime::PortsCatalog::instance().ensure_registered_with_hub();
    openmarketterminal::services::RelationshipMapService::instance().ensure_registered_with_hub();
    openmarketterminal::services::ma::MAAnalyticsService::instance().ensure_registered_with_hub();

    // ── Pre-warm the dashboard topics ────────────────────────────────────────
    // The user spends real time on the login / setup / recovery flow before
    // the dashboard ever paints. Kick the hub now so producers start fetching
    // immediately; by the time the dashboard widgets subscribe in showEvent,
    // peek() returns a fresh value and deliver_initial_value() paints it on
    // the first frame instead of showing the loading overlay.
    //
    // Set is the union of topics used by every widget in the default
    // `portfolio_manager` template plus the global indices/forex/crypto/
    // commodities universes (so any user-customised default still hits the
    // warm cache). Late-registered producers (the QTimer::singleShot(0)
    // batch below) warm themselves on their own next scheduler pass once
    // subscribers exist; pre-warming them here would orphan-log because
    // ensure_registered_with_hub() hasn't run for them yet.
    QTimer::singleShot(0, qApp, []() {
        auto& hub = openmarketterminal::datahub::DataHub::instance();
        QStringList topics;

        auto add_quotes = [&](const QStringList& syms) {
            topics.reserve(topics.size() + syms.size());
            for (const auto& s : syms)
                topics.append(QStringLiteral("market:quote:") + s);
        };

        // Default-template widget symbol sets (kept aligned with the widget
        // source — if a widget's hardcoded list changes, update here too).
        add_quotes(openmarketterminal::services::MarketDataService::indices_symbols());
        add_quotes(openmarketterminal::services::MarketDataService::forex_symbols());
        add_quotes(openmarketterminal::services::MarketDataService::crypto_symbols());
        add_quotes(openmarketterminal::services::MarketDataService::commodity_symbols());
        add_quotes({"^GSPC", "^IXIC", "^DJI", "^RUT", "^VIX", "GC=F"});           // performance
        add_quotes({"^VIX", "SPY", "QQQ", "IWM", "TLT",
                    "NVDA", "TSLA", "AMD", "META", "PLTR", "COIN"});              // risk_metrics
        add_quotes({"AAPL", "MSFT", "GOOGL", "AMZN", "NVDA", "TSLA", "META",
                    "JPM"});                                                       // watchlist default

        // Non-quote topics used by the default template.
        topics.append(QStringLiteral("news:general"));
        topics.append(QStringLiteral("econ:openmarketterminal:upcoming_events"));

        // De-duplicate (several add_quotes calls overlap on common symbols).
        topics.removeDuplicates();

        // force=true bypasses min_interval_ms so the very first cold-start
        // fetch isn't gated by an unrelated test refresh; producer rate
        // limits still apply at dispatch (DataHub::flush_coalesced_requests).
        hub.request(topics, /*force=*/true);
        LOG_INFO("App", QString("Pre-warmed %1 dashboard topics during login screen")
                            .arg(topics.size()));
    });

    // ── Deferred service init — fires after first window paint ───────────────
    // These services back tab-specific screens (F&O, prediction markets,
    // alpha arena, agents, wallet/treasury/staking, etc.) — none of them is
    // needed for the dashboard's first paint, so registering them here would
    // only add latency to the user-visible cold start. Late registration is
    // safe: the hub's scheduler tick picks up matching subscriptions on the
    // next pass once the producer is registered.
    QTimer::singleShot(0, qApp, []() {
        // NOTE: the legacy F&O / options-chain stack (OptionChainService,
        // OISnapshotter, FiiDiiService and the F&O algo engine) is not part of
        // this build — there is no F&O UI and those producers are not registered.
        // Multi-broker session manager — `ws:kraken:*` / `ws:hyperliquid:*`.
        openmarketterminal::trading::ExchangeSessionManager::instance().ensure_registered_with_hub();
        // Prediction Markets — `prediction:polymarket:*`.
        openmarketterminal::services::polymarket::PolymarketWebSocket::instance().ensure_registered_with_hub();
        // Alpha Arena engine — TickClock, ModelDispatcher, OrderRouter,
        // PaperVenue. Not a DataHub Producer (callback-style by design).
        // init() is idempotent; pre-resolves crash-recovery state.
        openmarketterminal::services::alpha_arena::AlphaArenaEngine::instance().init();
        {
            auto& reg = openmarketterminal::services::prediction::PredictionExchangeRegistry::instance();
            reg.register_adapter(
                std::make_unique<openmarketterminal::services::prediction::polymarket_ns::PolymarketAdapter>());
            reg.register_adapter(
                std::make_unique<openmarketterminal::services::prediction::kalshi_ns::KalshiAdapter>());
            // OpenMarketTerminal internal prediction-market adapter (demo mode until
            // `openmarketterminal.markets_endpoint` is configured).
            reg.register_adapter(
                std::make_unique<openmarketterminal::services::prediction::openmarketterminal_internal::OpenMarketTerminalInternalAdapter>());

            // Hydrate credentials from SecureStorage if previously saved.
            if (auto* pm = dynamic_cast<openmarketterminal::services::prediction::polymarket_ns::PolymarketAdapter*>(
                    reg.adapter(QStringLiteral("polymarket")))) {
                pm->reload_credentials();
            }
            if (auto* ks = dynamic_cast<openmarketterminal::services::prediction::kalshi_ns::KalshiAdapter*>(
                    reg.adapter(QStringLiteral("kalshi")))) {
                if (auto creds = openmarketterminal::services::prediction::PredictionCredentialStore::load_kalshi()) {
                    ks->set_credentials(*creds);
                }
            }
            if (auto* fi = reg.adapter(QStringLiteral("openmarketterminal"))) {
                fi->ensure_registered_with_hub();
            }
        }
        // Specialized data sources.
        openmarketterminal::services::DBnomicsService::instance().ensure_registered_with_hub();
        openmarketterminal::services::GovDataService::instance().ensure_registered_with_hub();
        // Agents — `agent:*` push-only producer.
        openmarketterminal::services::AgentService::instance().ensure_registered_with_hub();

        // Algo Engine — `algo:metrics:*`, `algo:trade:*`, `algo:state:*`.
        openmarketterminal::algo::AlgoEngineProducer::instance().ensure_registered_with_hub();

        // LOCAL-FIRST FORK: OpenMarketTerminal Cloud sync removed. No adapters are registered
        // and the engine is never initialised, so the user's workspace (watchlists,
        // notes, portfolios, dashboards, reports, agent configs, workflows, notebooks,
        // settings, news) is never pushed/pulled to api.example.com.

        // Broker session monitor — re-validates each connected broker account's
        // access token on a 5-min cadence and silently refreshes where supported
        // (e.g. OAuth refresh tokens). Keeps the connection indicator honest
        // instead of showing a stale "green".
        openmarketterminal::trading::AccountManager::instance().start_session_monitor();

        // Periodically auto-download historical candles for any watchlisted
        // series. Double-gated to a no-op: does nothing unless the Historify
        // watchlist has entries AND a broker account is connected.
        auto* historify_timer = new QTimer(qApp);
        historify_timer->setInterval(15 * 60 * 1000); // 15 min
        QObject::connect(historify_timer, &QTimer::timeout, qApp, []() {
            openmarketterminal::storage::HistoricalDataStore::instance().refresh_watchlist();
        });
        historify_timer->start();

        LOG_INFO("App", "Deferred service init complete");
    });

    // Create all application directories under %LOCALAPPDATA%/org.openterminal.OpenTerminal
    openmarketterminal::AppPaths::ensure_all();

    // ── One-time migration from legacy %APPDATA% location ─────────────────
    // Current locations (under %LOCALAPPDATA%\org.openterminal.OpenTerminal\):
    //   Log: <root>/logs/openmarketterminal.log    DB: <root>/data/openmarketterminal.db
    {
        const QString old_base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const auto migrate_file = [](const QString& old_path, const QString& new_path) {
            if (QFile::exists(old_path) && !QFile::exists(new_path))
                QFile::rename(old_path, new_path);
        };
        migrate_file(old_base + "/openmarketterminal.db", openmarketterminal::AppPaths::data() + "/openmarketterminal.db");
        migrate_file(old_base + "/cache.db", openmarketterminal::AppPaths::data() + "/cache.db");
        migrate_file(old_base + "/openmarketterminal.log", openmarketterminal::AppPaths::logs() + "/openmarketterminal.log");
        migrate_file(old_base + "/openmarketterminal-files", openmarketterminal::AppPaths::files());
        // Remove stale WAL/SHM from old location too
        QFile::remove(old_base + "/openmarketterminal.db-wal");
        QFile::remove(old_base + "/openmarketterminal.db-shm");
        QFile::remove(old_base + "/cache.db-wal");
        QFile::remove(old_base + "/cache.db-shm");
    }

    // SQLite owns its own .db-wal / .db-shm files. Pre-deleting them is
    // destructive: any committed transaction that has not yet been checkpointed
    // back into the main .db file lives entirely in the WAL. Auto-checkpoint
    // only fires past ~1000 pages, and the on-close checkpoint can fail
    // silently — small writes (e.g. pin_hash on the secure_credentials table)
    // routinely persist only in the WAL across runs. SQLite reads the WAL on
    // open to recover any uncheckpointed or crash-truncated state, so we leave
    // these files for SQLite to manage. Single-instance enforcement is owned
    // by InstanceLock above.

    // Clean legacy v3 DB location (these paths are no longer live DBs)
    {
        const QString local_dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        const QString legacy1 = local_dir.section('/', 0, -3) + "/OpenMarketTerminal/openmarketterminal_settings.db";
        const QString legacy2 =
            QString(local_dir).replace("OpenMarketTerminal/OpenMarketTerminal", "OpenMarketTerminal") + "/openmarketterminal_settings.db";
        QFile::remove(legacy1 + "-wal");
        QFile::remove(legacy1 + "-shm");
        QFile::remove(legacy2 + "-wal");
        QFile::remove(legacy2 + "-shm");
    }

    openmarketterminal::Logger::instance().set_file(openmarketterminal::AppPaths::logs() + "/openmarketterminal.log");

    // Seed the prebuilt OpenMarketTerminal Notebook library into the File Manager on first
    // run (idempotent — guarded by a marker file). Makes the curated notebooks
    // appear in both the Notebook Library and the File Manager out of the box.
    openmarketterminal::services::NotebookLibraryService::instance().seed_into_files();

    // P3.18 — route Qt's own qDebug/qWarning/qCritical messages into our log
    // file so framework/3rd-party warnings are visible in Release builds.
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
        const char* category = (ctx.category && *ctx.category) ? ctx.category : "Qt";
        switch (type) {
        case QtDebugMsg:    openmarketterminal::Logger::instance().debug(category, msg); break;
        case QtInfoMsg:     openmarketterminal::Logger::instance().info(category, msg); break;
        case QtWarningMsg:  openmarketterminal::Logger::instance().warn(category, msg); break;
        case QtCriticalMsg: openmarketterminal::Logger::instance().error(category, msg); break;
        case QtFatalMsg:
            openmarketterminal::Logger::instance().error(category, msg);
            openmarketterminal::Logger::instance().flush_and_close();
            break;
        }
    });
    {
        auto& log = openmarketterminal::Logger::instance();
        auto& cfg = openmarketterminal::AppConfig::instance();

        // Global level
        const QString gl = cfg.get("log/global_level", "Info").toString();
        const QHash<QString, openmarketterminal::LogLevel> lvl_map = {{"Trace", openmarketterminal::LogLevel::Trace},
                                                           {"Debug", openmarketterminal::LogLevel::Debug},
                                                           {"Info", openmarketterminal::LogLevel::Info},
                                                           {"Warn", openmarketterminal::LogLevel::Warn},
                                                           {"Error", openmarketterminal::LogLevel::Error},
                                                           {"Fatal", openmarketterminal::LogLevel::Fatal}};
        log.set_level(lvl_map.value(gl, openmarketterminal::LogLevel::Info));

        // JSON output mode (persisted in Settings → Logging)
        log.set_json_mode(cfg.get("log/json_mode", false).toBool());

        // Per-tag overrides
        const int count = cfg.get("log/tag_count", 0).toInt();
        for (int i = 0; i < count; ++i) {
            const QString tag = cfg.get(QString("log/tag_%1_name").arg(i)).toString();
            const QString level = cfg.get(QString("log/tag_%1_level").arg(i)).toString();
            if (!tag.isEmpty() && lvl_map.contains(level))
                log.set_tag_level(tag, lvl_map.value(level));
        }
    }
    LOG_INFO("App", QStringLiteral("OpenMarketTerminal v%1 starting...").arg(QCoreApplication::applicationVersion()));
    LOG_INFO("App", QString("TLS backend: %1 (available: %2)")
                        .arg(QSslSocket::activeBackend(),
                             QSslSocket::availableBackends().join(", ")));

    // Theme is applied after DB is open so saved font/theme are respected from the start.

    // Initialize config
    auto& config = openmarketterminal::AppConfig::instance();
    openmarketterminal::HttpClient::instance().set_base_url(config.api_base_url());
    // Note: auth tokens are managed by AuthManager::initialize() which loads
    // from SecureStorage (DPAPI) and SQLite — not from QSettings/Registry.

    // Register migrations explicitly (avoids MSVC /OPT:REF stripping static-init
    // TUs). The ordered list lives in register_all_migrations() so the GUI and
    // the headless host (HeadlessRuntime) share one source of truth and cannot
    // drift. Must run BEFORE Database::open().
    openmarketterminal::register_all_migrations();

    // Apply a staged data restore (from a user backup) BEFORE opening any
    // database — restores are swapped in on launch so live, open DBs are never
    // overwritten in place. No-op unless a restore was staged in Settings.
    {
        auto restore = openmarketterminal::BackupService::apply_pending_restore();
        if (restore.is_err())
            LOG_ERROR("App", "Pending restore failed: " + QString::fromStdString(restore.error()));
    }

    // Open main database
    QString db_path = openmarketterminal::AppPaths::data() + "/openmarketterminal.db";
    auto db_result = openmarketterminal::Database::instance().open(db_path);
    if (db_result.is_err()) {
        LOG_ERROR("App", "Failed to open database: " + QString::fromStdString(db_result.error()));
        // DB unavailable — apply theme with built-in defaults so the UI is at least styled
        openmarketterminal::ui::apply_global_stylesheet();
    } else {
        // Headless honesty selftest: run here, AFTER the DB is open but BEFORE
        // SecureStorage::init() touches the OS keychain. A rebuilt (ad-hoc-signed)
        // binary triggers a keychain ACL prompt that blocks forever in a non-GUI/CI
        // launch, so the line-800 selftest dispatch can never be reached headless.
        // This early return keeps --selftest-workflow-honesty runnable in CI.
        for (int wfh_i = 1; wfh_i < argc; ++wfh_i) {
            if (qstrcmp(argv[wfh_i], "--selftest-workflow-honesty") == 0)
                return openmarketterminal::workflow::run_workflow_honesty_selftest();
        }

        // Resolve the SecureStorage master key on the MAIN thread before any
        // credential read. On macOS this touches the OS keychain (must not run
        // first on a worker thread); the key is then cached for the worker
        // threads that read broker/wallet secrets during order placement etc.
        openmarketterminal::SecureStorage::instance().init();

        // Load broker accounts now that the DB is open. The AccountManager
        // singleton loads eagerly in its constructor on first access; if anything
        // touched it before this point (before open()), it found an unusable DB
        // and loaded nothing. This explicit main-thread reload guarantees the
        // account map is populated from the now-open DB, so configured brokers
        // survive restarts instead of vanishing.
        openmarketterminal::trading::AccountManager::instance().reload_from_db();

        // First-run convenience: if the user has no trading accounts yet, seed a
        // ready-to-use $100k PAPER account so the Equity screen populates out of
        // the box (mirrors Crypto's default paper portfolio). One-time + flagged,
        // so it never reappears after the user deliberately removes their accounts.
        openmarketterminal::trading::AccountManager::instance().ensure_default_paper_account();

        // Prune news articles older than 30 days — deferred to run after the event loop
        // starts so the startup critical path is not blocked.
        // NewsArticleRepository uses the main-thread DB connection (not thread-safe),
        // so we must not run this on a worker thread — QTimer::singleShot(0) posts it
        // to the main thread's event queue instead.
        {
            int64_t news_cutoff = QDateTime::currentSecsSinceEpoch() - (30LL * 86400);
            QTimer::singleShot(0, [news_cutoff]() {
                openmarketterminal::NewsArticleRepository::instance().prune_older_than(news_cutoff);
                LOG_INFO("App", "News articles pruned (keeping 30 days)");
            });
        }

        // Load persisted font settings and apply before any window is shown
        // — eliminates flash/wrong-font-on-startup. Theme is always Obsidian.
        {
            auto& repo = openmarketterminal::SettingsRepository::instance();
            auto& tm = openmarketterminal::ui::ThemeManager::instance();
            auto r_family = repo.get("appearance.font_family");
            auto r_size = repo.get("appearance.font_size");
            QString family = r_family.is_ok() ? r_family.value() : "Consolas";
            QString size_s = r_size.is_ok() ? r_size.value() : "14px";
            int size_px = size_s.left(size_s.indexOf("px")).toInt();
            if (size_px <= 0)
                size_px = 14;
            tm.apply_font(family, size_px);
            tm.apply_theme("Obsidian");
            LOG_INFO("App", "Theme: Obsidian, font: " + family + " " + size_s);
        }

        // Load persisted language and install the matching QTranslator before
        // any windows are shown — eliminates an English-flash on first paint
        // when the user has previously chosen another language.
        openmarketterminal::i18n::LanguageManager::instance().initialize();

        // Load persisted display-currency preference so the symbol is correct
        // on first paint of any calculator/analytics surface.
        openmarketterminal::currency::CurrencyManager::instance().initialize();
    }

    // Open cache database (non-fatal if fails)
    QString cache_path = openmarketterminal::AppPaths::data() + "/cache.db";
    auto cache_result = openmarketterminal::CacheDatabase::instance().open(cache_path);
    if (cache_result.is_err()) {
        LOG_WARN("App", "Cache DB failed (non-fatal): " + QString::fromStdString(cache_result.error()));
    }

    // LOCAL-FIRST FORK: purge any stale OpenMarketTerminal LLM provider row left in the
    // llm_configs table by a prior (pre-fork) build. The hosted OpenMarketTerminal proxy is
    // removed — LlmService already coerces it to local Ollama so it can never
    // transmit, and this also stops it appearing in Settings → LLM Config.
    openmarketterminal::LlmConfigRepository::instance().delete_provider("openmarketterminal");

    // Assign a unique session ID so ScreenStateManager can tag each state write.
    // This lets us distinguish cross-session restores from same-session saves.
    {
        const QString sid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        openmarketterminal::ScreenStateManager::instance().set_session_id(sid);
        LOG_INFO("App", "Session ID: " + sid);
    }

    LOG_INFO("App", "Checking settings for legacy migration...");
    // One-time migration: copy settings from old DB (Local\OpenMarketTerminal\openmarketterminal_settings.db)
    // to new DB (Roaming\OpenMarketTerminal\OpenMarketTerminal\openmarketterminal.db) if the new DB has no settings yet.
    {
        LOG_INFO("App", "Querying settings...");
        auto existing = openmarketterminal::SettingsRepository::instance().get("openmarketterminal_session");
        LOG_INFO("App", "Settings query done");
        bool new_db_empty = existing.is_err() || existing.value().isEmpty();
        if (new_db_empty) {
            QString local_base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            // AppLocalDataLocation = .../Local/OpenMarketTerminal/OpenMarketTerminal — strip to .../Local/OpenMarketTerminal
            QString old_db_path = local_base.section('/', 0, -3) + "/OpenMarketTerminal/openmarketterminal_settings.db";
            if (!QFile::exists(old_db_path)) {
                // Try without the org subfolder
                old_db_path = local_base.replace("OpenMarketTerminal/OpenMarketTerminal", "OpenMarketTerminal") + "/openmarketterminal_settings.db";
            }
            if (QFile::exists(old_db_path)) {
                QSqlDatabase old_db = QSqlDatabase::addDatabase("QSQLITE", "legacy_migration");
                old_db.setDatabaseName(old_db_path);
                if (old_db.open()) {
                    QSqlQuery src(old_db);
                    if (src.exec("SELECT key, value FROM settings")) {
                        int count = 0;
                        while (src.next()) {
                            openmarketterminal::SettingsRepository::instance().set(src.value(0).toString(),
                                                                        src.value(1).toString(), "migrated");
                            ++count;
                        }
                        LOG_INFO("App", QString("Migrated %1 settings from legacy DB").arg(count));
                    }
                    old_db.close();
                }
                QSqlDatabase::removeDatabase("legacy_migration");
            }
        }
    }

    LOG_INFO("App", "Starting session manager...");
    // Start session
    openmarketterminal::SessionManager::instance().start_session();

    // Phase 1 final lift: shell owns auth/lock service initialisation.
    // bootstrap_auth() runs AuthManager::initialize(), warms PinManager
    // from SecureStorage, and configures InactivityGuard's lock timeout
    // from SettingsRepository. The previous in-line block here is folded
    // into TerminalShell::bootstrap_auth.
    openmarketterminal::TerminalShell::instance().bootstrap_auth();

    // Force the ReportBuilderService singleton onto the main thread before
    // MCP tools register — tools route into it via QMetaObject::invokeMethod
    // with BlockingQueuedConnection from worker threads, so the service must
    // already exist with main-thread affinity.
    (void)openmarketterminal::services::ReportBuilderService::instance();

    // Initialize MCP tool system — registers all internal tools and starts
    // external MCP servers in the background (non-blocking).
    openmarketterminal::mcp::initialize_all_tools();

    // Install the per-action approval modal for destructive AI/MCP tools (live
    // orders, code execution, config writes). The gate calls this on the MAIN
    // thread and treats anything but an explicit Approve click as a DENY (Esc,
    // window-close, and the default button are all Deny), so the AI cannot act
    // un-approved. Without this presenter the gate fails closed (denies).
    openmarketterminal::mcp::ToolConfirmationGate::instance().set_presenter(
        [](const QString& title, const QString& detail) -> bool {
            QMessageBox box;
            box.setIcon(QMessageBox::Warning);
            box.setWindowTitle(QObject::tr("Confirm AI action"));
            box.setText(title);
            box.setInformativeText(detail);
            QPushButton* approve = box.addButton(QObject::tr("Approve"), QMessageBox::AcceptRole);
            QPushButton* deny = box.addButton(QObject::tr("Deny"), QMessageBox::RejectRole);
            box.setDefaultButton(deny);   // default highlighted button = Deny
            box.setEscapeButton(deny);    // Esc / window-close = Deny
            box.exec();
            return box.clickedButton() == approve;  // true ONLY on explicit Approve
        });

    // ── Headless tool-system self-test / catalog dump ────────────────────────
    // Runs after the real tool registration above but before any window or
    // network init, so it exercises exactly what ships. Exits without starting
    // the GUI — used by the dev loop and CI to measure tool retrieval recall
    // and registry integrity (no LLM / API key required).
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--selftest-tools") == 0)
            return openmarketterminal::mcp::run_tool_selftest();
        if (qstrcmp(argv[i], "--selftest-datahub-peek") == 0)
            return openmarketterminal::mcp::run_datahub_peek_selftest();
        if (qstrcmp(argv[i], "--selftest-bridge-discovery") == 0)
            return openmarketterminal::mcp::run_bridge_discovery_selftest();
        if (qstrcmp(argv[i], "--dump-tools") == 0)
            return openmarketterminal::mcp::dump_tools_json();
        if (qstrcmp(argv[i], "--selftest-feeds") == 0)
            return openmarketterminal::feeds::run_feed_selftest();
        if (qstrcmp(argv[i], "--selftest-dock-layout") == 0)
            return openmarketterminal::layout::run_dock_layout_selftest();
        if (qstrcmp(argv[i], "--selftest-universe-scan") == 0)
            return openmarketterminal::algo::run_universe_scan_selftest();
        // NOTE: --selftest-workflow-honesty is dispatched EARLY (before SecureStorage
        // init) so it stays runnable headless — the keychain ACL prompt would block here.
        if (qstrcmp(argv[i], "--selftest-paper") == 0)
            return openmarketterminal::trading::run_paper_trading_selftest();
        if (qstrcmp(argv[i], "--selftest-portfolio-replication") == 0)
            return openmarketterminal::trading::replication::run_portfolio_replication_selftest();
    }

    // Bridge autostart: make the localhost tool surface available whenever the
    // GUI is up (not only during an agent run), so openterminalcli can attach.
    // Placed AFTER the --selftest-* dispatch above so a selftest never binds a
    // socket or leaves a stale bridge.json — selftests return before reaching here.
    {
        const auto r = openmarketterminal::SettingsRepository::instance().get(
            QStringLiteral("bridge.autostart"), QStringLiteral("true"));
        const QString v = r.is_ok() ? r.value() : QStringLiteral("true");
        if (v != QStringLiteral("false"))
            openmarketterminal::mcp::TerminalMcpBridge::instance().start();
    }

    // Start the scan-watch background service. Runs after Database::open() (which
    // applies the scan_watches migration) and after bootstrap_auth() (broker
    // creds, needed by the first candle poll), and after the headless self-test
    // early-returns above so it is skipped on --selftest-tools / --dump-tools.
    // Placed before the Python-setup branch so both GUI paths (setup screen and
    // normal startup) start it exactly once. Candle fetching is native C++ (broker
    // REST / native Yahoo), so it does not require the Python env to be ready.
    openmarketterminal::algo::ScanMonitor::instance().start();

    // Centralized paper mark-to-market + order matching. Runs independent of which
    // screen is open so paper positions (equity AND F&O) keep their P&L live and
    // resting limit/stop/SL-TP orders fill continuously — not only while the
    // Equity tab is focused. Placed alongside ScanMonitor (after the self-test
    // early-returns, so it stays off in headless --selftest runs).
    openmarketterminal::trading::PaperMarkService::instance().start();

    // Native desktop notifications (Win toast / macOS Notification Center / Linux
    // libnotify) via a tray icon — also surfaces every in-app ToastService toast.
    openmarketterminal::ui::DesktopNotifier::instance().init();

    // Construct the AI-activity notifier on the main thread NOW so its EventBus
    // subscription (queued) is live from boot — toasts then fire on ANY screen,
    // not only after the Settings → AI Activity tab is first opened. Thread
    // affinity is fixed to the main thread by this first call.
    (void) openmarketterminal::app::AiActivityNotifier::instance();

    // ── Python environment check ─────────────────────────────────────────────
    // check_status() fast path (sentinel + markers present) is synchronous and
    // cheap. The slow path (first run) can spawn processes — but at this point
    // no window is visible yet so the brief block is acceptable. The SetupScreen
    // itself offloads prefill_completed_steps() to a background thread (P1).
    auto setup_status = openmarketterminal::python::PythonSetupManager::instance().check_status();

    if (setup_status.needs_setup) {
        LOG_INFO("App", "Python environment not ready — showing setup screen");

        // Use QPointer so the setup_complete lambda is safe against double-fire
        // (e.g. user somehow triggers it twice before the window is hidden).
        auto* setup_screen = new openmarketterminal::screens::SetupScreen;
        QPointer<openmarketterminal::screens::SetupScreen> screen_guard(setup_screen);
        setup_screen->setWindowTitle("Open Terminal — First-Time Setup");
        setup_screen->resize(800, 600);
        setup_screen->show();

        // When setup completes, hide setup screen and launch main window.
        // The connection uses Qt::SingleShotConnection (Qt 6.0+) so the lambda
        // fires exactly once even if setup_complete is somehow emitted twice.
        QObject::connect(setup_screen, &openmarketterminal::screens::SetupScreen::setup_complete,
                         [&app, &instance_lock, screen_guard]() {
            if (!screen_guard)
                return; // already cleaned up — ignore
            screen_guard->hide();
            screen_guard->deleteLater();

            openmarketterminal::KeyConfigManager::instance(); // init before WindowFrame registers shortcuts

            // Phase 6 final: if the previous session ended uncleanly and a
            // workspace snapshot is available, give the user the option to
            // restore. On accept, WorkspaceShell::apply already constructs
            // the frames it needs — we skip our own primary-window creation
            // path. On skip (or no recovery available), fall through.
            bool recovered = false;
            if (auto* recovery = openmarketterminal::TerminalShell::instance().crash_recovery();
                recovery && recovery->needs_recovery()) {
                openmarketterminal::screens::CrashRecoveryDialog dlg(
                    recovery, openmarketterminal::TerminalShell::instance().snapshot_ring());
                dlg.exec();
                recovered = dlg.was_restored();
            }

            if (!recovered) {
                // Single primary window by default — see the matching no-setup
                // path below for the full rationale. Extra windows stay an
                // explicit user action ("New Window" / Ctrl+Shift+N / tear-off).
                const QList<int> saved_ids =
                    openmarketterminal::SessionManager::instance().load_window_ids();
                const int primary_id = saved_ids.isEmpty() ? 0 : saved_ids.first();
                auto* window = new openmarketterminal::WindowFrame(primary_id);
                window->setAttribute(Qt::WA_DeleteOnClose);
                window->show();
            }

            // Wire new-window handler + Launchpad surface now that the
            // primary window exists. Single source of truth — see
            // wire_app_lifecycle() at the top of this file.
            wire_app_lifecycle(app, instance_lock);

            if (!openmarketterminal::ai_chat::LlmService::instance().is_configured())
                LOG_WARN("App",
                         "LLM provider not configured — AI chat will prompt user to configure Settings → LLM Config");

            // Warm agent discovery cache (same reason as the main path).
            QTimer::singleShot(0, &app, []() {
                openmarketterminal::services::AgentService::instance().discover_agents();
            });

            LOG_INFO("App", "Application ready (after setup)");
        });

        return app.exec();
    }

    // Ensure KeyConfigManager is initialized before WindowFrame registers shortcuts
    openmarketterminal::KeyConfigManager::instance();

    // Phase 6 final: offer crash recovery before constructing the primary
    // window. If the user accepts and restoration succeeds, WorkspaceShell
    // has already built the frames it needs from the snapshot — we skip
    // both the primary-window creation and the SessionManager-based
    // secondary-window restoration paths to avoid duplicating windows.
    bool recovered = false;
    if (auto* recovery = openmarketterminal::TerminalShell::instance().crash_recovery();
        recovery && recovery->needs_recovery()) {
        openmarketterminal::screens::CrashRecoveryDialog dlg(
            recovery, openmarketterminal::TerminalShell::instance().snapshot_ring());
        dlg.exec();
        recovered = dlg.was_restored();
    }

    // Restore a SINGLE primary window at startup. The previous session may
    // have had several windows spread across multiple monitors, but auto-
    // reopening all of them surprised multi-monitor users — every launch
    // popped a second terminal on the second screen. Opening additional
    // windows stays an EXPLICIT action (toolbar "New Window", Ctrl+Shift+N,
    // the Launchpad button, tear-off), consistent with the single-instance
    // relaunch policy in wire_app_lifecycle(). We reopen the lowest saved
    // window_id (the primary) so its geometry + dock layout come back; the
    // user spawns extra windows on demand. closeEvent self-heals the saved
    // id set to the surviving windows, so this converges to [primary] cleanly.
    if (!recovered) {
        const QList<int> saved_ids =
            openmarketterminal::SessionManager::instance().load_window_ids();
        const int primary_id = saved_ids.isEmpty() ? 0 : saved_ids.first();
        auto* primary = new openmarketterminal::WindowFrame(primary_id);
        primary->setAttribute(Qt::WA_DeleteOnClose);
        primary->show();
    }

    // Wire new-window handler + Launchpad surface — see wire_app_lifecycle()
    // at the top of this file for the contract.
    wire_app_lifecycle(app, instance_lock);

    // If requirements files changed (app update), sync packages in background
    // without blocking the user. Connect setup_complete so failures are logged
    // (no SetupScreen in this path, so we only log — don't show UI).
    if (setup_status.needs_package_sync) {
        LOG_INFO("App", "Requirements changed — syncing packages in background");
        auto& mgr = openmarketterminal::python::PythonSetupManager::instance();
        QObject::connect(&mgr, &openmarketterminal::python::PythonSetupManager::setup_complete,
                         &mgr, [](bool success, const QString& error) {
            if (success)
                LOG_INFO("App", "Background package sync completed successfully");
            else
                LOG_WARN("App", "Background package sync failed (non-fatal): " + error);
        }, Qt::SingleShotConnection);
        mgr.run_setup();
    }

    if (!openmarketterminal::ai_chat::LlmService::instance().is_configured())
        LOG_WARN("App", "LLM provider not configured — AI chat will prompt user to configure Settings → LLM Config");

    // Warm the agent discovery cache on startup. This populates
    // AgentService::cached_agents() so any screen that lists agents
    // (Agent Config, Portfolio → Agent Runner, Node Editor) shows the
    // full finagent_core set immediately instead of falling back to the
    // much smaller DB-only list. Run deferred so Python is fully ready.
    QTimer::singleShot(0, &app, []() {
        openmarketterminal::services::AgentService::instance().discover_agents();
    });

    LOG_INFO("App", "Application ready");
    return app.exec();
}
