#include "cli/ServeCommand.h"
#include "cli/BridgeDiscoveryFile.h"
#include "cli/CryptoFeedHub.h"
#include "cli/automation/AutomationState.h"
#include "core/headless/HeadlessRuntime.h"
#include "mcp/TerminalMcpBridge.h"
#include "mcp/McpProvider.h"
#include "mcp/McpTypes.h"
#include "mcp/tools/SettingsGate.h"
#include "services/crypto_latency/CryptoLatencyService.h"
#include "services/edge_radar/CryptoMicrostructureRadar.h"
#include "services/prediction/PredictionExchangeRegistry.h"
#include "services/prediction/kalshi/KalshiAdapter.h"
#include "services/sandbox/MakerQuotes.h"
#include "storage/repositories/EdgePredictionModelRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/sqlite/Database.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QSocketNotifier>
#include <QThread>
#include <QJsonObject>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QTimer>
#include <QStandardPaths>
#include <QUuid>
#include <QVariant>
#include <QXmlStreamWriter>
#include <cstdio>
#include <csignal>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/types.h>
#endif

namespace openmarketterminal::cli {

bool kalshi_event_stream_needs_recovery(bool workload_active, bool connected,
                                        int subscribed_assets, qint64 event_age_ms,
                                        qint64 stale_after_ms) {
    if (!workload_active || subscribed_assets <= 0) return false;
    return !connected || event_age_ms < 0 || event_age_ms > stale_after_ms;
}

bool kalshi_universe_request_timed_out(bool pending, qint64 request_age_ms,
                                      qint64 timeout_ms) {
    return pending && request_age_ms >= 0 && request_age_ms > timeout_ms;
}

bool kalshi_planner_process_timed_out(bool active, qint64 process_age_ms,
                                      qint64 timeout_ms) {
    return active && process_age_ms >= 0 && process_age_ms > timeout_ms;
}

bool kalshi_non_execution_process_timed_out(bool active, qint64 process_age_ms,
                                            qint64 timeout_ms) {
    return active && process_age_ms >= 0 && process_age_ms > timeout_ms;
}

qint64 kalshi_event_cycle_delay_ms(bool live_session_active, bool paper_active,
                                   qint64 elapsed_ms) {
    // An armed live session gets the next fresh decision immediately. Paper
    // evidence is deliberately paced: every websocket delta is not an
    // independent experiment, and re-planning on each one starves the worker.
    const qint64 minimum_interval_ms = live_session_active ? 3000
        : paper_active ? 15000 : 60000;
    return std::max<qint64>(0, minimum_interval_ms - std::max<qint64>(0, elapsed_ms));
}

QStringList kalshi_event_planner_args() {
    return {QStringLiteral("kalshi"), QStringLiteral("auto"), QStringLiteral("run"),
            QStringLiteral("--category"), QStringLiteral("Crypto#BTC@live"),
            // Live websocket events already select the nearest settlement cohort.
            // Twelve contracts keeps planning inside the executable-quote window;
            // an exhaustive surface scan belongs to the explicit CLI planner.
            QStringLiteral("--limit"), QStringLiteral("12"),
            QStringLiteral("--timeout-ms"), QStringLiteral("12000"),
            QStringLiteral("--max-positions"), QStringLiteral("5"),
            QStringLiteral("--unit-notional"), QStringLiteral("2"),
            QStringLiteral("--max-cost"), QStringLiteral("10"),
            QStringLiteral("--min-edge"), QStringLiteral("0.03")};
}

bool kalshi_should_persist_independent_spot_tick(const QString& source, double price,
                                                 qint64 received_ts_ms,
                                                 qint64 last_persisted_ts_ms,
                                                 qint64 minimum_interval_ms) {
    const QString normalized = source.trimmed().toLower();
    if (price <= 0.0 || received_ts_ms <= 0 ||
        (!normalized.contains(QStringLiteral("coinbase")) &&
         !normalized.contains(QStringLiteral("kraken"))))
        return false;
    return last_persisted_ts_ms <= 0 ||
           received_ts_ms - last_persisted_ts_ms >= std::max<qint64>(1, minimum_interval_ms);
}

KalshiFlowMetrics kalshi_flow_metrics(const QVector<KalshiFlowLevel>& yes_bid_levels,
                                      const QVector<KalshiFlowLevel>& no_bid_levels,
                                      const QVector<KalshiFlowTrade>& trades,
                                      const QVector<KalshiFlowDelta>& deltas,
                                      qint64 now_ms,
                                      qint64 lookback_ms,
                                      int depth_levels) {
    KalshiFlowMetrics out;
    out.observed_at_ms = now_ms;
    const qint64 cutoff_ms = now_ms - std::max<qint64>(1, lookback_ms);
    const int levels = std::max(1, depth_levels);
    for (int i = 0; i < yes_bid_levels.size() && i < levels; ++i)
        out.yes_bid_depth += std::max(0.0, yes_bid_levels[i].quantity);
    for (int i = 0; i < no_bid_levels.size() && i < levels; ++i)
        out.no_bid_depth += std::max(0.0, no_bid_levels[i].quantity);
    for (const auto& trade : trades) {
        if (trade.ts_ms < cutoff_ms || trade.ts_ms > now_ms || trade.quantity <= 0.0) continue;
        if (trade.outcome.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0)
            out.yes_taker_quantity += trade.quantity;
        else if (trade.outcome.compare(QStringLiteral("no"), Qt::CaseInsensitive) == 0)
            out.no_taker_quantity += trade.quantity;
        else
            continue;
        ++out.recent_trade_count;
    }
    for (const auto& delta : deltas) {
        if (delta.ts_ms < cutoff_ms || delta.ts_ms > now_ms || delta.quantity_delta == 0.0) continue;
        const bool yes = delta.outcome.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0;
        const bool no = delta.outcome.compare(QStringLiteral("no"), Qt::CaseInsensitive) == 0;
        if (!yes && !no) continue;
        const double quantity = std::abs(delta.quantity_delta);
        if (yes && delta.quantity_delta > 0.0) out.yes_added_quantity += quantity;
        if (yes && delta.quantity_delta < 0.0) out.yes_removed_quantity += quantity;
        if (no && delta.quantity_delta > 0.0) out.no_added_quantity += quantity;
        if (no && delta.quantity_delta < 0.0) out.no_removed_quantity += quantity;
        ++out.recent_delta_count;
    }
    const auto imbalance = [](double left, double right) {
        const double total = left + right;
        return total > 0.0 ? (left - right) / total : 0.0;
    };
    out.depth_imbalance = imbalance(out.yes_bid_depth, out.no_bid_depth);
    out.taker_imbalance = imbalance(out.yes_taker_quantity, out.no_taker_quantity);
    const double yes_wall_change = out.yes_added_quantity - out.yes_removed_quantity;
    const double no_wall_change = out.no_added_quantity - out.no_removed_quantity;
    out.wall_imbalance = imbalance(std::max(0.0, yes_wall_change),
                                   std::max(0.0, no_wall_change));
    // Resting depth is useful context, while prints are the stronger signal.
    // This remains advisory: market microstructure can change faster than a
    // one-window summary and it must never grant execution authority.
    out.combined_pressure = 0.55 * out.taker_imbalance + 0.35 * out.depth_imbalance
                          + 0.10 * out.wall_imbalance;
    const bool active = out.recent_trade_count >= 3 || out.recent_delta_count >= 6;
    const bool two_sided = out.yes_bid_depth > 0.0 && out.no_bid_depth > 0.0;
    if (active && two_sided && out.combined_pressure >= 0.25) {
        out.signal = QStringLiteral("YES PRESSURE");
        out.confidence = std::abs(out.combined_pressure) >= 0.50
            ? QStringLiteral("HIGH") : QStringLiteral("CONFIRMING");
    } else if (active && two_sided && out.combined_pressure <= -0.25) {
        out.signal = QStringLiteral("NO PRESSURE");
        out.confidence = std::abs(out.combined_pressure) >= 0.50
            ? QStringLiteral("HIGH") : QStringLiteral("CONFIRMING");
    } else if (active) {
        out.confidence = QStringLiteral("MIXED");
    }
    return out;
}

QJsonObject kalshi_flow_to_json(const KalshiFlowMetrics& metrics) {
    return QJsonObject{
        {QStringLiteral("observed_at_ms"), QString::number(metrics.observed_at_ms)},
        {QStringLiteral("window_ms"), 30000},
        {QStringLiteral("yes_bid_depth"), metrics.yes_bid_depth},
        {QStringLiteral("no_bid_depth"), metrics.no_bid_depth},
        {QStringLiteral("yes_taker_quantity"), metrics.yes_taker_quantity},
        {QStringLiteral("no_taker_quantity"), metrics.no_taker_quantity},
        {QStringLiteral("yes_added_quantity"), metrics.yes_added_quantity},
        {QStringLiteral("no_added_quantity"), metrics.no_added_quantity},
        {QStringLiteral("yes_removed_quantity"), metrics.yes_removed_quantity},
        {QStringLiteral("no_removed_quantity"), metrics.no_removed_quantity},
        {QStringLiteral("trade_count"), metrics.recent_trade_count},
        {QStringLiteral("delta_count"), metrics.recent_delta_count},
        {QStringLiteral("depth_imbalance"), metrics.depth_imbalance},
        {QStringLiteral("taker_imbalance"), metrics.taker_imbalance},
        {QStringLiteral("wall_imbalance"), metrics.wall_imbalance},
        {QStringLiteral("combined_pressure"), metrics.combined_pressure},
        {QStringLiteral("signal"), metrics.signal},
        {QStringLiteral("confidence"), metrics.confidence},
        {QStringLiteral("advisory_only"), true},
        {QStringLiteral("interpretation"),
         QStringLiteral("Public contract flow; not unique traders and never an execution signal")}};
}

namespace {
#ifndef _WIN32
int g_sigfd[2] = {-1, -1};                       // self-pipe: handler writes, notifier reads
void on_signal(int) { char c = 1; ssize_t n = ::write(g_sigfd[1], &c, 1); (void)n; }
#endif

QString daemon_slug(QString s) {
    s = s.trimmed();
    QString out;
    for (const QChar ch : s) {
        if (ch.isLetterOrNumber() || ch == '-' || ch == '_' || ch == '.')
            out += ch;
        else if (!out.endsWith('-'))
            out += '-';
    }
    while (out.endsWith('-')) out.chop(1);
    return out.isEmpty() ? QStringLiteral("default") : out;
}

QString daemon_label(const QString& profile) {
    return QStringLiteral("org.openterminal.cli.daemon.%1").arg(daemon_slug(profile));
}

QString daemon_plist_path(const QString& profile) {
    return QDir::homePath() + QStringLiteral("/Library/LaunchAgents/%1.plist").arg(daemon_label(profile));
}

QString daemon_logs_dir(const QString& profile) {
    return profile_root_for(profile) + QStringLiteral("/logs");
}

QString daemon_state_dir(const QString& profile) {
    return profile_root_for(profile) + QStringLiteral("/daemon");
}

QString daemon_jobs_path(const QString& profile) {
    return daemon_state_dir(profile) + QStringLiteral("/jobs.json");
}

QString daemon_runtime_path(const QString& profile) {
    return daemon_state_dir(profile) + QStringLiteral("/runtime.json");
}

QString daemon_history_db_path(const QString& profile) {
    return daemon_state_dir(profile) + QStringLiteral("/runs.sqlite");
}

QString daemon_job_log_path(const QString& profile) {
    return daemon_logs_dir(profile) + QStringLiteral("/daemon.jobs.log");
}

QString daemon_scalp_config_path(const QString& profile) {
    return daemon_state_dir(profile) + QStringLiteral("/scalp_engine.json");
}

QString daemon_scalp_state_path(const QString& profile) {
    return daemon_state_dir(profile) + QStringLiteral("/scalp_state.json");
}

QString daemon_scalp_ticks_path(const QString& profile) {
    return daemon_state_dir(profile) + QStringLiteral("/scalp_ticks.jsonl");
}

QString daemon_scalp_decisions_path(const QString& profile) {
    return daemon_state_dir(profile) + QStringLiteral("/scalp_decisions.jsonl");
}

QString daemon_maker_decisions_path(const QString& profile) {
    return daemon_state_dir(profile) + QStringLiteral("/maker_decisions.jsonl");
}

QString daemon_maker_ticks_path(const QString& profile) {
    return daemon_state_dir(profile) + QStringLiteral("/maker_ticks.jsonl");
}

QString now_utc() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QString kalshi_evidence_path(const QString& filename) {
    const QString override_dir = qEnvironmentVariable("OPENTERMINAL_KALSHI_EVIDENCE_DIR").trimmed();
    const QString dir = override_dir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
              QStringLiteral("/Open Terminal/Open Terminal")
        : override_dir;
    QDir().mkpath(dir);
    return QDir(dir).filePath(filename);
}

// Persistent Kalshi event coordinator. The socket only decides *when* to run
// the deterministic planner. All trading authority remains in execute-next ->
// prepare_order -> submit_order, which rechecks the revocable live gates,
// session, limits, quote freshness, duplicate contract and experiment cap.
class KalshiLiveEventEngine final : public QObject {
  public:
    explicit KalshiLiveEventEngine(QString profile, CryptoFeedHub* crypto_feed_hub,
                                   QObject* parent = nullptr)
        : QObject(parent), profile_(std::move(profile)), crypto_feed_hub_(crypto_feed_hub) {
        using services::prediction::PredictionExchangeRegistry;
        using services::prediction::kalshi_ns::KalshiAdapter;

        adapter_ = qobject_cast<KalshiAdapter*>(
            PredictionExchangeRegistry::instance().adapter(QStringLiteral("kalshi")));
        debounce_.setSingleShot(true);
        debounce_.setInterval(75);
        connect(&debounce_, &QTimer::timeout, this, [this]() { start_decision_cycle(); });

        status_flush_.setInterval(1000);
        connect(&status_flush_, &QTimer::timeout, this, [this]() {
            // Keep the health timestamp live even when a falsely connected
            // transport stops producing callbacks.
            write_status();
        });
        status_flush_.start();

        universe_refresh_.setInterval(60000);
        connect(&universe_refresh_, &QTimer::timeout, this, [this]() { refresh_universe(); });
        universe_refresh_.start();

        health_watchdog_.setInterval(2000);
        connect(&health_watchdog_, &QTimer::timeout, this, [this]() { check_health(); });
        health_watchdog_.start();

        book_snapshot_flush_.setSingleShot(true);
        book_snapshot_flush_.setInterval(25);
        connect(&book_snapshot_flush_, &QTimer::timeout, this,
                [this]() { write_book_snapshot(); });

        // The Kalshi socket knows the contract price, not the independent BTC
        // reference used by the planner. Keep that reference fresh from the
        // daemon's already-shared Coinbase/Kraken hub; this avoids a second
        // WebSocket connection and removes the old dependency on a separate
        // scheduled `edge collect` process.
        if (crypto_feed_hub_) {
            crypto_feed_hub_->ensure_symbol(QStringLiteral("BTC-USD"),
                                             {QStringLiteral("coinbase"),
                                              QStringLiteral("kraken")});
            spot_feed_connection_ = connect(
                crypto_feed_hub_, &CryptoFeedHub::tick_received, this,
                [this](const QString& symbol,
                       const services::crypto_latency::CryptoLatencyTick& tick) {
                    if (symbol != QStringLiteral("BTC-USD")) return;
                    persist_independent_spot_tick(tick);
                });
        } else {
            last_error_ = QStringLiteral("Kalshi independent spot feed hub unavailable");
        }

        if (!adapter_) {
            last_error_ = QStringLiteral("Kalshi adapter unavailable");
            status_dirty_ = true;
            return;
        }

        credentials_ = adapter_->has_credentials();
        connect(adapter_, &services::prediction::PredictionExchangeAdapter::markets_ready,
                this, [this](const auto& markets) { accept_market_universe(markets); });
        connect(adapter_, &KalshiAdapter::ws_connection_changed, this, [this](bool connected) {
            connected_ = connected;
            last_event_at_ = now_utc();
            status_dirty_ = true;
            if (connected) schedule_decision(QStringLiteral("websocket_connected"));
        });
        connect(adapter_, &KalshiAdapter::ws_trade_event, this,
                [this](const QString& ticker, const QJsonObject& payload) {
                    record_flow_trade(ticker, payload);
                    observe_market_event(QStringLiteral("trade"), ticker, payload);
                });
        connect(adapter_, &KalshiAdapter::ws_ticker_event, this,
                [this](const QString& ticker, const QJsonObject& payload) {
                    observe_market_event(QStringLiteral("ticker"), ticker, payload);
                });
        connect(adapter_, &KalshiAdapter::ws_orderbook_event, this,
                [this](const QString& type, const QString& ticker, qint64 sequence,
                       const QJsonObject& payload) {
                    last_sequence_ = sequence;
                    record_flow_delta(ticker, type, payload);
                    observe_market_event(type, ticker, payload);
                });
        connect(adapter_, &services::prediction::PredictionExchangeAdapter::ws_orderbook_updated,
                this, [this](const QString& asset_id,
                             const services::prediction::PredictionOrderBook& book) {
                    if (!subscribed_asset_ids_.contains(asset_id)) return;
                    normalized_books_.insert(asset_id, book);
                    const double bid = book.bids.isEmpty() ? 0.0 : book.bids.first().price;
                    const double ask = book.asks.isEmpty() ? 0.0 : book.asks.first().price;
                    const bool bid_usable = !book.bids.isEmpty() && book.bids.first().size >= 1.0;
                    const bool ask_usable = !book.asks.isEmpty() && book.asks.first().size >= 1.0;
                    const QString signature = QStringLiteral("%1|%2|%3|%4")
                        .arg(bid, 0, 'f', 4).arg(ask, 0, 'f', 4)
                        .arg(bid_usable ? 1 : 0).arg(ask_usable ? 1 : 0);
                    if (top_book_signatures_.value(asset_id) == signature) return;
                    top_book_signatures_.insert(asset_id, signature);
                    qint64 exchange_observed_at_ms = book.last_update_ms;
                    if (exchange_observed_at_ms > 0 && exchange_observed_at_ms < 10'000'000'000LL)
                        exchange_observed_at_ms *= 1000;
                    const qint64 observed_at_ms = QDateTime::currentMSecsSinceEpoch();
                    top_book_snapshots_.insert(asset_id, QJsonObject{
                        {QStringLiteral("asset_id"), asset_id},
                        {QStringLiteral("bid"), bid},
                        {QStringLiteral("ask"), ask},
                        {QStringLiteral("bid_size"), book.bids.isEmpty()
                             ? 0.0 : book.bids.first().size},
                        {QStringLiteral("ask_size"), book.asks.isEmpty()
                             ? 0.0 : book.asks.first().size},
                        {QStringLiteral("observed_at_ms"), QString::number(observed_at_ms)},
                        {QStringLiteral("exchange_observed_at_ms"),
                         QString::number(exchange_observed_at_ms)},
                        {QStringLiteral("source"), QStringLiteral("kalshi_websocket")}});
            const int asset_separator = asset_id.lastIndexOf(':');
            if (asset_separator > 0)
                update_flow_snapshot(asset_id.left(asset_separator), observed_at_ms);
                    book_snapshot_flush_.start();
                    schedule_decision(QStringLiteral("top_of_book_change"));
                });
        connect(adapter_, &KalshiAdapter::ws_market_lifecycle_event, this,
                [this](const QString& ticker, const QString&, const QJsonObject& payload) {
                    observe_market_event(QStringLiteral("market_lifecycle"), ticker, payload);
                    refresh_universe();
                });
        connect(adapter_, &KalshiAdapter::ws_account_event, this,
                [this](const QString&, const QJsonObject& payload) {
                    ++account_events_;
                    capture_exchange_timestamp(payload);
                    account_reconcile_pending_ = true;
                    status_dirty_ = true;
                    maybe_start_work();
                });
        connect(adapter_, &KalshiAdapter::ws_cf_benchmark_event, this,
                [this](const QString& index_id, double value, qint64 ts_ms,
                       const QJsonObject&) {
                    if (index_id.compare(QStringLiteral("BRTI"), Qt::CaseInsensitive) != 0 ||
                        value <= 0.0)
                        return;
                    if (ts_ms > 0 && ts_ms < 10'000'000'000LL) ts_ms *= 1000;
                    const qint64 received = QDateTime::currentMSecsSinceEpoch();
                    EdgePredictionRawTick tick;
                    tick.symbol = QStringLiteral("BTC");
                    tick.source = QStringLiteral("cfbenchmarks:BRTI");
                    tick.price = value;
                    tick.exchange_ts = ts_ms > 0 ? ts_ms : received;
                    tick.received_ts = received;
                    const auto stored = EdgePredictionModelRepository::instance().add_raw_tick(tick);
                    if (stored.is_err())
                        last_error_ = QStringLiteral("CF Benchmarks tick persistence failed: %1")
                                          .arg(QString::fromStdString(stored.error()));
                    ++cf_benchmark_events_;
                    latest_cf_benchmark_ = value;
                    latest_cf_benchmark_ms_ = tick.exchange_ts;
                    status_dirty_ = true;
                    schedule_decision(QStringLiteral("settlement_index_change"));
                });
        connect(adapter_, &services::prediction::PredictionExchangeAdapter::credentials_changed,
                this, [this]() {
                    credentials_ = adapter_ && adapter_->has_credentials();
                    status_dirty_ = true;
                    refresh_universe();
                });

        adapter_->subscribe_cf_benchmarks({QStringLiteral("BRTI")});
        refresh_universe();
        write_status();
    }

    ~KalshiLiveEventEngine() override {
        QObject::disconnect(spot_feed_connection_);
        if (adapter_ && !subscribed_asset_ids_.isEmpty())
            adapter_->unsubscribe_market(QStringList(subscribed_asset_ids_.begin(),
                                                      subscribed_asset_ids_.end()));
        write_status();
    }

  private:
    using KalshiAdapter = services::prediction::kalshi_ns::KalshiAdapter;

    bool session_active() const {
        QFile file(kalshi_evidence_path(QStringLiteral("kalshi-live-session.json")));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QJsonObject session = QJsonDocument::fromJson(file.readAll()).object();
        if (!session.value(QStringLiteral("enabled")).toBool() ||
            !session.value(QStringLiteral("autonomous")).toBool())
            return false;
        const QDateTime ends = QDateTime::fromString(
            session.value(QStringLiteral("ends_at")).toString(), Qt::ISODateWithMs);
        return !ends.isValid() || ends > QDateTime::currentDateTimeUtc();
    }

    bool parallel_paper_active() const {
        const auto value = SettingsRepository::instance().get(
            QStringLiteral("kalshi.paper_automation.enabled"), QStringLiteral("false"));
        return value.is_ok() && value.value().compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
    }

    qint64 event_age_ms(qint64 now_ms) const {
        if (last_event_at_.isEmpty()) return -1;
        const QDateTime event_time = QDateTime::fromString(last_event_at_, Qt::ISODateWithMs);
        return event_time.isValid() ? std::max<qint64>(0, now_ms - event_time.toMSecsSinceEpoch()) : -1;
    }

    void persist_independent_spot_tick(
        const services::crypto_latency::CryptoLatencyTick& tick) {
        static constexpr qint64 kPersistIntervalMs = 1000;
        const qint64 received = tick.received_ts_ms > 0
            ? tick.received_ts_ms : QDateTime::currentMSecsSinceEpoch();
        const QString source = tick.source.trimmed().toLower();
        const qint64 last_persisted = independent_spot_persisted_ms_.value(source, 0);
        if (!kalshi_should_persist_independent_spot_tick(source, tick.price, received,
                                                         last_persisted, kPersistIntervalMs))
            return;

        EdgePredictionRawTick raw;
        raw.id = QStringLiteral("kalshi-spot:%1:%2:%3")
                     .arg(source, QString::number(received), QString::number(tick.sequence));
        raw.symbol = QStringLiteral("BTC");
        raw.source = source;
        raw.price = tick.price;
        raw.exchange_ts = tick.exchange_ts_ms > 0 ? tick.exchange_ts_ms : received;
        raw.received_ts = received;
        const auto stored = EdgePredictionModelRepository::instance().add_raw_tick(raw);
        if (stored.is_err()) {
            last_error_ = QStringLiteral("independent BTC spot persistence failed: %1")
                              .arg(QString::fromStdString(stored.error()));
            status_dirty_ = true;
            return;
        }

        independent_spot_persisted_ms_.insert(source, received);
        latest_independent_spot_ = tick.price;
        latest_independent_spot_source_ = source;
        latest_independent_spot_ms_ = received;
        ++independent_spot_events_;
        status_dirty_ = true;
        schedule_decision(QStringLiteral("independent_spot_change"));
    }

    void check_health() {
        static constexpr qint64 kUniverseTimeoutMs = 15000;
        static constexpr qint64 kEventStaleMs = 10000;
        static constexpr qint64 kReconnectCooldownMs = 15000;
        static constexpr qint64 kRecoveryDecisionMs = 5000;
        static constexpr qint64 kPlannerTimeoutMs = 25000;
        static constexpr qint64 kPaperTimeoutMs = 25000;
        static constexpr qint64 kAccountReadTimeoutMs = 15000;

        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        check_process_timeout(now_ms, kPlannerTimeoutMs, kPaperTimeoutMs,
                              kAccountReadTimeoutMs);
        const bool live_active = session_active();
        const bool workload_active = live_active || parallel_paper_active();

        if (live_active && !session_was_active_) {
            ++session_wakeups_;
            schedule_decision(QStringLiteral("session_armed"));
            // A newly armed session must not wait for the minute refresh.
            if (!universe_request_pending_) refresh_universe();
        }
        session_was_active_ = live_active;

        const qint64 request_age = universe_request_pending_ && universe_request_started_ms_ > 0
            ? now_ms - universe_request_started_ms_ : -1;
        if (kalshi_universe_request_timed_out(universe_request_pending_, request_age,
                                              kUniverseTimeoutMs)) {
            universe_request_pending_ = false;
            universe_request_started_ms_ = 0;
            ++universe_timeouts_;
            last_error_ = QStringLiteral("Kalshi market-universe request timed out; retrying");
            refresh_universe();
        }

        const qint64 age = event_age_ms(now_ms);
        stream_healthy_ = !kalshi_event_stream_needs_recovery(
            workload_active, connected_, subscribed_asset_ids_.size(), age, kEventStaleMs);
        if (!stream_healthy_) {
            if (now_ms - last_reconnect_attempt_ms_ >= kReconnectCooldownMs) {
                last_reconnect_attempt_ms_ = now_ms;
                ++reconnect_attempts_;
                last_error_ = QStringLiteral("Kalshi event stream stale; reconnecting");
                if (adapter_) adapter_->restart_websocket();
            }
            // REST planner/executor fallback keeps a bounded live session
            // evaluating candidates while the socket reconnects. The normal
            // quote, duplicate, quota and live gates remain authoritative.
            if (live_active && now_ms - last_recovery_decision_ms_ >= kRecoveryDecisionMs) {
                last_recovery_decision_ms_ = now_ms;
                ++recovery_decisions_;
                schedule_decision(QStringLiteral("stale_stream_recovery"));
            }
        } else if (last_error_.startsWith(QStringLiteral("Kalshi event stream stale"))) {
            last_error_.clear();
        }
        status_dirty_ = true;
    }

    void refresh_universe() {
        if (!adapter_ || universe_request_pending_) return;
        credentials_ = adapter_->has_credentials();
        if (!credentials_) {
            last_error_ = QStringLiteral("Kalshi credentials are not configured");
            status_dirty_ = true;
            return;
        }
        universe_request_pending_ = true;
        universe_request_started_ms_ = QDateTime::currentMSecsSinceEpoch();
        adapter_->list_markets(QStringLiteral("Crypto#BTC@live"), QString(), 500, 0);
    }

    void accept_market_universe(
        const QVector<services::prediction::PredictionMarket>& markets) {
        if (!universe_request_pending_) return;
        universe_request_pending_ = false;
        universe_request_started_ms_ = 0;
        struct Candidate {
            const services::prediction::PredictionMarket* market = nullptr;
            qint64 close_ms = 0;
            double activity = 0.0;
        };
        QVector<Candidate> candidates;
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        QSet<QString> next;
        for (const auto& market : markets) {
            const QString ticker = market.key.market_id.trimmed().toUpper();
            if (!ticker.startsWith(QStringLiteral("KXBTC")) || market.closed || !market.active)
                continue;
            const QDateTime close = QDateTime::fromString(market.end_date_iso, Qt::ISODate);
            if (!close.isValid()) continue;
            const qint64 close_ms = close.toMSecsSinceEpoch();
            // The event engine is for the presently tradable 15m/hourly race,
            // not every future BTC ladder returned by the category endpoint.
            if (close_ms < now_ms - 60000 || close_ms > now_ms + 90 * 60 * 1000LL)
                continue;
            candidates.append(Candidate{&market, close_ms,
                                        market.volume + 2.0 * market.liquidity +
                                            0.25 * market.open_interest});
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            if (a.close_ms != b.close_ms) return a.close_ms < b.close_ms;
            return a.activity > b.activity;
        });
        // Keep the nearest two settlement cohorts and at most 32 liquid
        // contracts. The planner still obtains the complete ladder over REST;
        // this bounded set is only the low-latency event trigger surface.
        QVector<qint64> cohorts;
        QHash<qint64, int> cohort_counts;
        for (const Candidate& candidate : candidates) {
            if (!cohorts.contains(candidate.close_ms)) {
                if (cohorts.size() >= 2) continue;
                cohorts.append(candidate.close_ms);
            }
            if (!cohorts.contains(candidate.close_ms) ||
                cohort_counts.value(candidate.close_ms) >= 16)
                continue;
            for (const QString& asset_id : candidate.market->key.asset_ids)
                if (!asset_id.trimmed().isEmpty()) next.insert(asset_id);
            cohort_counts[candidate.close_ms] += 1;
        }
        QSet<QString> added = next;
        added.subtract(subscribed_asset_ids_);
        QSet<QString> removed = subscribed_asset_ids_;
        removed.subtract(next);
        if (!removed.isEmpty())
            adapter_->unsubscribe_market(QStringList(removed.begin(), removed.end()));
        for (const QString& asset_id : removed) top_book_signatures_.remove(asset_id);
        for (const QString& asset_id : removed) top_book_snapshots_.remove(asset_id);
        for (const QString& asset_id : removed) normalized_books_.remove(asset_id);
        if (!removed.isEmpty()) book_snapshot_flush_.start();
        if (!added.isEmpty())
            adapter_->subscribe_market(QStringList(added.begin(), added.end()));
        subscribed_asset_ids_ = next;
        if (!next.isEmpty() && subscription_started_ms_ <= 0)
            subscription_started_ms_ = QDateTime::currentMSecsSinceEpoch();
        last_universe_refresh_ = now_utc();
        last_error_.clear();
        status_dirty_ = true;
        if (!next.isEmpty()) schedule_decision(QStringLiteral("market_universe_refresh"));
    }

    bool tracks(const QString& ticker) const {
        return subscribed_asset_ids_.contains(ticker + QStringLiteral(":yes")) ||
               subscribed_asset_ids_.contains(ticker + QStringLiteral(":no"));
    }

    void capture_exchange_timestamp(const QJsonObject& payload) {
        for (const QString& key : {QStringLiteral("ts"), QStringLiteral("timestamp"),
                                   QStringLiteral("created_time"), QStringLiteral("updated_time")}) {
            const QJsonValue value = payload.value(key);
            if (!value.isUndefined() && !value.isNull()) {
                last_exchange_timestamp_ = value.isString()
                    ? value.toString() : QString::number(value.toDouble(), 'f', 0);
                return;
            }
        }
    }

    static qint64 payload_timestamp_ms(const QJsonObject& payload, qint64 fallback_ms) {
        for (const QString& key : {QStringLiteral("ts_ms"), QStringLiteral("timestamp"),
                                   QStringLiteral("created_time")}) {
            const QJsonValue value = payload.value(key);
            if (value.isUndefined() || value.isNull()) continue;
            bool ok = false;
            qint64 ts = value.isString() ? value.toString().toLongLong(&ok)
                                         : static_cast<qint64>(value.toDouble());
            if (!ok && value.isString()) continue;
            if (ts > 0 && ts < 10'000'000'000LL) ts *= 1000;
            if (ts > 0) return ts;
        }
        return fallback_ms;
    }

    static QString flow_outcome(const QJsonObject& payload) {
        const QString side = payload.value(QStringLiteral("taker_outcome_side")).toString(
            payload.value(QStringLiteral("outcome_side")).toString(
                payload.value(QStringLiteral("side")).toString())).trimmed().toLower();
        return side == QStringLiteral("yes") || side == QStringLiteral("no") ? side : QString{};
    }

    void trim_flow_buffers(const QString& ticker, qint64 now_ms) {
        constexpr qint64 kRetainMs = 60'000;
        auto& trades = flow_trades_[ticker];
        while (!trades.isEmpty() && trades.first().ts_ms < now_ms - kRetainMs)
            trades.removeFirst();
        auto& deltas = flow_deltas_[ticker];
        while (!deltas.isEmpty() && deltas.first().ts_ms < now_ms - kRetainMs)
            deltas.removeFirst();
    }

    void record_flow_trade(const QString& ticker, const QJsonObject& payload) {
        if (!tracks(ticker)) return;
        const QString outcome = flow_outcome(payload);
        const double quantity = payload.value(QStringLiteral("count_fp")).toDouble(
            payload.value(QStringLiteral("count")).toDouble());
        if (outcome.isEmpty() || quantity <= 0.0) return;
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        flow_trades_[ticker].append({payload_timestamp_ms(payload, now_ms), outcome, quantity});
        trim_flow_buffers(ticker, now_ms);
        update_flow_snapshot(ticker, now_ms);
        book_snapshot_flush_.start();
    }

    void record_flow_delta(const QString& ticker, const QString& type,
                           const QJsonObject& payload) {
        if (!tracks(ticker) || type != QStringLiteral("orderbook_delta")) return;
        const QString outcome = flow_outcome(payload);
        const double quantity_delta = payload.value(QStringLiteral("delta_fp")).toDouble();
        if (outcome.isEmpty() || quantity_delta == 0.0) return;
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        flow_deltas_[ticker].append(
            {payload_timestamp_ms(payload, now_ms), outcome, quantity_delta});
        trim_flow_buffers(ticker, now_ms);
        update_flow_snapshot(ticker, now_ms);
        book_snapshot_flush_.start();
    }

    void update_flow_snapshot(const QString& ticker, qint64 now_ms) {
        const auto yes_book = normalized_books_.value(ticker + QStringLiteral(":yes"));
        const auto no_book = normalized_books_.value(ticker + QStringLiteral(":no"));
        if (yes_book.asset_id.isEmpty() || no_book.asset_id.isEmpty()) return;
        QVector<KalshiFlowLevel> yes_levels;
        QVector<KalshiFlowLevel> no_levels;
        for (const auto& level : yes_book.bids) yes_levels.append({level.price, level.size});
        for (const auto& level : no_book.bids) no_levels.append({level.price, level.size});
        trim_flow_buffers(ticker, now_ms);
        QJsonObject snapshot = kalshi_flow_to_json(kalshi_flow_metrics(
            yes_levels, no_levels, flow_trades_.value(ticker), flow_deltas_.value(ticker), now_ms));
        snapshot.insert(QStringLiteral("ticker"), ticker);
        snapshot.insert(QStringLiteral("source"), QStringLiteral("kalshi_websocket"));
        flow_snapshots_.insert(ticker, snapshot);
    }

    void observe_market_event(const QString& type, const QString& ticker,
                              const QJsonObject& payload) {
        if (!tracks(ticker)) return;
        ++market_events_;
        if (type.contains(QStringLiteral("orderbook"))) ++orderbook_events_;
        else if (type == QStringLiteral("trade")) ++trade_events_;
        else if (type == QStringLiteral("ticker")) ++ticker_events_;
        last_event_type_ = type;
        last_event_ticker_ = ticker;
        last_event_at_ = now_utc();
        capture_exchange_timestamp(payload);
        status_dirty_ = true;
        // Raw depth deltas are retained for transport health and sequence
        // auditing. The normalized ws_orderbook_updated callback above wakes
        // the planner only when executable top-of-book state changes.
        if (!type.contains(QStringLiteral("orderbook"))) schedule_decision(type);
    }

    void schedule_decision(const QString& reason) {
        decision_pending_ = true;
        last_trigger_reason_ = reason;
        status_dirty_ = true;
        if (process_) return;
        const qint64 elapsed = last_cycle_started_ms_ > 0
            ? QDateTime::currentMSecsSinceEpoch() - last_cycle_started_ms_ : 10000;
        debounce_.start(static_cast<int>(kalshi_event_cycle_delay_ms(
            session_active(), parallel_paper_active(), elapsed)));
    }

    void maybe_start_work() {
        if (process_) return;
        if (account_reconcile_pending_) {
            account_reconcile_pending_ = false;
            start_process({QStringLiteral("kalshi"), QStringLiteral("auto"),
                           QStringLiteral("positions")}, Work::AccountReconcile);
            return;
        }
        if (decision_pending_) {
            const qint64 elapsed = last_cycle_started_ms_ > 0
                ? QDateTime::currentMSecsSinceEpoch() - last_cycle_started_ms_ : 10000;
            debounce_.start(static_cast<int>(kalshi_event_cycle_delay_ms(
                session_active(), parallel_paper_active(), elapsed)));
        }
    }

    void start_decision_cycle() {
        if (process_ || !decision_pending_) return;
        decision_pending_ = false;
        last_cycle_started_ms_ = QDateTime::currentMSecsSinceEpoch();
        ++decision_cycles_;
        start_process(kalshi_event_planner_args(), Work::Plan);
    }

    enum class Work { Plan, Paper, Execute, AccountReconcile };

    void start_process(QStringList command, Work work) {
        if (process_) return;
        process_ = new QProcess(this);
        active_work_ = work;
        process_started_ms_ = QDateTime::currentMSecsSinceEpoch();
        process_timed_out_ = false;
        QStringList args{QStringLiteral("--profile"), profile_, QStringLiteral("--json")};
        args.append(command);
        connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int exit_code, QProcess::ExitStatus status) {
                    const QByteArray standard_out = process_->readAllStandardOutput().trimmed();
                    const QByteArray standard_error = process_->readAllStandardError().trimmed();
                    const bool timed_out = process_timed_out_;
                    last_process_exit_ = exit_code;
                    last_process_at_ = now_utc();
                    const QJsonObject result = timed_out
                        ? QJsonObject{} : QJsonDocument::fromJson(standard_out).object();
                    if (timed_out) {
                        last_process_result_ = QStringLiteral("planner timed out; execution skipped");
                    } else if (!result.isEmpty()) {
                        const QString state = result.value(QStringLiteral("status")).toString(
                            result.value(QStringLiteral("event")).toString(QStringLiteral("ok")));
                        const QJsonObject portfolio = result.value(QStringLiteral("portfolio")).toObject();
                        last_process_result_ = QStringLiteral("%1; legs=%2; paper_only=%3")
                            .arg(state)
                            .arg(portfolio.value(QStringLiteral("legs")).toArray().size())
                            .arg(result.value(QStringLiteral("paper_only")).toBool() ?
                                     QStringLiteral("yes") : QStringLiteral("no"));
                    } else {
                        last_process_result_ = QString::fromUtf8(standard_out.left(512));
                    }
                    if (status != QProcess::NormalExit || exit_code != 0)
                        last_error_ = QString::fromUtf8(standard_error.left(1024));
                    else
                        last_error_.clear();
                    const Work completed = active_work_;
                    process_->deleteLater();
                    process_ = nullptr;
                    process_started_ms_ = 0;
                    process_timed_out_ = false;
                    status_dirty_ = true;

                    // A timed-out planner has no trustworthy candidate snapshot.
                    // Never let its completion fall through into live execution.
                    if (timed_out) {
                        maybe_start_work();
                        return;
                    }

                    if (completed == Work::Execute) {
                        record_live_result(result);
                        if (parallel_paper_active()) {
                            ++paper_cycles_;
                            start_process({QStringLiteral("sandbox"), QStringLiteral("tick")}, Work::Paper);
                            return;
                        }
                    }
                    // Live evaluation must run immediately after the planner.
                    // A paper subprocess can take longer than the five-second
                    // executable-quote lifetime and previously caused valid
                    // candidates to expire before execute-next inspected them.
                    if (completed == Work::Plan && session_active()) {
                        ++live_candidate_checks_;
                        start_process({QStringLiteral("kalshi"), QStringLiteral("auto"),
                                       QStringLiteral("live"), QStringLiteral("execute-next"),
                                       QStringLiteral("--max-stake"), QStringLiteral("2"),
                                       QStringLiteral("--experiment-cap"), QStringLiteral("120"),
                                       QStringLiteral("--require-session")}, Work::Execute);
                        return;
                    }
                    if (completed == Work::Plan && parallel_paper_active()) {
                        ++paper_cycles_;
                        start_process({QStringLiteral("sandbox"), QStringLiteral("tick")}, Work::Paper);
                        return;
                    }
                    if (completed == Work::AccountReconcile) ++account_reconciliations_;
                    maybe_start_work();
                });
        connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
            last_error_ = process_ ? process_->errorString() : QStringLiteral("process error");
            status_dirty_ = true;
        });
        process_->start(QCoreApplication::applicationFilePath(), args);
        status_dirty_ = true;
    }

    QString work_name(Work work) const {
        switch (work) {
        case Work::Plan: return QStringLiteral("plan");
        case Work::Paper: return QStringLiteral("paper_tick");
        case Work::Execute: return QStringLiteral("live_execute");
        case Work::AccountReconcile: return QStringLiteral("account_reconcile");
        }
        return QStringLiteral("unknown");
    }

    void check_process_timeout(qint64 now_ms, qint64 planner_timeout_ms,
                               qint64 paper_timeout_ms, qint64 account_timeout_ms) {
        if (!process_ || process_timed_out_) return;
        // An execution process may have reached the broker before it can report
        // the result. Do not kill it: reconciliation owns that uncertainty.
        qint64 timeout_ms = -1;
        if (active_work_ == Work::Plan) timeout_ms = planner_timeout_ms;
        else if (active_work_ == Work::Paper) timeout_ms = paper_timeout_ms;
        else if (active_work_ == Work::AccountReconcile) timeout_ms = account_timeout_ms;
        if (timeout_ms <= 0) return;
        const qint64 age_ms = process_started_ms_ > 0 ? now_ms - process_started_ms_ : -1;
        const bool timed_out = active_work_ == Work::Plan
            ? kalshi_planner_process_timed_out(true, age_ms, timeout_ms)
            : kalshi_non_execution_process_timed_out(true, age_ms, timeout_ms);
        if (!timed_out) return;
        process_timed_out_ = true;
        if (active_work_ == Work::Plan) ++planner_timeouts_;
        else ++non_execution_timeouts_;
        last_error_ = QStringLiteral("Kalshi %1 timed out after %2 ms; execution skipped")
                          .arg(work_name(active_work_)).arg(age_ms);
        status_dirty_ = true;
        process_->kill();
    }

    void record_live_result(const QJsonObject& result) {
        const QString state = result.value(QStringLiteral("status")).toString(
            QStringLiteral("unknown"));
        last_live_status_ = state;
        last_live_reason_ = result.value(QStringLiteral("reason")).toString();
        const QJsonObject candidate = result.value(QStringLiteral("candidate")).toObject();
        if (!candidate.isEmpty()) {
            ++live_candidates_found_;
            last_live_decision_id_ = candidate.value(QStringLiteral("decision_id")).toString();
        }
        const QJsonObject submission = result.value(QStringLiteral("submission")).toObject();
        if (!submission.isEmpty()) {
            ++live_submission_attempts_;
            const QString submission_state = submission.value(QStringLiteral("status")).toString();
            if (submission_state == QStringLiteral("submitted") ||
                submission_state == QStringLiteral("accepted") ||
                submission_state == QStringLiteral("open") ||
                submission_state == QStringLiteral("filled"))
                ++live_orders_accepted_;
            if (submission_state == QStringLiteral("filled") && state != QStringLiteral("filled"))
                ++live_orders_filled_;
        }
        if (state == QStringLiteral("filled")) ++live_orders_filled_;
        if (state != QStringLiteral("submitted") && state != QStringLiteral("accepted") &&
            state != QStringLiteral("open") && state != QStringLiteral("filled")) {
            QString reason = last_live_reason_.trimmed();
            if (reason.isEmpty()) reason = state;
            live_rejection_counts_[reason] += 1;
        }
    }

    void write_book_snapshot() {
        const QJsonObject payload{
            {QStringLiteral("schema"), 2},
            {QStringLiteral("updated_at_ms"),
             QString::number(QDateTime::currentMSecsSinceEpoch())},
            {QStringLiteral("books"), top_book_snapshots_},
            {QStringLiteral("flow"), flow_snapshots_}};
        QSaveFile file(kalshi_evidence_path(QStringLiteral("kalshi-ws-books.json")));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            last_error_ = QStringLiteral("Kalshi WebSocket book snapshot is not writable");
            status_dirty_ = true;
            return;
        }
        file.write(QJsonDocument(payload).toJson(QJsonDocument::Compact));
        if (!file.commit()) {
            last_error_ = QStringLiteral("Kalshi WebSocket book snapshot commit failed");
            status_dirty_ = true;
        }
    }

    QJsonObject live_rejection_counts_json() const {
        QJsonObject out;
        for (auto it = live_rejection_counts_.cbegin(); it != live_rejection_counts_.cend(); ++it)
            out.insert(it.key(), QString::number(it.value()));
        return out;
    }

    void write_status() {
        status_dirty_ = false;
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        const qint64 age_ms = event_age_ms(now_ms);
        const qint64 request_age_ms = universe_request_pending_ && universe_request_started_ms_ > 0
            ? now_ms - universe_request_started_ms_ : -1;
        const QJsonObject status{
            {QStringLiteral("schema"), 1},
            {QStringLiteral("mode"), QStringLiteral("websocket_event_driven")},
            {QStringLiteral("profile"), profile_},
            {QStringLiteral("credentials"), credentials_},
            {QStringLiteral("connected"), connected_},
            {QStringLiteral("stream_healthy"), stream_healthy_},
            {QStringLiteral("event_age_ms"), QString::number(age_ms)},
            {QStringLiteral("subscribed_assets"), subscribed_asset_ids_.size()},
            {QStringLiteral("subscribed_markets"), subscribed_asset_ids_.size() / 2},
            {QStringLiteral("market_events"), QString::number(market_events_)},
            {QStringLiteral("orderbook_events"), QString::number(orderbook_events_)},
            {QStringLiteral("trade_events"), QString::number(trade_events_)},
            {QStringLiteral("ticker_events"), QString::number(ticker_events_)},
            {QStringLiteral("account_events"), QString::number(account_events_)},
            {QStringLiteral("cf_benchmark_events"), QString::number(cf_benchmark_events_)},
            {QStringLiteral("latest_cf_benchmark"), latest_cf_benchmark_},
            {QStringLiteral("latest_cf_benchmark_ms"), QString::number(latest_cf_benchmark_ms_)},
            {QStringLiteral("independent_spot_events"), QString::number(independent_spot_events_)},
            {QStringLiteral("latest_independent_spot"), latest_independent_spot_},
            {QStringLiteral("latest_independent_spot_source"), latest_independent_spot_source_},
            {QStringLiteral("latest_independent_spot_age_ms"), QString::number(
                latest_independent_spot_ms_ > 0 ? std::max<qint64>(0, now_ms - latest_independent_spot_ms_) : -1)},
            {QStringLiteral("decision_cycles"), QString::number(decision_cycles_)},
            {QStringLiteral("paper_cycles"), QString::number(paper_cycles_)},
            {QStringLiteral("parallel_paper_enabled"), parallel_paper_active()},
            {QStringLiteral("cycle_min_interval_ms"), QString::number(
                kalshi_event_cycle_delay_ms(session_active(), parallel_paper_active(), 0))},
            // Compatibility alias retained for older status readers. It now
            // means candidate checks, not claimed order submissions.
            {QStringLiteral("execution_attempts"), QString::number(live_candidate_checks_)},
            {QStringLiteral("live_candidate_checks"), QString::number(live_candidate_checks_)},
            {QStringLiteral("live_candidates_found"), QString::number(live_candidates_found_)},
            {QStringLiteral("live_submission_attempts"), QString::number(live_submission_attempts_)},
            {QStringLiteral("live_orders_accepted"), QString::number(live_orders_accepted_)},
            {QStringLiteral("live_orders_filled"), QString::number(live_orders_filled_)},
            {QStringLiteral("live_rejection_counts"), live_rejection_counts_json()},
            {QStringLiteral("last_live_status"), last_live_status_},
            {QStringLiteral("last_live_reason"), last_live_reason_},
            {QStringLiteral("last_live_decision_id"), last_live_decision_id_},
            {QStringLiteral("account_reconciliations"), QString::number(account_reconciliations_)},
            {QStringLiteral("last_sequence"), QString::number(last_sequence_)},
            {QStringLiteral("last_event_at"), last_event_at_},
            {QStringLiteral("last_event_type"), last_event_type_},
            {QStringLiteral("last_event_ticker"), last_event_ticker_},
            {QStringLiteral("last_exchange_timestamp"), last_exchange_timestamp_},
            {QStringLiteral("last_trigger_reason"), last_trigger_reason_},
            {QStringLiteral("last_universe_refresh"), last_universe_refresh_},
            {QStringLiteral("universe_request_pending"), universe_request_pending_},
            {QStringLiteral("universe_request_age_ms"), QString::number(request_age_ms)},
            {QStringLiteral("universe_timeouts"), QString::number(universe_timeouts_)},
            {QStringLiteral("reconnect_attempts"), QString::number(reconnect_attempts_)},
            {QStringLiteral("recovery_decisions"), QString::number(recovery_decisions_)},
            {QStringLiteral("session_wakeups"), QString::number(session_wakeups_)},
            {QStringLiteral("session_active"), session_active()},
            {QStringLiteral("process_active"), process_ != nullptr},
            {QStringLiteral("process_work"), process_ ? work_name(active_work_) : QString()},
            {QStringLiteral("process_age_ms"), QString::number(
                process_ && process_started_ms_ > 0
                    ? std::max<qint64>(0, now_ms - process_started_ms_) : -1)},
            {QStringLiteral("planner_timeouts"), QString::number(planner_timeouts_)},
            {QStringLiteral("non_execution_timeouts"), QString::number(non_execution_timeouts_)},
            {QStringLiteral("last_process_at"), last_process_at_},
            {QStringLiteral("last_process_exit"), last_process_exit_},
            {QStringLiteral("last_process_result"), last_process_result_},
            {QStringLiteral("last_error"), last_error_},
            {QStringLiteral("updated_at"), now_utc()}};
        QSaveFile file(kalshi_evidence_path(QStringLiteral("kalshi-ws-engine.json")));
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            file.write(QJsonDocument(status).toJson(QJsonDocument::Indented));
            file.commit();
        }
    }

    QString profile_;
    CryptoFeedHub* crypto_feed_hub_ = nullptr;
    KalshiAdapter* adapter_ = nullptr;
    QMetaObject::Connection spot_feed_connection_;
    QSet<QString> subscribed_asset_ids_;
    QHash<QString, QString> top_book_signatures_;
    QTimer debounce_;
    QTimer status_flush_;
    QTimer universe_refresh_;
    QTimer health_watchdog_;
    QTimer book_snapshot_flush_;
    QProcess* process_ = nullptr;
    Work active_work_ = Work::Plan;
    bool credentials_ = false;
    bool connected_ = false;
    bool universe_request_pending_ = false;
    bool session_was_active_ = false;
    bool stream_healthy_ = false;
    bool decision_pending_ = false;
    bool account_reconcile_pending_ = false;
    bool status_dirty_ = true;
    qint64 last_cycle_started_ms_ = 0;
    qint64 process_started_ms_ = 0;
    qint64 universe_request_started_ms_ = 0;
    qint64 subscription_started_ms_ = 0;
    qint64 last_reconnect_attempt_ms_ = 0;
    qint64 last_recovery_decision_ms_ = 0;
    qint64 market_events_ = 0;
    qint64 orderbook_events_ = 0;
    qint64 trade_events_ = 0;
    qint64 ticker_events_ = 0;
    qint64 account_events_ = 0;
    qint64 cf_benchmark_events_ = 0;
    qint64 independent_spot_events_ = 0;
    qint64 latest_cf_benchmark_ms_ = 0;
    qint64 latest_independent_spot_ms_ = 0;
    double latest_cf_benchmark_ = 0.0;
    double latest_independent_spot_ = 0.0;
    qint64 decision_cycles_ = 0;
    qint64 paper_cycles_ = 0;
    qint64 live_candidate_checks_ = 0;
    qint64 live_candidates_found_ = 0;
    qint64 live_submission_attempts_ = 0;
    qint64 live_orders_accepted_ = 0;
    qint64 live_orders_filled_ = 0;
    qint64 account_reconciliations_ = 0;
    qint64 universe_timeouts_ = 0;
    qint64 reconnect_attempts_ = 0;
    qint64 recovery_decisions_ = 0;
    qint64 session_wakeups_ = 0;
    qint64 planner_timeouts_ = 0;
    qint64 non_execution_timeouts_ = 0;
    qint64 last_sequence_ = 0;
    int last_process_exit_ = 0;
    bool process_timed_out_ = false;
    QString last_event_at_;
    QString last_event_type_;
    QString last_event_ticker_;
    QString last_exchange_timestamp_;
    QString last_trigger_reason_;
    QString last_universe_refresh_;
    QString last_process_at_;
    QString last_process_result_;
    QHash<QString, qint64> live_rejection_counts_;
    QHash<QString, qint64> independent_spot_persisted_ms_;
    QJsonObject top_book_snapshots_;
    QHash<QString, services::prediction::PredictionOrderBook> normalized_books_;
    QHash<QString, QVector<KalshiFlowTrade>> flow_trades_;
    QHash<QString, QVector<KalshiFlowDelta>> flow_deltas_;
    QJsonObject flow_snapshots_;
    QString last_live_status_;
    QString last_live_reason_;
    QString last_live_decision_id_;
    QString latest_independent_spot_source_;
    QString last_error_;
};

QDateTime parse_utc(const QString& iso) {
    QDateTime dt = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(iso, Qt::ISODate);
    return dt.isValid() ? dt.toUTC() : dt;
}

QStringList json_array_to_strings(const QJsonArray& arr) {
    QStringList out;
    for (const auto& v : arr)
        out << v.toString();
    return out;
}

QJsonArray strings_to_json_array(const QStringList& list) {
    QJsonArray arr;
    for (const auto& s : list)
        arr.append(s);
    return arr;
}

QString tail_text(const QString& path, int lines) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QStringList all = QString::fromUtf8(f.readAll()).split('\n');
    while (!all.isEmpty() && all.last().isEmpty())
        all.removeLast();
    if (lines > 0 && all.size() > lines)
        all = all.mid(all.size() - lines);
    return all.join('\n');
}

void append_job_log(const QString& profile, const QString& line) {
    QDir().mkpath(daemon_logs_dir(profile));
    QFile f(daemon_job_log_path(profile));
    if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << now_utc() << " " << line << "\n";
    }
}

[[maybe_unused]] QString launch_domain() {
#if defined(Q_OS_MACOS)
    return QStringLiteral("gui/%1").arg(static_cast<unsigned long>(::getuid()));
#else
    return {};
#endif
}

struct ProcessResult {
    int exit_code = -1;
    QString out;
    QString err;
};

ProcessResult run_process(const QString& program, const QStringList& args, int timeout_ms = 10000) {
    QProcess p;
    p.start(program, args);
    if (!p.waitForStarted(3000))
        return {-1, {}, QStringLiteral("could not start %1").arg(program)};
    if (!p.waitForFinished(timeout_ms)) {
        p.kill();
        p.waitForFinished(1000);
        return {-1, QString::fromUtf8(p.readAllStandardOutput()),
                QStringLiteral("%1 timed out").arg(program)};
    }
    return {p.exitCode(), QString::fromUtf8(p.readAllStandardOutput()),
            QString::fromUtf8(p.readAllStandardError())};
}

QJsonObject empty_jobs_doc() {
    return QJsonObject{{"schema", 1}, {"jobs", QJsonArray{}}};
}

int default_job_timeout_sec(const QString& kind, int every_sec = 0) {
    const QString k = kind.trimmed().toLower();
    int base = 300;
    if (k == "health-check")
        base = 30;
    else if (k == "notify")
        base = 60;
    else if (k == "brief" || k == "risk" || k == "thesis" || k == "radar" || k == "ai")
        base = 180;
    else if (k == "notebook" || k == "paper-strategy")
        base = 600;
    else if (k == "chronos2" || k == "chronos2-equity")
        base = 300;
    if (every_sec > 0)
        base = std::min(base, std::max(30, every_sec - 1));
    return base;
}

int job_timeout_sec(const QJsonObject& job) {
    const int explicit_timeout = job.value("timeout_sec").toInt(0);
    if (explicit_timeout > 0)
        return explicit_timeout;
    return default_job_timeout_sec(job.value("kind").toString(),
                                   job.value("interval_sec").toInt(0));
}

QJsonObject load_jobs_doc(const QString& profile) {
    QFile f(daemon_jobs_path(profile));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return empty_jobs_doc();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return empty_jobs_doc();
    QJsonObject o = doc.object();
    if (!o.value("jobs").isArray())
        o["jobs"] = QJsonArray{};
    o["schema"] = 1;
    return o;
}

bool save_jobs_doc(const QString& profile, const QJsonObject& doc, QString* error = nullptr) {
    QDir().mkpath(daemon_state_dir(profile));
    QSaveFile f(daemon_jobs_path(profile));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("could not write jobs file");
        return false;
    }
    f.write(QJsonDocument(doc).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error) *error = QStringLiteral("could not commit jobs file");
        return false;
    }
    QFile::setPermissions(daemon_jobs_path(profile), QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

int find_job_index(const QJsonArray& jobs, const QString& id_or_name) {
    for (int i = 0; i < jobs.size(); ++i) {
        const QJsonObject j = jobs.at(i).toObject();
        if (j.value("id").toString() == id_or_name ||
            j.value("name").toString().compare(id_or_name, Qt::CaseInsensitive) == 0)
            return i;
    }
    for (int i = 0; i < jobs.size(); ++i) {
        const QJsonObject j = jobs.at(i).toObject();
        if (j.value("id").toString().contains(id_or_name, Qt::CaseInsensitive) ||
            j.value("name").toString().contains(id_or_name, Qt::CaseInsensitive))
            return i;
    }
    return -1;
}

QString compact_tail(const QString& s, int max_chars = 2000) {
    QString out = s.trimmed();
    if (out.size() > max_chars)
        out = out.right(max_chars);
    return out;
}

QJsonObject read_json_object_file(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    return err.error == QJsonParseError::NoError && doc.isObject() ? doc.object() : QJsonObject{};
}

bool write_json_object_file(const QString& path, const QJsonObject& obj, QString* error = nullptr) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("could not write %1").arg(path);
        return false;
    }
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error) *error = QStringLiteral("could not commit %1").arg(path);
        return false;
    }
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

struct DaemonHistoryDb {
    QString connection_name;
    QSqlDatabase db;
    QString error;

    explicit DaemonHistoryDb(const QString& profile)
        : connection_name(QStringLiteral("daemon-history-%1-%2")
                              .arg(daemon_slug(profile), QUuid::createUuid().toString(QUuid::Id128))) {
        QDir().mkpath(daemon_state_dir(profile));
        db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        db.setDatabaseName(daemon_history_db_path(profile));
        if (!db.open()) {
            error = db.lastError().text();
            return;
        }
        QSqlQuery pragma(db);
        pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
        pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
        QSqlQuery q(db);
        if (!q.exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS daemon_job_runs ("
                "run_id TEXT PRIMARY KEY,"
                "job_id TEXT NOT NULL,"
                "job_name TEXT,"
                "kind TEXT,"
                "trigger TEXT,"
                "command TEXT,"
                "status TEXT,"
                "exit_code INTEGER,"
                "started_at TEXT,"
                "finished_at TEXT,"
                "duration_ms INTEGER,"
                "timeout_sec INTEGER,"
                "pid INTEGER,"
                "daemon_pid INTEGER,"
                "stdout_tail TEXT,"
                "stderr_tail TEXT,"
                "error TEXT"
                ")"))) {
            error = q.lastError().text();
            return;
        }
        q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_daemon_job_runs_started "
                              "ON daemon_job_runs(started_at DESC)"));
        q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_daemon_job_runs_job "
                              "ON daemon_job_runs(job_id, started_at DESC)"));
        q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_daemon_job_runs_status "
                              "ON daemon_job_runs(status, started_at DESC)"));
    }

    ~DaemonHistoryDb() {
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection_name);
    }

    bool ok() const { return db.isOpen() && error.isEmpty(); }
};

void bind_run_identity(QSqlQuery& q, const QJsonObject& job) {
    q.bindValue(QStringLiteral(":job_id"), job.value("id").toString());
    q.bindValue(QStringLiteral(":job_name"), job.value("name").toString());
    q.bindValue(QStringLiteral(":kind"), job.value("kind").toString());
    q.bindValue(QStringLiteral(":command"), json_array_to_strings(job.value("command").toArray()).join(' '));
    q.bindValue(QStringLiteral(":timeout_sec"), job_timeout_sec(job));
    q.bindValue(QStringLiteral(":daemon_pid"),
                static_cast<qlonglong>(QCoreApplication::applicationPid()));
}

QString record_job_run_start(const QString& profile,
                             const QJsonObject& job,
                             const QString& trigger,
                             const QString& started_at = now_utc()) {
    DaemonHistoryDb h(profile);
    if (!h.ok()) {
        append_job_log(profile, QStringLiteral("history-error phase=start error=\"%1\"").arg(h.error));
        return {};
    }
    const QString run_id = QStringLiteral("run_%1").arg(QUuid::createUuid().toString(QUuid::Id128).left(16));
    QSqlQuery q(h.db);
    q.prepare(QStringLiteral(
        "INSERT INTO daemon_job_runs "
        "(run_id, job_id, job_name, kind, trigger, command, status, started_at, timeout_sec, daemon_pid) "
        "VALUES (:run_id, :job_id, :job_name, :kind, :trigger, :command, 'running', :started_at, :timeout_sec, :daemon_pid)"));
    q.bindValue(QStringLiteral(":run_id"), run_id);
    bind_run_identity(q, job);
    q.bindValue(QStringLiteral(":trigger"), trigger);
    q.bindValue(QStringLiteral(":started_at"), started_at);
    if (!q.exec()) {
        append_job_log(profile, QStringLiteral("history-error phase=start id=%1 error=\"%2\"")
                                    .arg(job.value("id").toString(), q.lastError().text()));
        return {};
    }
    return run_id;
}

void record_job_run_finish(const QString& profile,
                           const QString& run_id,
                           const QString& status,
                           int exit_code,
                           const QString& out,
                           const QString& err,
                           const QString& error_text = {}) {
    if (run_id.isEmpty())
        return;
    DaemonHistoryDb h(profile);
    if (!h.ok()) {
        append_job_log(profile, QStringLiteral("history-error phase=finish run=%1 error=\"%2\"").arg(run_id, h.error));
        return;
    }
    const QString finished = now_utc();
    QSqlQuery started_q(h.db);
    started_q.prepare(QStringLiteral("SELECT started_at FROM daemon_job_runs WHERE run_id=:run_id"));
    started_q.bindValue(QStringLiteral(":run_id"), run_id);
    qint64 duration_ms = -1;
    if (started_q.exec() && started_q.next()) {
        const QDateTime started = parse_utc(started_q.value(0).toString());
        const QDateTime finished_dt = parse_utc(finished);
        if (started.isValid() && finished_dt.isValid())
            duration_ms = started.msecsTo(finished_dt);
    }
    QSqlQuery q(h.db);
    q.prepare(QStringLiteral(
        "UPDATE daemon_job_runs SET "
        "status=:status, exit_code=:exit_code, finished_at=:finished_at, duration_ms=:duration_ms, "
        "stdout_tail=:stdout_tail, stderr_tail=:stderr_tail, error=:error "
        "WHERE run_id=:run_id"));
    q.bindValue(QStringLiteral(":status"), status);
    q.bindValue(QStringLiteral(":exit_code"), exit_code);
    q.bindValue(QStringLiteral(":finished_at"), finished);
    q.bindValue(QStringLiteral(":duration_ms"), duration_ms >= 0 ? QVariant(duration_ms) : QVariant());
    q.bindValue(QStringLiteral(":stdout_tail"), compact_tail(out));
    q.bindValue(QStringLiteral(":stderr_tail"), compact_tail(err));
    q.bindValue(QStringLiteral(":error"), error_text);
    q.bindValue(QStringLiteral(":run_id"), run_id);
    if (!q.exec()) {
        append_job_log(profile, QStringLiteral("history-error phase=finish run=%1 error=\"%2\"")
                                    .arg(run_id, q.lastError().text()));
    }
}

} // namespace

QStringList command_for_job_kind(const QString& kind, const QJsonObject& spec) {
    const QString k = kind.trimmed().toLower();
    if (k == "command")
        return json_array_to_strings(spec.value("command").toArray());
    if (k == "brief" || k == "risk" || k == "thesis" || k == "radar")
        return {k, spec.value("target").toString()};
    if (k == "ai") {
        const QString workflow = spec.value("workflow").toString("brief").trimmed().toLower();
        return {workflow, spec.value("target").toString()};
    }
    if (k == "notebook") {
        QStringList args{QStringLiteral("notebook"), QStringLiteral("run"), spec.value("selector").toString()};
        const int cell = spec.value("cell").toInt(0);
        if (cell > 0)
            args << QStringLiteral("--cell") << QString::number(cell);
        return args;
    }
    if (k == "paper-strategy") {
        QStringList args{QStringLiteral("strategy"), QStringLiteral("paper-run"),
                         spec.value("strategy").toString("meanrev")};
        const QString symbols = spec.value("symbols").toString();
        if (!symbols.isEmpty())
            args << QStringLiteral("--symbols") << symbols;
        const int max_iters = spec.value("max_iters").toInt(1);
        args << QStringLiteral("--max-iters") << QString::number(max_iters);
        args << QStringLiteral("--interval-sec") << QString::number(spec.value("interval_sec").toInt(0));
        const auto append_cap = [&](const char* spec_key, const char* flag) {
            bool ok = false;
            const double v = spec.value(QLatin1String(spec_key)).toString().toDouble(&ok);
            if (ok && v > 0.0)
                args << QLatin1String(flag) << QString::number(v);
        };
        append_cap("max_notional", "--max-notional-per-order");
        append_cap("max_position", "--max-position-qty");
        append_cap("max_aggregate", "--max-aggregate-qty");
        return args;
    }
    if (k == "chronos2") {
        QStringList args{QStringLiteral("edge"), QStringLiteral("chronos2"), QStringLiteral("forecast"),
                         spec.value("symbol").toString(QStringLiteral("BTC-USD")),
                         QStringLiteral("--horizon"), spec.value("horizon").toString(QStringLiteral("15m")),
                         QStringLiteral("--journal"),
                         QStringLiteral("--min-journal-edge-bps"),
                         QString::number(spec.value("min_journal_edge_bps").toDouble(15.0), 'f', 2)};
        const QString model = spec.value("model").toString();
        if (!model.isEmpty())
            args << QStringLiteral("--model") << model;
        const QString device = spec.value("device").toString();
        if (!device.isEmpty())
            args << QStringLiteral("--device") << device;
        const int context_limit = spec.value("context_limit").toInt(0);
        if (context_limit > 0)
            args << QStringLiteral("--context-limit") << QString::number(context_limit);
        if (spec.value("publish").toBool(false))
            args << QStringLiteral("--publish");
        return args;
    }
    if (k == "chronos2-equity") {
        QStringList args{QStringLiteral("edge"), QStringLiteral("chronos2"), QStringLiteral("equity"),
                         spec.value("symbol").toString(QStringLiteral("AAPL")),
                         QStringLiteral("--horizon"), spec.value("horizon").toString(QStringLiteral("1d")),
                         QStringLiteral("--period"), spec.value("period").toString(QStringLiteral("2y")),
                         QStringLiteral("--journal"),
                         QStringLiteral("--min-journal-edge-bps"),
                         QString::number(spec.value("min_journal_edge_bps").toDouble(50.0), 'f', 2)};
        const QString model = spec.value("model").toString();
        if (!model.isEmpty())
            args << QStringLiteral("--model") << model;
        const QString device = spec.value("device").toString();
        if (!device.isEmpty())
            args << QStringLiteral("--device") << device;
        const QString interval = spec.value("interval").toString();
        if (!interval.isEmpty())
            args << QStringLiteral("--interval") << interval;
        const int context_limit = spec.value("context_limit").toInt(0);
        if (context_limit > 0)
            args << QStringLiteral("--context-limit") << QString::number(context_limit);
        if (spec.value("publish").toBool(false))
            args << QStringLiteral("--publish");
        return args;
    }
    if (k == "notify") {
        QStringList args{QStringLiteral("notify"), QStringLiteral("send")};
        const QString provider = spec.value("provider").toString();
        if (!provider.isEmpty())
            args << QStringLiteral("--provider") << provider;
        else
            args << QStringLiteral("--all");
        args << QStringLiteral("--title") << spec.value("title").toString()
             << QStringLiteral("--message") << spec.value("message").toString()
             << QStringLiteral("--level") << spec.value("level").toString("info")
             << QStringLiteral("--yes");
        return args;
    }
    if (k == "health-check")
        return {QStringLiteral("daemon"), QStringLiteral("health")};
    return {};
}

namespace {

QJsonObject make_job(const QString& kind,
                     const QString& name,
                     const QJsonObject& spec,
                     int every_sec,
                     int timeout_sec,
                     bool enabled = true) {
    QStringList command = command_for_job_kind(kind, spec);
    const QString id = QStringLiteral("job_%1").arg(QUuid::createUuid().toString(QUuid::Id128).left(12));
    const QString created = now_utc();
    const int effective_timeout = timeout_sec > 0 ? timeout_sec : default_job_timeout_sec(kind, every_sec);
    return QJsonObject{{"id", id},
                       {"name", name.trimmed().isEmpty() ? kind + QStringLiteral(" ") + id.right(4) : name.trimmed()},
                       {"kind", kind},
                       {"enabled", enabled},
                       {"schedule", every_sec > 0 ? QStringLiteral("interval") : QStringLiteral("manual")},
                       {"interval_sec", every_sec},
                       {"timeout_sec", effective_timeout},
                       {"next_run_at", every_sec > 0 ? created : QString()},
                       {"running", false},
                       {"run_count", 0},
                       {"fail_count", 0},
                       {"created_at", created},
                       {"updated_at", created},
                       {"spec", spec},
                       {"command", strings_to_json_array(command)}};
}

ProcessResult run_job_once_sync(const QString& profile, const QJsonObject& job, int timeout_ms = 0) {
    QStringList args{QStringLiteral("--profile"), profile};
    args << json_array_to_strings(job.value("command").toArray());
    if (timeout_ms <= 0)
        timeout_ms = std::max(1, job_timeout_sec(job)) * 1000;
    return run_process(QCoreApplication::applicationFilePath(), args, timeout_ms);
}

bool job_has_current_failure(const QJsonObject& job) {
    if (job.value("running").toBool())
        return false;
    const QString status = job.value("last_status").toString().trimmed().toLower();
    return status == QStringLiteral("failed") ||
           status == QStringLiteral("timeout") ||
           status == QStringLiteral("stale-timeout");
}

QJsonObject jobs_summary(const QString& profile) {
    const QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
    int enabled = 0, running = 0, failed = 0, failed_history = 0, interval = 0, stale = 0;
    int disabled_current_failures = 0, historical_only = 0;
    QJsonArray by_kind;
    QJsonObject counts;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const auto& v : jobs) {
        const QJsonObject j = v.toObject();
        const bool is_enabled = j.value("enabled").toBool();
        if (is_enabled) ++enabled;
        if (j.value("running").toBool()) ++running;
        if (j.value("fail_count").toInt() > 0) ++failed_history;
        if (job_has_current_failure(j)) {
            if (is_enabled)
                ++failed;
            else
                ++disabled_current_failures;
        } else if (j.value("fail_count").toInt() > 0) {
            ++historical_only;
        }
        if (j.value("schedule").toString() == QStringLiteral("interval")) ++interval;
        if (j.value("running").toBool()) {
            const QDateTime started = parse_utc(j.value("last_started_at").toString());
            if (started.isValid() && started.addSecs(job_timeout_sec(j) + 15) < now)
                ++stale;
        }
        const QString kind = j.value("kind").toString("unknown");
        counts[kind] = counts.value(kind).toInt() + 1;
    }
    for (auto it = counts.begin(); it != counts.end(); ++it)
        by_kind.append(QJsonObject{{"kind", it.key()}, {"count", it.value().toInt()}});
    return QJsonObject{{"total", jobs.size()},
                       {"enabled", enabled},
                       {"running", running},
                       {"failed", failed},
                       {"failed_history", failed_history},
                       {"historical_only", historical_only},
                       {"disabled_current_failures", disabled_current_failures},
                       {"stale", stale},
                       {"interval", interval},
                       {"by_kind", by_kind}};
}

bool launchd_loaded(const QString& profile) {
#if defined(Q_OS_MACOS)
    const ProcessResult r = run_process(QStringLiteral("launchctl"),
                                        {QStringLiteral("print"), launch_domain() + "/" + daemon_label(profile)},
                                        5000);
    return r.exit_code == 0;
#else
    Q_UNUSED(profile);
    return false;
#endif
}

void write_plist_key_string(QXmlStreamWriter& w, const QString& key, const QString& value) {
    w.writeTextElement(QStringLiteral("key"), key);
    w.writeTextElement(QStringLiteral("string"), value);
}

void write_plist_key_bool(QXmlStreamWriter& w, const QString& key, bool value) {
    w.writeTextElement(QStringLiteral("key"), key);
    w.writeEmptyElement(value ? QStringLiteral("true") : QStringLiteral("false"));
}

[[maybe_unused]] bool write_daemon_plist(const QString& profile, QString* error) {
    const QString path = daemon_plist_path(profile);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QDir().mkpath(daemon_logs_dir(profile));

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("could not write LaunchAgent plist: ") + path;
        return false;
    }

    QXmlStreamWriter w(&f);
    w.setAutoFormatting(true);
    w.writeStartDocument(QStringLiteral("1.0"));
    w.writeDTD(QStringLiteral("<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                              "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"));
    w.writeStartElement(QStringLiteral("plist"));
    w.writeAttribute(QStringLiteral("version"), QStringLiteral("1.0"));
    w.writeStartElement(QStringLiteral("dict"));

    write_plist_key_string(w, QStringLiteral("Label"), daemon_label(profile));
    w.writeTextElement(QStringLiteral("key"), QStringLiteral("ProgramArguments"));
    w.writeStartElement(QStringLiteral("array"));
    w.writeTextElement(QStringLiteral("string"), QCoreApplication::applicationFilePath());
    w.writeTextElement(QStringLiteral("string"), QStringLiteral("--profile"));
    w.writeTextElement(QStringLiteral("string"), profile);
    w.writeTextElement(QStringLiteral("string"), QStringLiteral("serve"));
    w.writeEndElement();

    write_plist_key_bool(w, QStringLiteral("RunAtLoad"), true);
    w.writeTextElement(QStringLiteral("key"), QStringLiteral("KeepAlive"));
    w.writeStartElement(QStringLiteral("dict"));
    write_plist_key_bool(w, QStringLiteral("Crashed"), true);
    w.writeEndElement();
    write_plist_key_string(w, QStringLiteral("WorkingDirectory"), QCoreApplication::applicationDirPath());
    write_plist_key_string(w, QStringLiteral("ProcessType"), QStringLiteral("Background"));
    write_plist_key_string(w, QStringLiteral("StandardOutPath"),
                           daemon_logs_dir(profile) + QStringLiteral("/daemon.out.log"));
    write_plist_key_string(w, QStringLiteral("StandardErrorPath"),
                           daemon_logs_dir(profile) + QStringLiteral("/daemon.err.log"));

    w.writeEndElement(); // dict
    w.writeEndElement(); // plist
    w.writeEndDocument();
    f.close();
    return true;
}

QJsonObject read_daemon_runtime(const QString& profile) {
    QFile f(daemon_runtime_path(profile));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return {};
    return doc.object();
}

bool daemon_runtime_live(const QJsonObject& rt) {
    const qint64 pid = static_cast<qint64>(rt.value(QStringLiteral("pid")).toDouble());
    return pid > 0 && is_pid_alive(pid);
}

bool write_daemon_runtime(const QString& profile,
                          const QString& mode,
                          const QString& endpoint = {},
                          const QString& bridge_owner_kind = {},
                          const QString& bridge_owner_endpoint = {},
                          const QJsonArray& crypto_feeds = {}) {
    QDir().mkpath(daemon_state_dir(profile));
    QJsonObject old = read_daemon_runtime(profile);
    QJsonObject rt{{"schema", 1},
                   {"profile", profile},
                   {"pid", QCoreApplication::applicationPid()},
                   {"mode", mode},
                   {"scheduler_running", true},
                   {"heartbeat_at", now_utc()}};
    rt["started_at"] = old.value("pid").toDouble() == QCoreApplication::applicationPid()
                           ? old.value("started_at").toString(now_utc())
                           : now_utc();
    if (!endpoint.isEmpty())
        rt["endpoint"] = endpoint;
    if (!bridge_owner_kind.isEmpty())
        rt["bridge_owner_kind"] = bridge_owner_kind;
    if (!bridge_owner_endpoint.isEmpty())
        rt["bridge_owner_endpoint"] = bridge_owner_endpoint;
    // Crypto feed health (Task 4's CryptoFeedHub::feed_health()) is only
    // available once the hub exists in the serve loop; until then, preserve
    // whatever was last written so the key doesn't flicker away between
    // heartbeats.
    if (!crypto_feeds.isEmpty())
        rt["crypto_feeds"] = crypto_feeds;
    else if (old.contains(QStringLiteral("crypto_feeds")))
        rt["crypto_feeds"] = old.value(QStringLiteral("crypto_feeds"));

    QSaveFile f(daemon_runtime_path(profile));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    f.write(QJsonDocument(rt).toJson(QJsonDocument::Indented));
    if (!f.commit())
        return false;
    QFile::setPermissions(daemon_runtime_path(profile), QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

bool remove_daemon_runtime(const QString& profile) {
    const QString path = daemon_runtime_path(profile);
    return !QFile::exists(path) || QFile::remove(path);
}

QJsonObject daemon_status_object(const QString& profile) {
    auto info = read_bridge_file(profile_root_for(profile));
    const bool owner_live = info && is_pid_alive(info->pid);
    const QJsonObject runtime = read_daemon_runtime(profile);
    const bool runtime_live = daemon_runtime_live(runtime);
    const QString runtime_mode = runtime_live ? runtime.value("mode").toString() : QString();
    const bool bridge_daemon_owner = owner_live && info->kind == QStringLiteral("daemon");
    const bool scheduler_running = runtime_live || bridge_daemon_owner;
    const bool running = scheduler_running;
    const QString owner_kind = owner_live ? info->kind : QString();
    const QString mode = bridge_daemon_owner ? QStringLiteral("daemon-owner")
                       : runtime_live && owner_live ? QStringLiteral("%1-owner+daemon-warm").arg(owner_kind)
                       : runtime_live ? QStringLiteral("daemon-warm")
                       : owner_live ? QStringLiteral("%1-owner").arg(owner_kind)
                       : info ? QStringLiteral("stale-owner")
                              : QStringLiteral("idle");
    QJsonObject o{{"profile", profile},
                  {"label", daemon_label(profile)},
                  {"plist", daemon_plist_path(profile)},
                  {"installed", QFileInfo::exists(daemon_plist_path(profile))},
                  {"loaded", launchd_loaded(profile)},
                  {"running", running},
                  {"daemon_running", running},
                  {"daemon_bridge_owner", bridge_daemon_owner},
                  {"daemon_warm", runtime_live && !bridge_daemon_owner},
                  {"scheduler_running", scheduler_running},
                  {"profile_owner_reachable", owner_live},
                  {"active_owner_kind", owner_kind},
                  {"mode", mode},
                  {"runtime", runtime},
                  {"can_start_daemon", !scheduler_running}};
    if (owner_live) {
        o["owner_kind"] = info->kind;
        o["active_owner_pid"] = info->pid;
        o["active_endpoint"] = info->endpoint;
        o["pid"] = info->pid;
        o["endpoint"] = info->endpoint;
        if (!bridge_daemon_owner)
            o["daemon_blocked_by"] = QStringLiteral("profile is currently owned by %1").arg(info->kind);
    } else if (info) {
        o["daemon_blocked_by"] = QStringLiteral("stale bridge owner metadata");
    }
    if (runtime_live) {
        o["daemon_process_pid"] = runtime.value("pid");
        o["daemon_process_mode"] = runtime_mode;
        o["daemon_heartbeat_at"] = runtime.value("heartbeat_at");
        o["daemon_started_at"] = runtime.value("started_at");
        o["crypto_feeds"] = runtime.value("crypto_feeds");
    }
    return o;
}

[[maybe_unused]] bool wait_for_daemon_running(const QString& profile, int timeout_ms) {
    const int sleep_ms = 250;
    const int tries = std::max(1, timeout_ms / sleep_ms);
    for (int i = 0; i < tries; ++i) {
        if (daemon_status_object(profile).value("running").toBool())
            return true;
        QThread::msleep(sleep_ms);
    }
    return daemon_status_object(profile).value("running").toBool();
}

QStringList split_csv(QString raw) {
    raw.replace(';', ',');
    QStringList out;
    for (QString part : raw.split(',', Qt::SkipEmptyParts)) {
        part = part.trimmed();
        if (!part.isEmpty())
            out << part;
    }
    return out;
}

QStringList scalp_sources_for_symbol(QStringList sources, const QString& symbol) {
    if (sources.isEmpty())
        sources = services::crypto_latency::CryptoLatencyService::default_sources();
    const QString normalized = services::crypto_latency::CryptoLatencyService::normalize_symbol(symbol);
    QStringList out;
    for (QString source : sources) {
        source = source.trimmed().toLower();
        if (source.isEmpty())
            continue;
        if (source == QStringLiteral("bitcointicker") && normalized != QStringLiteral("BTC-USD"))
            continue;
        if (!out.contains(source))
            out << source;
    }
    return out;
}

struct ScalpVenueFeeProfile {
    QString key;
    double maker_bps = 0.0;
    double taker_bps = 0.0;
    QString source;
};

struct ScalpCoinbaseFeeTier {
    const char* key;
    const char* volume;
    double maker_bps;
    double taker_bps;
};

static constexpr ScalpCoinbaseFeeTier kScalpCoinbaseFeeTiers[] = {
    {"coinbase_advanced", "$0K-$10K", 40.0, 60.0},
    {"coinbase_tier2", "$10K-$50K", 25.0, 40.0},
    {"coinbase_tier3", "$50K-$100K", 15.0, 25.0},
    {"coinbase_tier4", "$100K-$1M", 10.0, 20.0},
    {"coinbase_tier5", "$1M-$15M", 8.0, 18.0},
    {"coinbase_tier6", "$15M-$75M", 6.0, 16.0},
    {"coinbase_tier7", "$75M-$250M", 3.0, 10.0},
    {"coinbase_tier8", "$250M-$400M", 0.0, 6.0},
    {"coinbase_tier9", "$400M+", 0.0, 4.0},
};

std::optional<ScalpCoinbaseFeeTier> scalp_coinbase_fee_tier_by_key(const QString& key) {
    for (const ScalpCoinbaseFeeTier& tier : kScalpCoinbaseFeeTiers) {
        if (key == QLatin1String(tier.key))
            return tier;
    }
    return std::nullopt;
}

QString scalp_fee_venue(QString venue) {
    venue = venue.trimmed().toLower();
    QString compact = venue;
    compact.replace(QLatin1Char('-'), QLatin1Char('_'));
    compact.replace(QLatin1Char(' '), QLatin1Char('_'));
    if (compact == QLatin1String("coinbase_tier1") || compact == QLatin1String("coinbase_tier_1") ||
        compact == QLatin1String("coinbase_base") || compact == QLatin1String("coinbase_advanced"))
        return QStringLiteral("coinbase_advanced");
    if (compact == QLatin1String("coinbase_tier2") || compact == QLatin1String("coinbase_tier_2") ||
        compact == QLatin1String("coinbase_10k") || compact == QLatin1String("coinbase_10k_50k"))
        return QStringLiteral("coinbase_tier2");
    if (compact == QLatin1String("coinbase_tier3") || compact == QLatin1String("coinbase_tier_3") ||
        compact == QLatin1String("coinbase_50k") || compact == QLatin1String("coinbase_50k_100k"))
        return QStringLiteral("coinbase_tier3");
    if (compact == QLatin1String("coinbase_tier4") || compact == QLatin1String("coinbase_tier_4") ||
        compact == QLatin1String("coinbase_100k") || compact == QLatin1String("coinbase_100k_1m"))
        return QStringLiteral("coinbase_tier4");
    if (compact == QLatin1String("coinbase_tier5") || compact == QLatin1String("coinbase_tier_5") ||
        compact == QLatin1String("coinbase_1m") || compact == QLatin1String("coinbase_1m_15m"))
        return QStringLiteral("coinbase_tier5");
    if (compact == QLatin1String("coinbase_tier6") || compact == QLatin1String("coinbase_tier_6") ||
        compact == QLatin1String("coinbase_15m") || compact == QLatin1String("coinbase_15m_75m"))
        return QStringLiteral("coinbase_tier6");
    if (compact == QLatin1String("coinbase_tier7") || compact == QLatin1String("coinbase_tier_7") ||
        compact == QLatin1String("coinbase_75m") || compact == QLatin1String("coinbase_75m_250m"))
        return QStringLiteral("coinbase_tier7");
    if (compact == QLatin1String("coinbase_tier8") || compact == QLatin1String("coinbase_tier_8") ||
        compact == QLatin1String("coinbase_250m") || compact == QLatin1String("coinbase_250m_400m"))
        return QStringLiteral("coinbase_tier8");
    if (compact == QLatin1String("coinbase_tier9") || compact == QLatin1String("coinbase_tier_9") ||
        compact == QLatin1String("coinbase_400m") || compact == QLatin1String("coinbase_400m_plus"))
        return QStringLiteral("coinbase_tier9");
    if (venue.contains(QStringLiteral("coinbase")))
        return QStringLiteral("coinbase_advanced");
    if (venue.contains(QStringLiteral("alpaca")))
        return QStringLiteral("alpaca_crypto");
    if (venue.contains(QStringLiteral("kraken")))
        return QStringLiteral("kraken_pro");
    if (venue.contains(QStringLiteral("binance")))
        return QStringLiteral("binanceus");
    return venue.isEmpty() || venue == QLatin1String("unknown") ? QStringLiteral("coinbase_advanced") : venue;
}

QString scalp_liquidity_mode(QString mode) {
    mode = mode.trimmed().toLower();
    if (mode == QLatin1String("post-only") || mode == QLatin1String("postonly") ||
        mode == QLatin1String("limit-maker"))
        return QStringLiteral("maker");
    if (mode == QLatin1String("market"))
        return QStringLiteral("taker");
    if (mode == QLatin1String("maker") || mode == QLatin1String("taker"))
        return mode;
    return QStringLiteral("maker");
}

ScalpVenueFeeProfile scalp_fee_profile(const QString& venue) {
    const QString v = scalp_fee_venue(venue);
    if (const auto coinbase_tier = scalp_coinbase_fee_tier_by_key(v))
        return {v,
                coinbase_tier->maker_bps,
                coinbase_tier->taker_bps,
                QStringLiteral("Coinbase Advanced, %1 trailing 30-day volume").arg(QString::fromLatin1(coinbase_tier->volume))};
    if (v == QLatin1String("kraken_pro"))
        return {v, 25.0, 40.0, QStringLiteral("Kraken Pro spot base tier")};
    if (v == QLatin1String("binanceus"))
        return {v, 0.0, 1.0, QStringLiteral("Binance.US Tier 0 spot pairs, public fee page")};
    if (v == QLatin1String("alpaca_crypto"))
        return {v, 15.0, 25.0, QStringLiteral("Alpaca crypto tier 1")};
    return {v, 10.0, 20.0, QStringLiteral("generic local fallback")};
}

double scalp_default_fee_bps(const QString& venue, const QString& liquidity_mode) {
    const ScalpVenueFeeProfile p = scalp_fee_profile(venue);
    return scalp_liquidity_mode(liquidity_mode) == QLatin1String("maker") ? p.maker_bps : p.taker_bps;
}

QStringList scalp_default_venue_ladder() {
    return {QStringLiteral("binanceus"),
            QStringLiteral("alpaca_crypto"),
            QStringLiteral("kraken_pro"),
            QStringLiteral("coinbase_tier9"),
            QStringLiteral("coinbase_tier8"),
            QStringLiteral("coinbase_tier7"),
            QStringLiteral("coinbase_tier6"),
            QStringLiteral("coinbase_tier5"),
            QStringLiteral("coinbase_tier4"),
            QStringLiteral("coinbase_tier3"),
            QStringLiteral("coinbase_tier2"),
            QStringLiteral("coinbase_advanced")};
}

QString scalp_viability_label(double min_observed_move_bps) {
    if (!std::isfinite(min_observed_move_bps))
        return QStringLiteral("not viable");
    if (min_observed_move_bps <= 25.0)
        return QStringLiteral("micro-viable");
    if (min_observed_move_bps <= 75.0)
        return QStringLiteral("possible");
    if (min_observed_move_bps <= 200.0)
        return QStringLiteral("large-move only");
    return QStringLiteral("not a scalp");
}

struct ScalpMsWindow {
    int ms = 0;
    int ticks = 0;
    int upticks = 0;
    int downticks = 0;
    int flat_ticks = 0;
    double start_price = 0.0;
    double end_price = 0.0;
    double move_bps = 0.0;
    double pressure = 0.0;
    bool available = false;
};

QJsonObject scalp_ms_window_json(const ScalpMsWindow& w) {
    return QJsonObject{{"ms", w.ms},
                       {"ticks", w.ticks},
                       {"upticks", w.upticks},
                       {"downticks", w.downticks},
                       {"flat_ticks", w.flat_ticks},
                       {"start_price", w.start_price},
                       {"end_price", w.end_price},
                       {"move_bps", w.move_bps},
                       {"pressure", w.pressure},
                       {"available", w.available}};
}

QJsonArray double_list_json(const QVector<double>& values) {
    QJsonArray arr;
    for (const double v : values)
        arr.append(v);
    return arr;
}

double normalize_confidence_gate(double value) {
    if (!std::isfinite(value))
        return 0.0;
    if (value > 1.0)
        value /= 100.0;
    return std::clamp(value, 0.0, 1.0);
}

class DaemonScalpEngine {
  public:
    explicit DaemonScalpEngine(QString profile, CryptoFeedHub* hub)
        : profile_(std::move(profile)),
          hub_(hub),
          config_path_(daemon_scalp_config_path(profile_)),
          state_path_(daemon_scalp_state_path(profile_)),
          ticks_path_(daemon_scalp_ticks_path(profile_)),
          decisions_path_(daemon_scalp_decisions_path(profile_)) {
        // The hub owns the feeds (shared with the maker engine). We connect once
        // and filter to the symbols this engine currently tracks so config
        // reloads that change symbols_ are handled without reconnecting.
        hub_connection_ = QObject::connect(
            hub_, &CryptoFeedHub::tick_received, qApp,
            [this](const QString& symbol, const CryptoLatencyTick& tick) {
                if (!enabled_ || !symbols_.contains(symbol))
                    return;
                if (!scalp_sources_for_symbol(sources_, symbol).contains(tick.source))
                    return;
                ingest_tick(symbol, tick);
            });
        config_timer_ = new QTimer(qApp);
        config_timer_->setInterval(1000);
        QObject::connect(config_timer_, &QTimer::timeout, qApp, [this]() { reload_config(); });
        config_timer_->start();

        decision_timer_ = new QTimer(qApp);
        decision_timer_->setInterval(cadence_ms_);
        QObject::connect(decision_timer_, &QTimer::timeout, qApp, [this]() { evaluate(); });
        decision_timer_->start();
        QTimer::singleShot(0, qApp, [this]() { reload_config(); });
    }

    ~DaemonScalpEngine() {
        QObject::disconnect(hub_connection_);
        stop_services();
    }

  private:
    using CryptoLatencyService = services::crypto_latency::CryptoLatencyService;
    using CryptoLatencyTick = services::crypto_latency::CryptoLatencyTick;
    using CryptoMicrostructureRadar = services::edge_radar::CryptoMicrostructureRadar;

    ScalpMsWindow ms_window(const QVector<CryptoLatencyTick>& ticks, int ms) const {
        ScalpMsWindow out;
        out.ms = ms;
        if (ticks.size() < 2 || ms <= 0)
            return out;
        const qint64 newest = ticks.last().received_ts_ms;
        QVector<CryptoLatencyTick> rows;
        for (const auto& tick : ticks) {
            if (newest - tick.received_ts_ms <= ms)
                rows << tick;
        }
        if (rows.size() < 2)
            return out;
        out.ticks = rows.size();
        out.start_price = rows.first().price;
        out.end_price = rows.last().price;
        if (out.start_price <= 0.0 || out.end_price <= 0.0)
            return out;
        for (int i = 1; i < rows.size(); ++i) {
            if (rows[i].price > rows[i - 1].price)
                ++out.upticks;
            else if (rows[i].price < rows[i - 1].price)
                ++out.downticks;
            else
                ++out.flat_ticks;
        }
        const int directional = out.upticks + out.downticks;
        out.pressure = directional > 0
                           ? std::clamp(static_cast<double>(out.upticks - out.downticks) /
                                            static_cast<double>(directional),
                                        -1.0, 1.0)
                           : 0.0;
        out.move_bps = ((out.end_price / out.start_price) - 1.0) * 10000.0;
        out.available = true;
        return out;
    }

    QJsonObject public_config() const {
        return QJsonObject{{"enabled", enabled_},
                           {"paper", true},
                           {"symbols", QJsonArray::fromStringList(symbols_)},
                           {"sources", QJsonArray::fromStringList(sources_)},
                           {"paper_amounts_usd", double_list_json(paper_amounts_usd_)},
                           {"cadence_ms", cadence_ms_},
                           {"venue", fee_venue_},
                           {"liquidity", liquidity_mode_},
                           {"fee_bps", fee_bps_},
                           {"slippage_bps", slippage_bps_},
                           {"safety_bps", safety_bps_},
                           {"minimum_profit_bps", minimum_profit_bps_},
                           {"min_net_bps", min_net_bps_},
                           {"min_confidence", min_confidence_},
                           {"capture_ratio", capture_ratio_},
                           {"max_age_ms", max_age_ms_},
                           {"max_spread_bps", max_spread_bps_},
                           {"min_live_sources", min_live_sources_}};
    }

    QString config_fingerprint(const QJsonObject& obj) const {
        return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    }

    int int_value(const QJsonObject& obj, const QString& key, int fallback, int min_v, int max_v) const {
        return std::clamp(obj.value(key).toInt(fallback), min_v, max_v);
    }

    double double_value(const QJsonObject& obj,
                        const QString& key,
                        double fallback,
                        double min_v,
                        double max_v) const {
        const double v = obj.value(key).toDouble(fallback);
        return std::isfinite(v) ? std::clamp(v, min_v, max_v) : fallback;
    }

    QStringList string_array_value(const QJsonObject& obj,
                                   const QString& key,
                                   const QStringList& fallback) const {
        QStringList out;
        const QJsonArray arr = obj.value(key).toArray();
        for (const auto& v : arr) {
            const QString s = v.toString().trimmed();
            if (!s.isEmpty())
                out << s;
        }
        return out.isEmpty() ? fallback : out;
    }

    QVector<double> double_array_value(const QJsonObject& obj,
                                       const QString& key,
                                       const QVector<double>& fallback) const {
        QVector<double> out;
        const QJsonArray arr = obj.value(key).toArray();
        for (const auto& v : arr) {
            const double d = v.toDouble(-1.0);
            if (std::isfinite(d) && d > 0.0)
                out << d;
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out.isEmpty() ? fallback : out;
    }

    void reload_config() {
        const QJsonObject cfg = read_json_object_file(config_path_);
        const bool enabled = cfg.value(QStringLiteral("enabled")).toBool(false);
        const QString fingerprint = config_fingerprint(cfg);
        if (fingerprint == active_fingerprint_)
            return;
        active_fingerprint_ = fingerprint;

        if (!enabled) {
            enabled_ = false;
            stop_services();
            write_state(QStringLiteral("disabled"));
            return;
        }

        QStringList symbols = string_array_value(cfg, QStringLiteral("symbols"), {QStringLiteral("BTC-USD")});
        for (QString& symbol : symbols)
            symbol = CryptoLatencyService::normalize_symbol(symbol);
        symbols.removeDuplicates();

        QStringList sources = string_array_value(cfg, QStringLiteral("sources"), CryptoLatencyService::default_sources());
        for (QString& source : sources)
            source = source.trimmed().toLower();
        sources.removeDuplicates();

        cadence_ms_ = int_value(cfg, QStringLiteral("cadence_ms"), 250, 50, 5000);
        paper_amounts_usd_ = double_array_value(cfg, QStringLiteral("paper_amounts_usd"), {25.0, 50.0});
        fee_venue_ = scalp_fee_venue(cfg.value(QStringLiteral("venue")).toString(QStringLiteral("coinbase")));
        liquidity_mode_ = scalp_liquidity_mode(cfg.value(QStringLiteral("liquidity")).toString(QStringLiteral("maker")));
        fee_bps_ = double_value(cfg, QStringLiteral("fee_bps"),
                                scalp_default_fee_bps(fee_venue_, liquidity_mode_), 0.0, 1000.0);
        slippage_bps_ = double_value(cfg, QStringLiteral("slippage_bps"), 0.0, 0.0, 1000.0);
        safety_bps_ = double_value(cfg, QStringLiteral("safety_bps"), 5.0, 0.0, 1000.0);
        minimum_profit_bps_ = double_value(cfg, QStringLiteral("minimum_profit_bps"),
                                           cfg.value(QStringLiteral("min_net_bps")).toDouble(10.0),
                                           0.0, 1000.0);
        min_net_bps_ = minimum_profit_bps_;
        min_confidence_ = normalize_confidence_gate(double_value(cfg, QStringLiteral("min_confidence"), 0.0,
                                                                 0.0, 100.0));
        capture_ratio_ = double_value(cfg, QStringLiteral("capture_ratio"), 0.35, 0.01, 1.0);
        max_age_ms_ = int_value(cfg, QStringLiteral("max_age_ms"), 1000, 50, 30000);
        max_spread_bps_ = double_value(cfg, QStringLiteral("max_spread_bps"), 8.0, 0.0, 1000.0);
        min_live_sources_ = int_value(cfg, QStringLiteral("min_live_sources"), 2, 1, 10);
        symbols_ = symbols;
        sources_ = sources;
        enabled_ = true;
        started_at_ = now_utc();
        decision_timer_->setInterval(cadence_ms_);
        restart_services();
        append_job_log(profile_, QStringLiteral("scalp-engine-start symbols=%1 cadence_ms=%2")
                                     .arg(symbols_.join(','), QString::number(cadence_ms_)));
        write_state(QStringLiteral("starting"));
    }

    void restart_services() {
        stop_services();
        for (const QString& symbol : symbols_) {
            radars_.insert(symbol, CryptoMicrostructureRadar{});
            recent_.insert(symbol, {});
            const QStringList safe_sources = scalp_sources_for_symbol(sources_, symbol);
            // The hub owns the shared feed; ensure it covers this symbol with our
            // sources. Ticks arrive via the hub connection made in the ctor.
            hub_->ensure_symbol(symbol, safe_sources);
        }
    }

    void stop_services() {
        // The hub owns the feeds and outlives this engine; we only drop our own
        // per-symbol derived state here (never tear down the shared service).
        radars_.clear();
        recent_.clear();
        last_decisions_.clear();
        last_decision_hash_.clear();
        last_journal_ms_.clear();
    }

    void ingest_tick(const QString& symbol, const CryptoLatencyTick& tick) {
        auto& rows = recent_[symbol];
        rows << tick;
        std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            if (a.received_ts_ms != b.received_ts_ms)
                return a.received_ts_ms < b.received_ts_ms;
            return a.sequence < b.sequence;
        });
        const qint64 newest = rows.isEmpty() ? 0 : rows.last().received_ts_ms;
        while (!rows.isEmpty() && newest - rows.first().received_ts_ms > 300000)
            rows.removeFirst();
        while (rows.size() > 5000)
            rows.removeFirst();
        radars_[symbol].add_tick(tick);
        automation::append_jsonl_rotating(ticks_path_, CryptoLatencyService::tick_to_json(tick));
    }

    QJsonObject evaluate_symbol(const QString& symbol) {
        const auto latency_snapshot = CryptoLatencyService::filtered_snapshot(
            hub_->snapshot(symbol), scalp_sources_for_symbol(sources_, symbol));
        const auto micro = radars_[symbol].snapshot(latency_snapshot);
        const QVector<CryptoLatencyTick> rows = recent_.value(symbol);
        const ScalpMsWindow w250 = ms_window(rows, 250);
        const ScalpMsWindow w500 = ms_window(rows, 500);
        const ScalpMsWindow w1000 = ms_window(rows, 1000);
        const ScalpMsWindow w5000 = ms_window(rows, 5000);

        QStringList blockers;
        QString verdict = QStringLiteral("NO TRADE");
        QString action = QStringLiteral("NO_ORDER");
        QString direction = QStringLiteral("flat");
        const double pressure = (w250.pressure * 0.25) + (w500.pressure * 0.30) +
                                (w1000.pressure * 0.30) + (micro.book_pressure * 0.15);
        const double confidence = std::clamp(std::abs(pressure), 0.0, 1.0);
        if (pressure > 0.15)
            direction = QStringLiteral("up");
        else if (pressure < -0.15)
            direction = QStringLiteral("down");

        const double observed_bps = w1000.available ? w1000.move_bps : (w500.available ? w500.move_bps : 0.0);
        const double directional_bps = direction == QLatin1String("up") ? std::max(0.0, observed_bps) : 0.0;
        const double expected_capture_bps = directional_bps * capture_ratio_ * confidence;
        const double spread_cost_bps = liquidity_mode_ == QLatin1String("taker")
                                           ? std::max(0.0, latency_snapshot.cross_source_spread_bps)
                                           : 0.0;
        const double entry_fee_bps = fee_bps_;
        const double exit_fee_bps = fee_bps_;
        const double round_trip_bps = entry_fee_bps + exit_fee_bps + (slippage_bps_ * 2.0) +
                                      spread_cost_bps + safety_bps_;
        const double net_bps = expected_capture_bps - round_trip_bps;
        const double required_edge_bps = round_trip_bps + minimum_profit_bps_;
        const double edge_surplus_bps = expected_capture_bps - required_edge_bps;

        if (latency_snapshot.freshest_age_ms < 0 || latency_snapshot.freshest_age_ms > max_age_ms_)
            blockers << QStringLiteral("freshest tick too stale");
        if (latency_snapshot.live_sources < min_live_sources_)
            blockers << QStringLiteral("not enough live sources");
        if (latency_snapshot.cross_source_spread_bps > max_spread_bps_)
            blockers << QStringLiteral("spread/divergence too wide");
        if (!w500.available && !w1000.available)
            blockers << QStringLiteral("not enough millisecond window history");
        if (direction != QLatin1String("up"))
            blockers << QStringLiteral("paper spot scalp only supports long/up candidates");
        if (confidence < min_confidence_)
            blockers << QStringLiteral("confidence below risk threshold");
        if (edge_surplus_bps < 0.0)
            blockers << QStringLiteral("estimated captured move does not clear required edge");

        if (blockers.isEmpty()) {
            verdict = QStringLiteral("PAPER TRADE CANDIDATE");
            action = QStringLiteral("PAPER_LIMIT_BUY_ONLY");
        } else if (direction == QLatin1String("up") && latency_snapshot.freshest_age_ms >= 0 &&
                   latency_snapshot.freshest_age_ms <= max_age_ms_) {
            verdict = QStringLiteral("WATCH");
        }

        QJsonArray windows;
        windows << scalp_ms_window_json(w250) << scalp_ms_window_json(w500)
                << scalp_ms_window_json(w1000) << scalp_ms_window_json(w5000);
        QJsonArray paper_amounts;
        for (const double amount : paper_amounts_usd_) {
            paper_amounts.append(QJsonObject{{"notional_usd", amount},
                                             {"expected_capture_usd", amount * expected_capture_bps / 10000.0},
                                             {"round_trip_cost_usd", amount * round_trip_bps / 10000.0},
                                             {"minimum_profit_usd", amount * minimum_profit_bps_ / 10000.0},
                                             {"required_edge_usd", amount * required_edge_bps / 10000.0},
                                             {"net_after_cost_usd", amount * net_bps / 10000.0},
                                             {"edge_surplus_usd", amount * edge_surplus_bps / 10000.0}});
        }
        QJsonObject decision{{"ts", now_utc()},
                             {"ts_ms", QString::number(QDateTime::currentMSecsSinceEpoch())},
                             {"profile", profile_},
                             {"engine", "daemon-scalp-ms"},
                             {"paper", true},
                             {"symbol", symbol},
                             {"verdict", verdict},
                             {"action", action},
                             {"direction", direction},
                             {"reference_price", latency_snapshot.mid_price},
                             {"freshest_source", latency_snapshot.freshest_source},
                             {"freshest_age_ms", latency_snapshot.freshest_age_ms},
                             {"live_sources", latency_snapshot.live_sources},
                             {"spread_bps", latency_snapshot.cross_source_spread_bps},
                             {"cross_source_spread_bps", latency_snapshot.cross_source_spread_bps},
                             {"spread_cost_bps", spread_cost_bps},
                             {"pressure", pressure},
                             {"confidence", confidence},
                             {"min_confidence", min_confidence_},
                             {"observed_move_bps", observed_bps},
                             {"expected_capture_bps", expected_capture_bps},
                             {"round_trip_cost_bps", round_trip_bps},
                             {"minimum_profit_bps", minimum_profit_bps_},
                             {"required_edge_bps", required_edge_bps},
                             {"net_after_cost_bps", net_bps},
                             {"edge_surplus_bps", edge_surplus_bps},
                             {"venue", fee_venue_},
                             {"liquidity", liquidity_mode_},
                             {"entry_fee_bps", entry_fee_bps},
                             {"exit_fee_bps", exit_fee_bps},
                             {"fee_bps", fee_bps_},
                             {"slippage_bps", slippage_bps_},
                             {"safety_bps", safety_bps_},
                             {"paper_amounts", paper_amounts},
                             {"cadence_ms", cadence_ms_},
                             {"blockers", QJsonArray::fromStringList(blockers)},
                             {"windows_ms", windows},
                             {"microstructure", services::edge_radar::CryptoMicrostructureRadar::to_json(micro)}};

        const QString hash = QStringLiteral("%1|%2|%3|%4").arg(symbol, verdict, direction, blockers.join(';'));
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const bool should_journal = last_decision_hash_.value(symbol) != hash ||
                                    now - last_journal_ms_.value(symbol, 0) >= 1000;
        if (should_journal) {
            automation::append_jsonl_rotating(decisions_path_, decision);
            last_decision_hash_[symbol] = hash;
            last_journal_ms_[symbol] = now;
        }
        return decision;
    }

    void evaluate() {
        if (!enabled_)
            return;
        QJsonArray decisions;
        for (const QString& symbol : symbols_) {
            const QJsonObject decision = evaluate_symbol(symbol);
            last_decisions_[symbol] = decision;
            decisions.append(decision);
        }
        write_state(QStringLiteral("running"), decisions);
    }

    void write_state(const QString& status, QJsonArray decisions = {}) const {
        if (decisions.isEmpty()) {
            for (auto it = last_decisions_.begin(); it != last_decisions_.end(); ++it)
                decisions.append(it.value());
        }
        QJsonObject source_map;
        for (const QString& symbol : symbols_)
            source_map[symbol] = QJsonArray::fromStringList(scalp_sources_for_symbol(sources_, symbol));
        const QJsonObject state{{"profile", profile_},
                                {"status", status},
                                {"enabled", enabled_},
                                {"pid", static_cast<double>(QCoreApplication::applicationPid())},
                                {"started_at", started_at_},
                                {"heartbeat_at", now_utc()},
                                {"config", public_config()},
                                {"effective_sources", source_map},
                                {"decisions", decisions},
                                {"ticks_path", ticks_path_},
                                {"decisions_path", decisions_path_}};
        QString error;
        if (!write_json_object_file(state_path_, state, &error))
            append_job_log(profile_, QStringLiteral("scalp-state-write-failed error=\"%1\"").arg(error));
    }

    QString profile_;
    CryptoFeedHub* hub_ = nullptr;
    QMetaObject::Connection hub_connection_;
    QString config_path_;
    QString state_path_;
    QString ticks_path_;
    QString decisions_path_;
    QTimer* config_timer_ = nullptr;
    QTimer* decision_timer_ = nullptr;
    QString active_fingerprint_;
    bool enabled_ = false;
    QString started_at_;
    QStringList symbols_{QStringLiteral("BTC-USD")};
    QStringList sources_ = CryptoLatencyService::default_sources();
    QVector<double> paper_amounts_usd_{25.0, 50.0};
    int cadence_ms_ = 250;
    QString fee_venue_{QStringLiteral("coinbase")};
    QString liquidity_mode_{QStringLiteral("maker")};
    double fee_bps_ = 40.0;
    double slippage_bps_ = 0.0;
    double safety_bps_ = 5.0;
    double minimum_profit_bps_ = 10.0;
    double min_net_bps_ = 5.0;
    double min_confidence_ = 0.0;
    double capture_ratio_ = 0.35;
    int max_age_ms_ = 1000;
    double max_spread_bps_ = 8.0;
    int min_live_sources_ = 2;
    QHash<QString, CryptoMicrostructureRadar> radars_;
    QHash<QString, QVector<CryptoLatencyTick>> recent_;
    QHash<QString, QJsonObject> last_decisions_;
    QHash<QString, QString> last_decision_hash_;
    QHash<QString, qint64> last_journal_ms_;
};

// Paper maker-spread producer. On a timer it reads each venue's own BBO/last
// trade for every crypto symbol and writes two-sided
// resting-quote decision rows (bid+ask) to maker_decisions.jsonl, the journal
// the sandbox executor consumer (open_maker_quotes) already reads. It is
// read-only: no order path, no credentials — it only appends decision rows.
//
// The same feed also writes venue-tagged ticks to maker_ticks.jsonl. That keeps
// Coinbase and Kraken fill evidence separate and lets maker lanes run without
// depending on the separately-configured scalp engine.
//
// NOTE (deliberate): unlike DaemonScalpEngine, this engine has no config file
// and no `enabled` gate — it is always-on while `serve` runs. It no longer opens
// its own crypto feeds: it registers its symbols with the shared CryptoFeedHub
// and consumes the hub's ticks/snapshots, so it shares one WebSocket per venue
// with the scalp engine instead of duplicating connections. Read-only glue.
class DaemonMakerEngine {
  public:
    explicit DaemonMakerEngine(QString profile, CryptoFeedHub* hub)
        : profile_(std::move(profile)),
          hub_(hub),
          decisions_path_(daemon_maker_decisions_path(profile_)),
          ticks_path_(daemon_maker_ticks_path(profile_)) {
        // The hub owns the shared feeds. Register each maker symbol with the
        // maker's own venue sources and consume ticks/snapshots from the hub.
        for (const QString& symbol : symbols_)
            hub_->ensure_symbol(symbol, {QStringLiteral("coinbase"), QStringLiteral("kraken")});
        hub_connection_ = QObject::connect(
            hub_, &CryptoFeedHub::tick_received, qApp,
            [this](const QString& symbol, const CryptoLatencyTick& tick) {
                if (symbols_.contains(symbol))
                    ingest_tick(tick);
            });
        decision_timer_ = new QTimer(qApp);
        decision_timer_->setInterval(cadence_ms_);
        QObject::connect(decision_timer_, &QTimer::timeout, qApp, [this]() { evaluate(); });
        decision_timer_->start();
    }

    ~DaemonMakerEngine() { QObject::disconnect(hub_connection_); }

  private:
    using CryptoLatencyService = services::crypto_latency::CryptoLatencyService;
    using CryptoLatencyTick = services::crypto_latency::CryptoLatencyTick;

    static QString venue_for_source(const QString& source) {
        if (source == QLatin1String("coinbase"))
            return QStringLiteral("coinbase_advanced");
        if (source == QLatin1String("kraken"))
            return QStringLiteral("kraken_pro");
        return {};
    }

    void ingest_tick(const CryptoLatencyTick& tick) {
        const QString venue = venue_for_source(tick.source);
        if (venue.isEmpty())
            return;
        QJsonObject row = CryptoLatencyService::tick_to_json(tick);
        row.insert(QStringLiteral("venue"), venue);
        automation::append_jsonl_rotating(ticks_path_, row);
    }

    void evaluate() {
        // Match DaemonScalpEngine's decision clock (ts_ms).
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        for (const QString& symbol : symbols_) {
            const auto snap = hub_->snapshot(symbol);
            int fresh_sources = 0;
            for (const auto& tick : snap.latest_ticks) {
                // Count only the maker's own venue sources (coinbase/kraken). The
                // hub's service may carry extra sources (e.g. scalp's binanceus /
                // bitcointicker) on symbols both engines share, but legacy maker
                // never saw those, so exclude them to keep decision rows identical.
                if (venue_for_source(tick.source).isEmpty())
                    continue;
                if (tick.received_ts_ms > 0 && now_ms - tick.received_ts_ms <= max_tick_age_ms_)
                    ++fresh_sources;
            }
            for (const auto& tick : snap.latest_ticks) {
                const QString venue = venue_for_source(tick.source);
                const qint64 age_ms = tick.received_ts_ms > 0 ? now_ms - tick.received_ts_ms : -1;
                if (venue.isEmpty() || age_ms < 0 || age_ms > max_tick_age_ms_)
                    continue;
                const double venue_mid = tick.best_bid > 0.0 && tick.best_ask >= tick.best_bid
                    ? (tick.best_bid + tick.best_ask) / 2.0
                    : tick.price;
                for (const QJsonObject& row : services::sandbox::maker_decision_rows(
                         symbol, venue, venue_mid, half_spread_for(venue),
                         static_cast<double>(age_ms), fresh_sources, now_ms)) {
                    automation::append_jsonl_rotating(decisions_path_, row);
                }
            }
        }
    }

    static double half_spread_for(const QString& venue) {
        // MUST stay in lockstep with spot_lane_grid's crypto venue table in
        // SandboxRegistry.cpp: coinbase_advanced and kraken_pro both use
        // half_spread_bps = 2.0. If a venue's half_spread_bps changes there,
        // change it here.
        Q_UNUSED(venue);
        return 2.0;
    }

    QString profile_;
    CryptoFeedHub* hub_ = nullptr;
    QMetaObject::Connection hub_connection_;
    QString decisions_path_;
    QString ticks_path_;
    QTimer* decision_timer_ = nullptr;
    // The sandbox consumes only the latest quote on its much slower job
    // cadence. One-second snapshots preserve market movement without writing
    // four redundant decision batches per second to the rotating journal.
    int cadence_ms_ = 1000;
    int max_tick_age_ms_ = 5000;
    // Mirrors spot_lane_grid's crypto venues' symbol set (BTC-USD,ETH-USD,SOL-USD
    // in SandboxRegistry.cpp:140-143) so every seeded maker lane has a producer:
    // open_maker_quotes iterates all of a strategy's symbols, so omitting SOL-USD
    // would leave its seeded maker lanes dormant. CryptoLatencyService supports all
    // three (normalize_symbol/kraken_pair/make_feed are generic, not two-symbol).
    const QStringList symbols_{QStringLiteral("BTC-USD"), QStringLiteral("ETH-USD"),
                               QStringLiteral("SOL-USD")};
};

QJsonObject daemon_health_object(const QString& profile) {
    QJsonObject o = daemon_status_object(profile);
    auto info = read_bridge_file(profile_root_for(profile));
    if (info && is_pid_alive(info->pid)) {
        const QDateTime started = parse_utc(info->started_at);
        if (started.isValid())
            o["uptime_sec"] = started.secsTo(QDateTime::currentDateTimeUtc());
    }
    o["version"] = QCoreApplication::applicationVersion();
    o["jobs"] = jobs_summary(profile);
    o["logs"] = QJsonObject{{"stdout", daemon_logs_dir(profile) + QStringLiteral("/daemon.out.log")},
                            {"stderr", daemon_logs_dir(profile) + QStringLiteral("/daemon.err.log")},
                            {"jobs", daemon_job_log_path(profile)}};
    o["capabilities"] = QJsonArray{"health", "readiness", "logs", "jobs", "monitors", "collectors", "notify",
                                   "paper-strategy", "ai-automation", "audit"};
    return o;
}

int emit_daemon_health(const QString& profile, bool json) {
    const QJsonObject o = daemon_health_object(profile);
    if (json) {
        std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("profile      %s\n", qUtf8Printable(profile));
    std::printf("mode         %s\n", qUtf8Printable(o.value("mode").toString()));
    std::printf("daemon       %s\n", o.value("daemon_running").toBool() ? "running" : "not running");
    std::printf("installed    %s\n", o.value("installed").toBool() ? "yes" : "no");
    if (o.value("profile_owner_reachable").toBool()) {
        std::printf("owner        %s pid=%lld endpoint=%s\n",
                    qUtf8Printable(o.value("active_owner_kind").toString()),
                    static_cast<long long>(o.value("active_owner_pid").toDouble()),
                    qUtf8Printable(o.value("active_endpoint").toString()));
    }
    if (o.value("scheduler_running").toBool()) {
        const qint64 scheduler_pid = o.contains("daemon_process_pid")
                                         ? static_cast<qint64>(o.value("daemon_process_pid").toDouble())
                                         : static_cast<qint64>(o.value("pid").toDouble());
        std::printf("scheduler    %s pid=%lld\n",
                    qUtf8Printable(o.value("daemon_process_mode").toString(
                        o.value("daemon_bridge_owner").toBool() ? QStringLiteral("owner") : QStringLiteral("warm"))),
                    static_cast<long long>(scheduler_pid));
    }
    if (o.contains("uptime_sec"))
        std::printf("uptime       %llds\n", static_cast<long long>(o.value("uptime_sec").toDouble()));
    const QJsonObject j = o.value("jobs").toObject();
    std::printf("jobs         total=%d enabled=%d running=%d failed=%d stale=%d history=%d historical_only=%d disabled_failed=%d interval=%d\n",
                j.value("total").toInt(), j.value("enabled").toInt(), j.value("running").toInt(),
                j.value("failed").toInt(), j.value("stale").toInt(),
                j.value("failed_history").toInt(), j.value("historical_only").toInt(),
                j.value("disabled_current_failures").toInt(), j.value("interval").toInt());
    std::printf("endpoint     %s\n", qUtf8Printable(o.value("endpoint").toString("(none)")));
    if (o.contains("daemon_blocked_by"))
        std::printf("note         bridge %s; daemon scheduler %s\n",
                    qUtf8Printable(o.value("daemon_blocked_by").toString()),
                    o.value("scheduler_running").toBool() ? "is warm and can run jobs" : "is not running");
    return 0;
}

int emit_daemon_logs(const QString& profile, bool json, QStringList args) {
    QString channel = QStringLiteral("stderr");
    int lines = 80;
    for (int i = 0; i < args.size(); ++i) {
        const QString token = args.at(i);
        if (token == "--lines" && i + 1 < args.size()) {
            bool ok = false;
            lines = args.at(++i).toInt(&ok);
            if (!ok || lines <= 0) {
                std::fprintf(stderr, "--lines requires a positive integer\n");
                return 2;
            }
        } else if (token == "stderr" || token == "err" || token == "stdout" || token == "out" ||
                   token == "jobs" || token == "job") {
            channel = (token == "out") ? QStringLiteral("stdout")
                      : (token == "err") ? QStringLiteral("stderr")
                      : (token == "job") ? QStringLiteral("jobs")
                                         : token;
        } else {
            std::fprintf(stderr, "usage: daemon logs [stderr|stdout|jobs] [--lines N]\n");
            return 2;
        }
    }
    const QString path = channel == "stdout" ? daemon_logs_dir(profile) + QStringLiteral("/daemon.out.log")
                       : channel == "jobs" ? daemon_job_log_path(profile)
                                           : daemon_logs_dir(profile) + QStringLiteral("/daemon.err.log");
    const QString text = tail_text(path, lines);
    if (json) {
        std::printf("%s\n", QJsonDocument(QJsonObject{{"profile", profile},
                                                      {"channel", channel},
                                                      {"path", path},
                                                      {"lines", lines},
                                                      {"text", text}})
                                .toJson(QJsonDocument::Compact)
                                .constData());
    } else {
        if (text.isEmpty())
            std::printf("no log data: %s\n", qUtf8Printable(path));
        else
            std::printf("%s\n", qUtf8Printable(text));
    }
    return 0;
}

int emit_daemon_audit(const QString& profile, bool json) {
    QJsonObject o{{"profile", profile},
                  {"plist", daemon_plist_path(profile)},
                  {"jobs_file", daemon_jobs_path(profile)},
                  {"history_db", daemon_history_db_path(profile)},
                  {"logs_dir", daemon_logs_dir(profile)},
                  {"api_keys_leave_machine", false},
                  {"unattended_live_trading", false},
                  {"daemon_bridge_destructive_tools", false},
                  {"daemon_settings_write_tools", false}};
    o["allowed_job_kinds"] = QJsonArray{"command", "brief", "risk", "thesis", "radar", "ai", "notebook",
                                        "paper-strategy", "notify", "health-check"};
    o["guardrails"] = QJsonArray{"LaunchAgent is per-user, not root",
                                 "jobs run as the selected local profile",
                                 "live deployment remains GUI-gated",
                                 "secrets are not written to daemon job specs"};
    o["status"] = daemon_status_object(profile);
    if (json) {
        std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
    } else {
        std::printf("daemon audit  profile=%s\n", qUtf8Printable(profile));
        std::printf("live trading unattended: no\n");
        std::printf("destructive bridge tools: no\n");
        std::printf("settings writes from daemon bridge: no\n");
        std::printf("job store: %s\n", qUtf8Printable(daemon_jobs_path(profile)));
        std::printf("run history: %s\n", qUtf8Printable(daemon_history_db_path(profile)));
        std::printf("plist: %s\n", qUtf8Printable(daemon_plist_path(profile)));
    }
    return 0;
}

int emit_daemon_status(const QString& profile, bool json) {
    const QJsonObject o = daemon_status_object(profile);
    if (json) {
        std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
        return o.value("installed").toBool() || o.value("running").toBool() ? 0 : 3;
    }
    std::printf("profile    %s\n", qUtf8Printable(profile));
    std::printf("label      %s\n", qUtf8Printable(o.value("label").toString()));
    std::printf("plist      %s\n", qUtf8Printable(o.value("plist").toString()));
    std::printf("installed  %s\n", o.value("installed").toBool() ? "yes" : "no");
    std::printf("loaded     %s\n", o.value("loaded").toBool() ? "yes" : "no");
    std::printf("daemon     %s\n", o.value("scheduler_running").toBool() ? "running" : "not running");
    if (o.value("scheduler_running").toBool()) {
        const qint64 scheduler_pid = o.contains("daemon_process_pid")
                                         ? static_cast<qint64>(o.value("daemon_process_pid").toDouble())
                                         : static_cast<qint64>(o.value("pid").toDouble());
        std::printf("scheduler  %s pid=%lld\n",
                    qUtf8Printable(o.value("daemon_process_mode").toString(
                        o.value("daemon_bridge_owner").toBool() ? QStringLiteral("owner") : QStringLiteral("warm"))),
                    static_cast<long long>(scheduler_pid));
    }
    std::printf("mode       %s\n", qUtf8Printable(o.value("mode").toString()));
    if (o.contains("owner_kind")) {
        std::printf("owner      %s pid=%lld endpoint=%s\n",
                    qUtf8Printable(o.value("owner_kind").toString()),
                    static_cast<long long>(o.value("pid").toDouble()),
                    qUtf8Printable(o.value("endpoint").toString()));
    }
    if (o.contains("daemon_blocked_by"))
        std::printf("note       bridge %s; daemon scheduler can still run warm in the background\n",
                    qUtf8Printable(o.value("daemon_blocked_by").toString()));
    return o.value("installed").toBool() || o.value("running").toBool() ? 0 : 3;
}

int daemon_start_impl(const QString& profile, bool json) {
#if !defined(Q_OS_MACOS)
    Q_UNUSED(profile); Q_UNUSED(json);
    std::fprintf(stderr, "daemon install/start is currently supported on macOS launchd only\n");
    return 2;
#else
    auto emit_started = [&](bool already_running, const QString& warning = {}) {
        if (json) {
            QJsonObject o = daemon_status_object(profile);
            o["started"] = !already_running;
            o["already_running"] = already_running;
            if (!warning.isEmpty())
                o["warning"] = warning;
            std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
        } else {
            if (!warning.isEmpty())
                std::fprintf(stderr, "warning: %s\n", qUtf8Printable(warning));
            std::printf("%s for profile '%s'\n",
                        already_running ? "daemon already running" : "daemon started",
                        qUtf8Printable(profile));
        }
        return 0;
    };
    const QString plist = daemon_plist_path(profile);
    if (!QFileInfo::exists(plist)) {
        std::fprintf(stderr, "daemon is not installed for profile '%s'; run daemon install first\n",
                     qUtf8Printable(profile));
        return 3;
    }
    const QJsonObject current = daemon_status_object(profile);
    if (current.value("scheduler_running").toBool())
        return emit_started(true);
    if (!launchd_loaded(profile)) {
        const ProcessResult boot = run_process(QStringLiteral("launchctl"),
                                               {QStringLiteral("bootstrap"), launch_domain(), plist},
                                               10000);
        if (boot.exit_code != 0 && !boot.err.contains(QStringLiteral("already"), Qt::CaseInsensitive)) {
            std::fprintf(stderr, "launchctl bootstrap failed: %s\n", qUtf8Printable(boot.err.trimmed()));
            return 7;
        }
        if (wait_for_daemon_running(profile, 5000))
            return emit_started(false);
    }
    if (daemon_status_object(profile).value("running").toBool())
        return emit_started(true);
    const ProcessResult kick = run_process(QStringLiteral("launchctl"),
                                           {QStringLiteral("kickstart"), QStringLiteral("-k"),
                                            launch_domain() + "/" + daemon_label(profile)},
                                           10000);
    if (kick.exit_code != 0) {
        if (wait_for_daemon_running(profile, 15000)) {
            const QString detail = kick.err.trimmed().isEmpty()
                                       ? QStringLiteral("no launchctl error text")
                                       : kick.err.trimmed();
            const QString warning = QStringLiteral("launchctl kickstart reported an error after the daemon became reachable: %1")
                                        .arg(detail);
            return emit_started(false, warning);
        }
        std::fprintf(stderr, "launchctl kickstart failed: %s\n", qUtf8Printable(kick.err.trimmed()));
        return 7;
    }
    if (!wait_for_daemon_running(profile, 15000)) {
        std::fprintf(stderr, "daemon start returned but the local bridge did not become reachable\n");
        return 7;
    }
    return emit_started(false);
#endif
}

int daemon_stop_impl(const QString& profile, bool json, bool quiet = false) {
#if !defined(Q_OS_MACOS)
    Q_UNUSED(json);
    Q_UNUSED(quiet);
    return serve_stop(profile);
#else
    bool stopped = false;
    if (launchd_loaded(profile)) {
        const ProcessResult r = run_process(QStringLiteral("launchctl"),
                                            {QStringLiteral("bootout"), launch_domain() + "/" + daemon_label(profile)},
                                            10000);
        if (r.exit_code != 0 && !r.err.contains(QStringLiteral("No such process"), Qt::CaseInsensitive)) {
            std::fprintf(stderr, "launchctl bootout failed: %s\n", qUtf8Printable(r.err.trimmed()));
            return 7;
        }
        stopped = true;
    } else {
        auto info = read_bridge_file(profile_root_for(profile));
        if (info && is_pid_alive(info->pid) && info->kind == QStringLiteral("daemon")) {
            const int rc = serve_stop(profile);
            if (rc != 0) return rc;
            stopped = true;
        }
    }
    if (quiet) {
        return 0;
    }
    if (json) {
        QJsonObject o = daemon_status_object(profile);
        o["stopped"] = stopped;
        std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
    } else if (stopped) {
        std::printf("daemon stopped for profile '%s'\n", qUtf8Printable(profile));
    } else {
        std::fprintf(stderr, "daemon is not loaded/running for profile '%s'\n", qUtf8Printable(profile));
        return 3;
    }
    return 0;
#endif
}

} // namespace

void start_daemon_job_scheduler(const QString& profile);

int serve_run(const QString& profile) {
    const QString root = profile_root_for(profile);
    std::optional<BridgeInfo> startup_owner = read_bridge_file(root);
    const bool startup_owner_live = startup_owner && is_pid_alive(startup_owner->pid);
    // The bridge is still single-owner. A daemon may now coexist with a GUI as
    // a warm scheduler, but two daemon owners would duplicate scheduled jobs.
    if (startup_owner_live && startup_owner->kind == QStringLiteral("daemon")) {
        std::fprintf(stderr, "An instance already owns profile '%s' (%s, pid %lld) at %s\n",
                     qUtf8Printable(profile), qUtf8Printable(startup_owner->kind),
                     static_cast<long long>(startup_owner->pid), qUtf8Printable(startup_owner->endpoint));
        return 3;
    }
    const QJsonObject existing_runtime = read_daemon_runtime(profile);
    if (daemon_runtime_live(existing_runtime) &&
        static_cast<qint64>(existing_runtime.value(QStringLiteral("pid")).toDouble()) !=
            QCoreApplication::applicationPid()) {
        std::fprintf(stderr, "A daemon scheduler already runs for profile '%s' (pid %lld)\n",
                     qUtf8Printable(profile),
                     static_cast<long long>(existing_runtime.value(QStringLiteral("pid")).toDouble()));
        return 3;
    }

    headless::HeadlessRuntime rt;
    if (auto r = rt.init(profile); !r.ok) {
        std::fprintf(stderr, "daemon init failed: %s\n", qUtf8Printable(r.error));
        return 7;
    }

    // Read-only over the bridge: deny destructive AND settings-write regardless
    // of toggles (writes/destructive over a long-lived daemon need the revocable
    // -token design — deferred). Reads + the >=Verified floor unchanged.
    // NOTE: this gate covers only McpProvider (internal) tool calls. If a future
    // task initializes McpService (external MCP servers) in the daemon, those
    // calls would bypass this checker and need their own read-only gate.
    mcp::McpProvider::instance().set_auth_checker(
        [](const QString& tool, const QJsonObject& args, mcp::AuthLevel required, bool is_destructive) {
            if (required >= mcp::AuthLevel::Verified) return false;
            if (tool == "submit_order") {
                // Normalize identically to the handler so a case/whitespace variant
                // can't take a different branch than the handler will.
                const QString mode = args.value("mode").toString().trimmed().toLower();
                if (mode == "paper") return true;                // reach the handler; it enforces the toggle + executes
                return mcp::cli_trading_allowed() && mcp::cli_live_armed();  // live: reach the handler only when armed (handler enforces the full stack)
            }
            // Fast-live carve-out (Phase D) — IDENTICAL predicate in all three
            // hosts. The fast-live tool set is reachable ONLY when fully armed:
            // base trading + base live arm + the SECOND fast arm. Raw live_* are
            // NOT in this set, so they fall through to the destructive denial
            // below and stay denied. (When the fast tools are built they must NOT
            // be classified live-execution, or the AI-facing hosts that deny those
            // would block them before this gate fires.)
            if (mcp::is_fast_live_tool(tool))
                return mcp::cli_trading_allowed() && mcp::cli_live_armed() && mcp::cli_fast_live_armed();
            if (is_destructive) return false;            // daemon MVP: no writes/destructive
            if (mcp::is_settings_write_tool(tool)) return false;
            return true;
        });

#ifndef _WIN32
    // Clean shutdown on SIGTERM/SIGINT via the self-pipe trick (async-signal-safe).
    // Install the handlers BEFORE bridge.start() so a signal arriving during/after
    // start() routes through the notifier (clean stop) rather than killing the
    // process with a stale bridge.json on disk. The pipe + std::signal install do
    // not depend on the bridge; the QSocketNotifier needs qApp, which exists.
    // O_CLOEXEC keeps the self-pipe fds out of any child processes the daemon spawns.
    bool sig_ready = false;
#  if defined(__linux__)
    sig_ready = (::pipe2(g_sigfd, O_CLOEXEC) == 0);       // Linux: atomic CLOEXEC
#  endif
    if (!sig_ready && ::pipe(g_sigfd) == 0) {             // macOS fallback: pipe + fcntl
        ::fcntl(g_sigfd[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(g_sigfd[1], F_SETFD, FD_CLOEXEC);
        sig_ready = true;
    }
    if (sig_ready) {
        auto* sn = new QSocketNotifier(g_sigfd[0], QSocketNotifier::Read, qApp);
        QObject::connect(sn, &QSocketNotifier::activated, qApp, []() { QCoreApplication::quit(); });
        std::signal(SIGTERM, on_signal);
        std::signal(SIGINT, on_signal);
    }
#endif

    auto& bridge = mcp::TerminalMcpBridge::instance();
    bool bridge_started = false;
    // Declared here (before write_runtime_state captures it by reference) but
    // constructed later, once the crypto engines are set up; write_runtime_state
    // tolerates it being null until then.
    CryptoFeedHub* crypto_feed_hub = nullptr;
    auto write_runtime_state = [&]() {
        const QJsonArray feeds = crypto_feed_hub ? crypto_feed_hub->feed_health() : QJsonArray{};
        if (bridge_started) {
            write_daemon_runtime(profile, QStringLiteral("owner"), bridge.endpoint(), QString(), QString(),
                                 feeds);
            return;
        }
        auto owner = read_bridge_file(root);
        const bool owner_live = owner && is_pid_alive(owner->pid);
        write_daemon_runtime(profile,
                             owner_live ? QStringLiteral("warm") : QStringLiteral("warm-no-owner"),
                             {},
                             owner_live ? owner->kind : QString(),
                             owner_live ? owner->endpoint : QString(),
                             feeds);
    };

    auto try_promote_to_owner = [&]() {
        if (bridge_started)
            return;
        auto owner = read_bridge_file(root);
        if (owner && is_pid_alive(owner->pid))
            return;
        bridge.set_owner_kind("daemon");
        if (!bridge.start()) {
            append_job_log(profile, QStringLiteral("daemon-promote-failed"));
            return;
        }
        bridge_started = true;
        append_job_log(profile, QStringLiteral("daemon-promoted-to-owner endpoint=%1").arg(bridge.endpoint()));
        write_runtime_state();
    };

    if (!startup_owner_live) {
        bridge.set_owner_kind("daemon");
        if (!bridge.start()) {                                 // binds 127.0.0.1 + writes bridge.json(kind=daemon)
            std::fprintf(stderr, "daemon: failed to start the bridge\n");
            return 7;
        }
        bridge_started = true;
        std::fprintf(stderr, "openterminalcli serve: %s (profile '%s', pid %lld). Ctrl-C / SIGTERM to stop.\n",
                     qUtf8Printable(bridge.endpoint()), qUtf8Printable(profile),
                     static_cast<long long>(QCoreApplication::applicationPid()));
    } else {
        std::fprintf(stderr,
                     "openterminalcli serve: warm scheduler for profile '%s' beside %s pid %lld at %s "
                     "(daemon pid %lld). It will promote when the bridge owner exits.\n",
                     qUtf8Printable(profile),
                     qUtf8Printable(startup_owner->kind),
                     static_cast<long long>(startup_owner->pid),
                     qUtf8Printable(startup_owner->endpoint),
                     static_cast<long long>(QCoreApplication::applicationPid()));
    }
    write_runtime_state();
    auto* heartbeat = new QTimer(qApp);
    heartbeat->setInterval(2000);
    QObject::connect(heartbeat, &QTimer::timeout, qApp, [&, profile]() {
        try_promote_to_owner();
        write_runtime_state();
    });
    heartbeat->start();
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, [profile]() { remove_daemon_runtime(profile); });
    start_daemon_job_scheduler(profile);
    // One shared crypto feed hub for both daemon engines: it owns a single
    // CryptoLatencyService per union symbol so scalp and maker share one
    // WebSocket per venue instead of each opening their own (which tripped
    // Kraken's HTTP 429 rate limit). Constructed before the engines; its
    // teardown is registered last so the hub outlives both engines.
    crypto_feed_hub = new CryptoFeedHub(qApp);
    auto* scalp_engine = new DaemonScalpEngine(profile, crypto_feed_hub);
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, [scalp_engine]() { delete scalp_engine; });
    auto* maker_engine = new DaemonMakerEngine(profile, crypto_feed_hub);
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, [maker_engine]() { delete maker_engine; });
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, [crypto_feed_hub]() { delete crypto_feed_hub; });
    auto* kalshi_event_engine = new KalshiLiveEventEngine(profile, crypto_feed_hub, qApp);
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp,
                     [kalshi_event_engine]() { delete kalshi_event_engine; });

    const int rc = QCoreApplication::exec();              // feeds/subscriptions live here
    if (bridge_started)
        bridge.stop();                                    // removes bridge.json only if this daemon owned it
    remove_daemon_runtime(profile);
    return rc;
}

int serve_status(const QString& profile, bool json) {
    auto info = read_bridge_file(profile_root_for(profile));
    const bool live = info && is_pid_alive(info->pid);
    if (json) {
        QJsonObject o{{"running", live}};
        if (live) { o["endpoint"]=info->endpoint; o["pid"]=info->pid; o["kind"]=info->kind; }
        std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
    } else if (live) {
        std::printf("running  kind=%s  endpoint=%s  pid=%lld\n",
                    qUtf8Printable(info->kind), qUtf8Printable(info->endpoint),
                    static_cast<long long>(info->pid));
    } else {
        std::fprintf(stderr, "no running instance for profile '%s'\n", qUtf8Printable(profile));
    }
    return live ? 0 : 3;
}

int serve_stop(const QString& profile) {
    auto info = read_bridge_file(profile_root_for(profile));
    if (!info || !is_pid_alive(info->pid)) {
        std::fprintf(stderr, "no running instance for profile '%s'\n", qUtf8Printable(profile));
        return 3;
    }
    if (info->kind != "daemon") {
        std::fprintf(stderr, "owner is a %s, not a daemon — refusing to stop it (quit it directly)\n",
                     qUtf8Printable(info->kind));
        return 3;
    }
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(info->pid));
    if (!process) {
        std::fprintf(stderr, "failed to open daemon pid %lld for termination\n",
                     static_cast<long long>(info->pid));
        return 7;
    }
    if (!TerminateProcess(process, 0)) {
        CloseHandle(process);
        std::fprintf(stderr, "failed to terminate daemon pid %lld\n", static_cast<long long>(info->pid));
        return 7;
    }
    WaitForSingleObject(process, 5000);
    CloseHandle(process);
    remove_bridge_file(profile_root_for(profile));
    std::printf("daemon pid %lld stopped\n", static_cast<long long>(info->pid));
    return 0;
#else
    if (::kill(static_cast<pid_t>(info->pid), SIGTERM) != 0) {
        std::fprintf(stderr, "failed to signal daemon pid %lld\n", static_cast<long long>(info->pid));
        return 7;
    }
    // Grace: wait up to ~5s for clean exit; escalate to SIGKILL if still alive.
    for (int i = 0; i < 50 && is_pid_alive(info->pid); ++i)
        QThread::msleep(100);
    if (is_pid_alive(info->pid)) {
        std::fprintf(stderr, "daemon pid %lld did not exit on SIGTERM; sending SIGKILL\n",
                     static_cast<long long>(info->pid));
        ::kill(static_cast<pid_t>(info->pid), SIGKILL);
        for (int i = 0; i < 20 && is_pid_alive(info->pid); ++i) QThread::msleep(100);
        // A SIGKILLed daemon can't run its aboutToQuit cleanup, so the stale
        // bridge.json may remain; remove it so the next attach/serve is clean.
        remove_bridge_file(profile_root_for(profile));
        std::printf("daemon pid %lld force-stopped (SIGKILL)\n", static_cast<long long>(info->pid));
        return 0;
    }
    std::printf("sent SIGTERM to daemon pid %lld\n", static_cast<long long>(info->pid));
    return 0;
#endif
}

int emit_jobs_list(const QString& profile, bool json) {
    const QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
    if (json) {
        std::printf("%s\n", QJsonDocument(QJsonObject{{"profile", profile}, {"jobs", jobs}})
                                .toJson(QJsonDocument::Compact)
                                .constData());
        return 0;
    }
    if (jobs.isEmpty()) {
        std::printf("no daemon jobs\n");
        return 0;
    }
    std::printf("%-16s %-22s %-15s %-8s %-12s %-8s %-8s %s\n",
                "id", "name", "kind", "enabled", "last", "fails", "timeout", "command");
    for (const auto& v : jobs) {
        const QJsonObject j = v.toObject();
        const QString timeout_label = QStringLiteral("%1s").arg(job_timeout_sec(j));
        std::printf("%-16s %-22s %-15s %-8s %-12s %-8d %-8s %s\n",
                    qUtf8Printable(j.value("id").toString()),
                    qUtf8Printable(j.value("name").toString().left(22)),
                    qUtf8Printable(j.value("kind").toString()),
                    j.value("enabled").toBool() ? "yes" : "no",
                    qUtf8Printable(j.value("last_status").toString("-")),
                    j.value("fail_count").toInt(),
                    qUtf8Printable(timeout_label),
                    qUtf8Printable(json_array_to_strings(j.value("command").toArray()).join(' ')));
    }
    return 0;
}

QString duration_label(qint64 duration_ms) {
    if (duration_ms < 0)
        return QStringLiteral("-");
    if (duration_ms < 1000)
        return QStringLiteral("%1ms").arg(duration_ms);
    return QStringLiteral("%1.%2s").arg(duration_ms / 1000).arg((duration_ms % 1000) / 100);
}

QJsonObject history_row_json(QSqlQuery& q) {
    return QJsonObject{{"run_id", q.value(0).toString()},
                       {"job_id", q.value(1).toString()},
                       {"job_name", q.value(2).toString()},
                       {"kind", q.value(3).toString()},
                       {"trigger", q.value(4).toString()},
                       {"status", q.value(5).toString()},
                       {"exit_code", q.value(6).isNull() ? QJsonValue() : QJsonValue(q.value(6).toInt())},
                       {"started_at", q.value(7).toString()},
                       {"finished_at", q.value(8).toString()},
                       {"duration_ms", q.value(9).isNull() ? QJsonValue() : QJsonValue(static_cast<qint64>(q.value(9).toLongLong()))},
                       {"timeout_sec", q.value(10).isNull() ? QJsonValue() : QJsonValue(q.value(10).toInt())},
                       {"command", q.value(11).toString()},
                       {"error", q.value(12).toString()}};
}

bool consume_history_common(QStringList& args, int* limit, QString* selector) {
    *limit = 20;
    for (int i = 0; i < args.size(); ++i) {
        const QString token = args.at(i);
        if (token == QStringLiteral("--limit")) {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "--limit requires a value\n");
                return false;
            }
            bool ok = false;
            const int value = args.at(i + 1).toInt(&ok);
            if (!ok || value <= 0) {
                std::fprintf(stderr, "--limit requires a positive integer\n");
                return false;
            }
            *limit = value;
            args.removeAt(i + 1);
            args.removeAt(i);
            --i;
            if (*limit <= 0) {
                std::fprintf(stderr, "--limit requires a positive integer\n");
                return false;
            }
        }
    }
    *selector = args.join(' ').trimmed();
    return true;
}

void bind_history_selector(QSqlQuery& q, const QString& selector, const QString& job_id = {}) {
    const QString needle = QStringLiteral("%") + selector + QStringLiteral("%");
    q.bindValue(QStringLiteral(":job_id"), job_id.isEmpty() ? selector : job_id);
    q.bindValue(QStringLiteral(":needle"), needle);
}

int emit_jobs_history(const QString& profile, bool json, QStringList args, bool failures_only = false) {
    int limit = 20;
    QString selector;
    if (!consume_history_common(args, &limit, &selector))
        return 2;

    QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
    QString selected_job_id;
    if (!selector.isEmpty()) {
        const int idx = find_job_index(jobs, selector);
        if (idx >= 0)
            selected_job_id = jobs.at(idx).toObject().value("id").toString();
    }

    DaemonHistoryDb h(profile);
    if (!h.ok()) {
        std::fprintf(stderr, "history db unavailable: %s\n", qUtf8Printable(h.error));
        return 7;
    }
    QString sql = QStringLiteral(
        "SELECT run_id, job_id, job_name, kind, trigger, status, exit_code, started_at, finished_at, "
        "duration_ms, timeout_sec, command, error FROM daemon_job_runs");
    QStringList where;
    if (failures_only)
        where << QStringLiteral("status IN ('failed','timeout','stale-timeout')");
    if (!selector.isEmpty())
        where << QStringLiteral("(job_id=:job_id OR job_name LIKE :needle OR run_id LIKE :needle)");
    if (!where.isEmpty())
        sql += QStringLiteral(" WHERE ") + where.join(QStringLiteral(" AND "));
    sql += QStringLiteral(" ORDER BY started_at DESC LIMIT :limit");

    QSqlQuery q(h.db);
    q.prepare(sql);
    if (!selector.isEmpty())
        bind_history_selector(q, selector, selected_job_id);
    q.bindValue(QStringLiteral(":limit"), limit);
    if (!q.exec()) {
        std::fprintf(stderr, "history query failed: %s\n", qUtf8Printable(q.lastError().text()));
        return 7;
    }

    QJsonArray rows;
    if (!json) {
        std::printf("%-20s %-22s %-10s %-14s %-8s %-8s %s\n",
                    "started", "job", "trigger", "status", "exit", "duration", "run");
    }
    while (q.next()) {
        const QJsonObject row = history_row_json(q);
        rows.append(row);
        if (!json) {
            const QString started = row.value("started_at").toString().left(20);
            const QString job_name = row.value("job_name").toString().left(22);
            const QString trigger = row.value("trigger").toString().left(10);
            const QString status = row.value("status").toString().left(14);
            const QString exit_label = row.value("exit_code").isUndefined() || row.value("exit_code").isNull()
                                           ? QStringLiteral("-")
                                           : QString::number(row.value("exit_code").toInt());
            const QString dur = row.value("duration_ms").isUndefined() || row.value("duration_ms").isNull()
                                    ? QStringLiteral("-")
                                    : duration_label(static_cast<qint64>(row.value("duration_ms").toDouble()));
            std::printf("%-20s %-22s %-10s %-14s %-8s %-8s %s\n",
                        qUtf8Printable(started),
                        qUtf8Printable(job_name),
                        qUtf8Printable(trigger),
                        qUtf8Printable(status),
                        qUtf8Printable(exit_label),
                        qUtf8Printable(dur),
                        qUtf8Printable(row.value("run_id").toString()));
        }
    }
    if (json)
        std::printf("%s\n", QJsonDocument(QJsonObject{{"profile", profile}, {"runs", rows}})
                                .toJson(QJsonDocument::Compact)
                                .constData());
    else if (rows.isEmpty())
        std::printf("no job run history\n");
    return 0;
}

int emit_jobs_stats(const QString& profile, bool json, QStringList args) {
    int limit = 0;
    QString selector;
    if (!consume_history_common(args, &limit, &selector))
        return 2;
    Q_UNUSED(limit);

    QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
    QString selected_job_id;
    if (!selector.isEmpty()) {
        const int idx = find_job_index(jobs, selector);
        if (idx >= 0)
            selected_job_id = jobs.at(idx).toObject().value("id").toString();
    }

    DaemonHistoryDb h(profile);
    if (!h.ok()) {
        std::fprintf(stderr, "history db unavailable: %s\n", qUtf8Printable(h.error));
        return 7;
    }
    QString sql = QStringLiteral(
        "SELECT COUNT(*), "
        "SUM(CASE WHEN status='ok' THEN 1 ELSE 0 END), "
        "SUM(CASE WHEN status IN ('failed','timeout','stale-timeout') THEN 1 ELSE 0 END), "
        "SUM(CASE WHEN status='timeout' THEN 1 ELSE 0 END), "
        "SUM(CASE WHEN status='stale-timeout' THEN 1 ELSE 0 END), "
        "AVG(duration_ms), MAX(started_at) FROM daemon_job_runs");
    if (!selector.isEmpty())
        sql += QStringLiteral(" WHERE (job_id=:job_id OR job_name LIKE :needle OR run_id LIKE :needle)");
    QSqlQuery q(h.db);
    q.prepare(sql);
    if (!selector.isEmpty())
        bind_history_selector(q, selector, selected_job_id);
    if (!q.exec() || !q.next()) {
        std::fprintf(stderr, "stats query failed: %s\n", qUtf8Printable(q.lastError().text()));
        return 7;
    }
    const int total = q.value(0).toInt();
    const int ok = q.value(1).toInt();
    const int failed = q.value(2).toInt();
    const int timeout = q.value(3).toInt();
    const int stale = q.value(4).toInt();
    const double avg_ms = q.value(5).toDouble();
    const QString last = q.value(6).toString();
    QJsonObject out{{"profile", profile},
                    {"selector", selector},
                    {"total", total},
                    {"ok", ok},
                    {"failed", failed},
                    {"timeout", timeout},
                    {"stale_timeout", stale},
                    {"avg_duration_ms", q.value(5).isNull() ? QJsonValue() : QJsonValue(avg_ms)},
                    {"last_started_at", last}};
    if (json) {
        std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
    } else {
        std::printf("runs         %d\n", total);
        std::printf("ok           %d\n", ok);
        std::printf("failed       %d\n", failed);
        std::printf("timeout      %d\n", timeout);
        std::printf("stale        %d\n", stale);
        std::printf("avg duration %s\n", qUtf8Printable(q.value(5).isNull()
                                                        ? QStringLiteral("-")
                                                        : duration_label(static_cast<qint64>(avg_ms))));
        std::printf("last start   %s\n", qUtf8Printable(last.isEmpty() ? QStringLiteral("-") : last));
    }
    return 0;
}

// The daemon-side jobs-lock timeout is deliberately SHORT: jobs_save_update
// and update_job_by_id run on the daemon's single Qt event loop, and
// QLockFile::tryLock blocks the calling thread -- a long contended wait
// would freeze every scheduled job and QProcess finished/error callback for
// its duration. 250ms bounds that stall; on contention the write is dropped
// and the scheduler's next scan (every few seconds) retries. The automation
// writers keep the 5s default (see AutomationState.h): the live order path
// is fail-closed and can afford to wait.
inline constexpr int kJobsLockTimeoutMs = 250;

// Internal, lock-free load->mutate->save cycle. Only called with the "jobs"
// StateLock already held (by jobs_save_update or update_job_by_id) so the
// two entry points never try to re-enter the same QLockFile from one call
// stack.
int jobs_save_update_locked(const QString& profile, const QJsonArray& jobs) {
    QJsonObject doc = load_jobs_doc(profile);
    doc["jobs"] = jobs;
    doc["updated_at"] = now_utc();
    QString error;
    if (!save_jobs_doc(profile, doc, &error)) {
        std::fprintf(stderr, "%s\n", qUtf8Printable(error));
        return 7;
    }
    return 0;
}

int jobs_save_update(const QString& profile, const QJsonArray& jobs) {
    automation::StateLock lock(profile, QStringLiteral("jobs"), kJobsLockTimeoutMs);
    if (!lock.locked()) {
        std::fprintf(stderr, "jobs lock busy\n");
        return 7;
    }
    return jobs_save_update_locked(profile, jobs);
}

bool consume_value(QStringList& args, int& i, const QString& flag, QString* out) {
    if (i + 1 >= args.size()) {
        std::fprintf(stderr, "%s requires a value\n", qUtf8Printable(flag));
        return false;
    }
    *out = args.takeAt(i + 1);
    args.removeAt(i);
    --i;
    return true;
}

bool consume_int(QStringList& args, int& i, const QString& flag, int* out) {
    QString raw;
    if (!consume_value(args, i, flag, &raw))
        return false;
    bool ok = false;
    const int n = raw.toInt(&ok);
    if (!ok || n < 0) {
        std::fprintf(stderr, "%s requires a non-negative integer\n", qUtf8Printable(flag));
        return false;
    }
    *out = n;
    return true;
}

int readiness_rank(const QString& status) {
    if (status == QStringLiteral("not_safe"))
        return 3;
    if (status == QStringLiteral("degraded"))
        return 2;
    if (status == QStringLiteral("watch"))
        return 1;
    return 0;
}

QString merge_readiness_status(QString current, const QString& next) {
    return readiness_rank(next) > readiness_rank(current) ? next : current;
}

QString readiness_overall_label(const QString& status) {
    if (status == QStringLiteral("not_safe"))
        return QStringLiteral("NOT SAFE");
    if (status == QStringLiteral("degraded"))
        return QStringLiteral("DEGRADED");
    if (status == QStringLiteral("watch"))
        return QStringLiteral("WATCH");
    return QStringLiteral("READY");
}

QString age_label_ms(qint64 age_ms) {
    if (age_ms < 0)
        return QStringLiteral("-");
    const qint64 sec = age_ms / 1000;
    if (sec < 90)
        return QStringLiteral("%1s").arg(sec);
    const qint64 min = sec / 60;
    if (min < 90)
        return QStringLiteral("%1m").arg(min);
    const qint64 hour = min / 60;
    if (hour < 48)
        return QStringLiteral("%1h").arg(hour);
    return QStringLiteral("%1d").arg(hour / 24);
}

bool init_readiness_runtime(const QString& profile, QString* error) {
    static headless::HeadlessRuntime rt;
    auto r = rt.init(profile);
    if (!r.ok) {
        if (error)
            *error = r.error;
        return false;
    }
    return true;
}

struct DaemonReadinessOptions {
    QString symbol = QStringLiteral("BTC");
    int tick_stale_sec = 45;
    int market_stale_sec = 300;
    int model_output_stale_sec = 600;
    int train_stale_sec = 86400;
    int min_samples = 30;
    int min_fresh_sources = 2;
};

QJsonObject readiness_check(QString key,
                            QString label,
                            QString status,
                            QString detail,
                            const QStringList& reasons = {},
                            const QJsonObject& metrics = {}) {
    return QJsonObject{{"key", key},
                       {"label", label},
                       {"status", status},
                       {"detail", detail},
                       {"reasons", QJsonArray::fromStringList(reasons)},
                       {"metrics", metrics}};
}

qint64 latest_market_snapshot_ms(const QString& symbol, int* count, QString* error) {
    auto r = Database::instance().execute(
        QStringLiteral("SELECT COUNT(*), MAX(observed_at) FROM edge_prediction_market_snapshots WHERE symbol=?"),
        {symbol.trimmed().toUpper()});
    if (r.is_err()) {
        if (error)
            *error = QString::fromStdString(r.error());
        return 0;
    }
    auto& q = r.value();
    if (!q.next()) {
        if (count)
            *count = 0;
        return 0;
    }
    if (count)
        *count = q.value(0).toInt();
    return q.value(1).toLongLong();
}

QJsonObject build_daemon_readiness(const QString& profile, const DaemonReadinessOptions& opt) {
    const qint64 now = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    QString overall = QStringLiteral("ready");
    QJsonArray checks;
    QStringList blockers;
    QStringList warnings;

    auto add_check = [&](const QJsonObject& check) {
        const QString status = check.value(QStringLiteral("status")).toString(QStringLiteral("ready"));
        overall = merge_readiness_status(overall, status);
        const QJsonArray reasons = check.value(QStringLiteral("reasons")).toArray();
        for (const auto& v : reasons) {
            const QString reason = v.toString();
            if (reason.isEmpty())
                continue;
            if (status == QStringLiteral("not_safe"))
                blockers << reason;
            else if (status != QStringLiteral("ready"))
                warnings << reason;
        }
        checks.append(check);
    };

    const QJsonObject health = daemon_health_object(profile);
    const QJsonObject jobs = health.value(QStringLiteral("jobs")).toObject();
    QStringList scheduler_reasons;
    QString scheduler_status = QStringLiteral("ready");
    if (!health.value(QStringLiteral("running")).toBool()) {
        scheduler_status = QStringLiteral("not_safe");
        if (health.value(QStringLiteral("profile_owner_reachable")).toBool() &&
            health.value(QStringLiteral("active_owner_kind")).toString() != QStringLiteral("daemon")) {
            scheduler_reasons << QStringLiteral("profile is owned by ") +
                                     health.value(QStringLiteral("active_owner_kind")).toString() +
                                     QStringLiteral(", so daemon scheduler is on standby");
        } else {
            scheduler_reasons << QStringLiteral("daemon is not running");
        }
    }
    if (jobs.value(QStringLiteral("enabled")).toInt() <= 0) {
        scheduler_status = merge_readiness_status(scheduler_status, QStringLiteral("degraded"));
        scheduler_reasons << QStringLiteral("no enabled daemon jobs");
    }
    if (jobs.value(QStringLiteral("failed")).toInt() > 0 || jobs.value(QStringLiteral("stale")).toInt() > 0) {
        // Job outcomes are diagnostic, not a substitute for the concrete
        // tick/snapshot/model checks below. Optional research and long-horizon
        // jobs may fail independently; their failures stay visible without
        // incorrectly blocking a healthy BTC signal path.
        scheduler_status = merge_readiness_status(scheduler_status, QStringLiteral("watch"));
        scheduler_reasons << QStringLiteral("one or more daemon jobs currently failed or stale; inspect job health");
    } else if (jobs.value(QStringLiteral("failed_history")).toInt() > 0) {
        scheduler_status = merge_readiness_status(scheduler_status, QStringLiteral("watch"));
        scheduler_reasons << QStringLiteral("daemon has acknowledged historical job failures");
    }
    add_check(readiness_check(QStringLiteral("daemon"),
                              QStringLiteral("Daemon Scheduler"),
                              scheduler_status,
                              health.value(QStringLiteral("running")).toBool()
                                  ? QStringLiteral("daemon is running")
                                  : health.value(QStringLiteral("profile_owner_reachable")).toBool()
                                        ? QStringLiteral("profile endpoint is owned by %1; daemon scheduler is not active")
                                              .arg(health.value(QStringLiteral("active_owner_kind")).toString())
                                        : QStringLiteral("daemon process is unavailable"),
                              scheduler_reasons,
                              QJsonObject{{"running", health.value(QStringLiteral("running")).toBool()},
                                          {"mode", health.value(QStringLiteral("mode")).toString()},
                                          {"profile_owner_reachable", health.value(QStringLiteral("profile_owner_reachable")).toBool()},
                                          {"active_owner_kind", health.value(QStringLiteral("active_owner_kind")).toString()},
                                          {"installed", health.value(QStringLiteral("installed")).toBool()},
                                          {"jobs", jobs}}));

    QString init_error;
    const bool db_ready = init_readiness_runtime(profile, &init_error);
    if (!db_ready) {
        add_check(readiness_check(QStringLiteral("profile_db"),
                                  QStringLiteral("Local Profile DB"),
                                  QStringLiteral("not_safe"),
                                  QStringLiteral("profile database could not be opened"),
                                  {init_error}));
        return QJsonObject{{"profile", profile},
                           {"symbol", opt.symbol},
                           {"overall", readiness_overall_label(overall)},
                           {"status", overall},
                           {"trade_gate", QStringLiteral("NO TRADE")},
                           {"blockers", QJsonArray::fromStringList(blockers)},
                           {"warnings", QJsonArray::fromStringList(warnings)},
                           {"checks", checks},
                           {"generated_at", now_utc()}};
    }

    auto ticks_result = EdgePredictionModelRepository::instance().list_raw_ticks(opt.symbol, 1000);
    QStringList tick_reasons;
    QString tick_status = QStringLiteral("ready");
    QJsonObject latest_by_source;
    qint64 freshest_direct = 0;
    int fresh_direct_sources = 0;
    int advisory_fresh = 0;
    const QStringList direct_sources{QStringLiteral("coinbase"),
                                     QStringLiteral("kraken"),
                                     QStringLiteral("binance"),
                                     QStringLiteral("binanceus")};
    if (ticks_result.is_err()) {
        tick_status = QStringLiteral("not_safe");
        tick_reasons << QStringLiteral("raw tick store is unavailable: ") + QString::fromStdString(ticks_result.error());
    } else {
        QHash<QString, EdgePredictionRawTick> latest;
        for (const auto& t : ticks_result.value()) {
            if (t.source.endsWith(QStringLiteral("-1m-close")))
                continue;
            if (!latest.contains(t.source) || t.received_ts > latest.value(t.source).received_ts)
                latest.insert(t.source, t);
        }
        for (auto it = latest.cbegin(); it != latest.cend(); ++it) {
            const qint64 age_ms = it.value().received_ts > 0 ? std::max<qint64>(0, now - it.value().received_ts) : -1;
            latest_by_source.insert(it.key(), QJsonObject{{"price", it.value().price},
                                                          {"age_ms", age_ms},
                                                          {"age", age_label_ms(age_ms)},
                                                          {"received_ts", QString::number(it.value().received_ts)}});
            if (direct_sources.contains(it.key())) {
                freshest_direct = std::max(freshest_direct, it.value().received_ts);
                if (age_ms >= 0 && age_ms <= static_cast<qint64>(opt.tick_stale_sec) * 1000)
                    ++fresh_direct_sources;
            } else if (it.key() == QStringLiteral("bitcointicker") &&
                       age_ms >= 0 && age_ms <= static_cast<qint64>(opt.tick_stale_sec) * 1000) {
                ++advisory_fresh;
            }
        }
        const qint64 direct_age_ms = freshest_direct > 0 ? std::max<qint64>(0, now - freshest_direct) : -1;
        if (freshest_direct <= 0) {
            tick_status = QStringLiteral("not_safe");
            tick_reasons << QStringLiteral("no live direct exchange tick has been stored");
        } else if (direct_age_ms > static_cast<qint64>(opt.tick_stale_sec) * 1000) {
            tick_status = QStringLiteral("not_safe");
            tick_reasons << QStringLiteral("freshest direct exchange tick is stale");
        }
        if (fresh_direct_sources < opt.min_fresh_sources) {
            tick_status = merge_readiness_status(tick_status, QStringLiteral("degraded"));
            tick_reasons << QStringLiteral("fewer than required live direct exchange sources are fresh");
        }
    }
    add_check(readiness_check(QStringLiteral("ticks"),
                              QStringLiteral("Live BTC Ticks"),
                              tick_status,
                              QStringLiteral("%1 fresh direct source(s), %2 advisory source(s)")
                                  .arg(fresh_direct_sources)
                                  .arg(advisory_fresh),
                              tick_reasons,
                              QJsonObject{{"symbol", opt.symbol},
                                          {"tick_stale_sec", opt.tick_stale_sec},
                                          {"min_fresh_sources", opt.min_fresh_sources},
                                          {"fresh_direct_sources", fresh_direct_sources},
                                          {"fresh_advisory_sources", advisory_fresh},
                                          {"latest_by_source", latest_by_source}}));

    int snapshot_count = 0;
    QString snapshot_error;
    const qint64 latest_snapshot = latest_market_snapshot_ms(opt.symbol, &snapshot_count, &snapshot_error);
    QString market_status = QStringLiteral("ready");
    QStringList market_reasons;
    const qint64 snapshot_age_ms = latest_snapshot > 0 ? std::max<qint64>(0, now - latest_snapshot) : -1;
    if (!snapshot_error.isEmpty()) {
        market_status = QStringLiteral("degraded");
        market_reasons << QStringLiteral("prediction-market snapshot store is unavailable: ") + snapshot_error;
    } else if (snapshot_count <= 0) {
        market_status = QStringLiteral("degraded");
        market_reasons << QStringLiteral("no prediction-market snapshots have been stored yet");
    } else if (snapshot_age_ms > static_cast<qint64>(opt.market_stale_sec) * 1000) {
        market_status = QStringLiteral("degraded");
        market_reasons << QStringLiteral("latest prediction-market snapshot is stale");
    }
    add_check(readiness_check(QStringLiteral("prediction_markets"),
                              QStringLiteral("Prediction Market Snapshots"),
                              market_status,
                              snapshot_count > 0
                                  ? QStringLiteral("latest snapshot age %1").arg(age_label_ms(snapshot_age_ms))
                                  : QStringLiteral("no local market snapshots"),
                              market_reasons,
                              QJsonObject{{"snapshot_count", snapshot_count},
                                          {"latest_observed_at", QString::number(latest_snapshot)},
                                          {"age_ms", snapshot_age_ms},
                                          {"market_stale_sec", opt.market_stale_sec}}));

    auto models_result = EdgePredictionModelRepository::instance().list_models(opt.symbol);
    auto outputs_result = EdgePredictionModelRepository::instance().list_model_outputs(opt.symbol, now, 64);
    // The managed Chronos books publish these three horizons. The former 5m
    // book is deliberately retired, so treating it as a required model makes
    // readiness report a permanent false alarm.
    // EdgePredictionModel canonicalizes the daily horizon as "daily". The
    // Chronos CLI accepts "1d", but readiness queries the local model store.
    const QStringList horizons{QStringLiteral("15m"), QStringLiteral("1h"), QStringLiteral("daily")};
    QJsonArray model_rows;
    QString model_status = QStringLiteral("ready");
    QStringList model_reasons;
    if (models_result.is_err()) {
        model_status = QStringLiteral("not_safe");
        model_reasons << QStringLiteral("model store is unavailable: ") + QString::fromStdString(models_result.error());
    } else {
        QHash<QString, EdgePredictionModelRecord> by_horizon;
        for (const auto& m : models_result.value())
            by_horizon.insert(m.horizon, m);
        for (const auto& h : horizons) {
            QJsonObject row{{"horizon", h}};
            if (!by_horizon.contains(h)) {
                model_status = QStringLiteral("not_safe");
                model_reasons << QStringLiteral("missing trained model for ") + h;
                row["status"] = QStringLiteral("missing");
                model_rows.append(row);
                continue;
            }
            const auto m = by_horizon.value(h);
            const qint64 trained_age = m.trained_at > 0 ? std::max<qint64>(0, now - m.trained_at) : -1;
            QString row_status = QStringLiteral("ready");
            if (m.sample_count < opt.min_samples) {
                row_status = QStringLiteral("not_safe");
                model_status = QStringLiteral("not_safe");
                model_reasons << QStringLiteral("%1 model has only %2 sample(s)").arg(h).arg(m.sample_count);
            } else if (trained_age > static_cast<qint64>(opt.train_stale_sec) * 1000) {
                row_status = QStringLiteral("degraded");
                model_status = merge_readiness_status(model_status, QStringLiteral("degraded"));
                model_reasons << QStringLiteral("%1 model training is stale").arg(h);
            }
            row["status"] = row_status;
            row["sample_count"] = m.sample_count;
            row["brier_score"] = m.brier_score;
            row["trained_at"] = QString::number(m.trained_at);
            row["trained_age_ms"] = trained_age;
            model_rows.append(row);
        }
    }
    add_check(readiness_check(QStringLiteral("models"),
                              QStringLiteral("Horizon Models"),
                              model_status,
                              QStringLiteral("requires %1 settled sample(s) per horizon").arg(opt.min_samples),
                              model_reasons,
                              QJsonObject{{"symbol", opt.symbol},
                                          {"min_samples", opt.min_samples},
                                          {"train_stale_sec", opt.train_stale_sec},
                                          {"horizons", model_rows}}));

    QString output_status = QStringLiteral("ready");
    QStringList output_reasons;
    QJsonArray output_rows;
    if (outputs_result.is_err()) {
        output_status = QStringLiteral("degraded");
        output_reasons << QStringLiteral("model output snapshots are unavailable: ") + QString::fromStdString(outputs_result.error());
    } else {
        QHash<QString, EdgePredictionModelOutput> latest_output;
        for (const auto& o : outputs_result.value()) {
            if (!latest_output.contains(o.horizon) || o.as_of > latest_output.value(o.horizon).as_of)
                latest_output.insert(o.horizon, o);
        }
        for (const auto& h : horizons) {
            QJsonObject row{{"horizon", h}};
            if (!latest_output.contains(h)) {
                output_status = merge_readiness_status(output_status, QStringLiteral("degraded"));
                output_reasons << QStringLiteral("no published model output for ") + h;
                row["status"] = QStringLiteral("missing");
                output_rows.append(row);
                continue;
            }
            const auto o = latest_output.value(h);
            const qint64 age_ms = o.as_of > 0 ? std::max<qint64>(0, now - o.as_of) : -1;
            QString row_status = QStringLiteral("ready");
            if (age_ms > static_cast<qint64>(opt.model_output_stale_sec) * 1000) {
                row_status = QStringLiteral("degraded");
                output_status = merge_readiness_status(output_status, QStringLiteral("degraded"));
                output_reasons << QStringLiteral("%1 model output is stale").arg(h);
            }
            if (o.readiness != QStringLiteral("ready")) {
                row_status = merge_readiness_status(row_status, QStringLiteral("degraded"));
                output_status = merge_readiness_status(output_status, QStringLiteral("degraded"));
                output_reasons << QStringLiteral("%1 model output readiness is %2").arg(h, o.readiness);
            }
            row["status"] = row_status;
            row["readiness"] = o.readiness;
            row["probability"] = o.probability;
            row["confidence"] = o.confidence;
            row["sample_count"] = o.sample_count;
            row["as_of"] = QString::number(o.as_of);
            row["age_ms"] = age_ms;
            output_rows.append(row);
        }
    }
    add_check(readiness_check(QStringLiteral("model_outputs"),
                              QStringLiteral("Published Model Outputs"),
                              output_status,
                              QStringLiteral("latest probability snapshots by horizon"),
                              output_reasons,
                              QJsonObject{{"model_output_stale_sec", opt.model_output_stale_sec},
                                          {"outputs", output_rows}}));

    const QString trade_gate = overall == QStringLiteral("ready") ? QStringLiteral("TRADE GATE READY")
                              : overall == QStringLiteral("watch") ? QStringLiteral("WATCH")
                              : overall == QStringLiteral("degraded") ? QStringLiteral("NO TRADE: DEGRADED")
                                                                        : QStringLiteral("NO TRADE: NOT SAFE");
    warnings.removeDuplicates();
    blockers.removeDuplicates();
    return QJsonObject{{"profile", profile},
                       {"symbol", opt.symbol},
                       {"overall", readiness_overall_label(overall)},
                       {"status", overall},
                       {"trade_gate", trade_gate},
                       {"blockers", QJsonArray::fromStringList(blockers)},
                       {"warnings", QJsonArray::fromStringList(warnings)},
                       {"checks", checks},
                       {"thresholds", QJsonObject{{"tick_stale_sec", opt.tick_stale_sec},
                                                  {"market_stale_sec", opt.market_stale_sec},
                                                  {"model_output_stale_sec", opt.model_output_stale_sec},
                                                  {"train_stale_sec", opt.train_stale_sec},
                                                  {"min_samples", opt.min_samples},
                                                  {"min_fresh_sources", opt.min_fresh_sources}}},
                       {"generated_at", now_utc()}};
}

bool parse_readiness_options(QStringList& args, DaemonReadinessOptions* opt) {
    for (int i = 0; i < args.size(); ++i) {
        const QString flag = args.at(i);
        if (flag == QStringLiteral("--symbol")) {
            QString value;
            if (!consume_value(args, i, flag, &value))
                return false;
            opt->symbol = value.trimmed().isEmpty() ? QStringLiteral("BTC") : value.trimmed().toUpper();
        } else if (flag == QStringLiteral("--tick-stale-sec")) {
            if (!consume_int(args, i, flag, &opt->tick_stale_sec))
                return false;
        } else if (flag == QStringLiteral("--market-stale-sec")) {
            if (!consume_int(args, i, flag, &opt->market_stale_sec))
                return false;
        } else if (flag == QStringLiteral("--model-stale-sec")) {
            if (!consume_int(args, i, flag, &opt->model_output_stale_sec))
                return false;
        } else if (flag == QStringLiteral("--train-stale-sec")) {
            if (!consume_int(args, i, flag, &opt->train_stale_sec))
                return false;
        } else if (flag == QStringLiteral("--min-samples")) {
            if (!consume_int(args, i, flag, &opt->min_samples))
                return false;
        } else if (flag == QStringLiteral("--min-fresh-sources")) {
            if (!consume_int(args, i, flag, &opt->min_fresh_sources))
                return false;
        }
    }
    if (opt->tick_stale_sec < 1 || opt->market_stale_sec < 1 ||
        opt->model_output_stale_sec < 1 || opt->train_stale_sec < 1 ||
        opt->min_samples < 1 || opt->min_fresh_sources < 1) {
        std::fprintf(stderr, "readiness thresholds must be positive integers\n");
        return false;
    }
    if (!args.isEmpty()) {
        std::fprintf(stderr, "unknown args: %s\n", qUtf8Printable(args.join(' ')));
        return false;
    }
    return true;
}

int emit_daemon_readiness(const QString& profile, bool json, QStringList args) {
    DaemonReadinessOptions opt;
    if (!parse_readiness_options(args, &opt)) {
        std::fprintf(stderr,
                     "usage: daemon readiness [--symbol BTC] [--tick-stale-sec N] "
                     "[--market-stale-sec N] [--model-stale-sec N] [--train-stale-sec N] "
                     "[--min-samples N] [--min-fresh-sources N]\n");
        return 2;
    }

    const QJsonObject out = build_daemon_readiness(profile, opt);
    if (json) {
        std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("readiness    %s\n", qUtf8Printable(out.value("overall").toString()));
    std::printf("trade gate   %s\n", qUtf8Printable(out.value("trade_gate").toString()));
    std::printf("profile      %s\n", qUtf8Printable(profile));
    std::printf("symbol       %s\n", qUtf8Printable(opt.symbol));
    const QJsonArray checks = out.value("checks").toArray();
    std::printf("\n%-24s %-10s %s\n", "CHECK", "STATUS", "DETAIL");
    for (const auto& v : checks) {
        const QJsonObject c = v.toObject();
        std::printf("%-24s %-10s %s\n",
                    qUtf8Printable(c.value("label").toString().left(24)),
                    qUtf8Printable(c.value("status").toString().toUpper().left(10)),
                    qUtf8Printable(c.value("detail").toString()));
        const QJsonArray reasons = c.value("reasons").toArray();
        for (const auto& r : reasons)
            if (!r.toString().isEmpty())
                std::printf("  - %s\n", qUtf8Printable(r.toString()));
    }
    const QJsonArray blockers = out.value("blockers").toArray();
    if (!blockers.isEmpty()) {
        std::printf("\nBLOCKERS\n");
        for (const auto& b : blockers)
            std::printf("- %s\n", qUtf8Printable(b.toString()));
    }
    const QJsonArray warnings = out.value("warnings").toArray();
    if (!warnings.isEmpty()) {
        std::printf("\nWARNINGS\n");
        for (const auto& w : warnings)
            std::printf("- %s\n", qUtf8Printable(w.toString()));
    }
    return out.value("status").toString() == QStringLiteral("not_safe") ? 4 : 0;
}

void reconcile_stale_running_jobs(const QString& profile, const QDateTime& now) {
    QJsonObject doc = load_jobs_doc(profile);
    QJsonArray jobs = doc.value("jobs").toArray();
    bool changed = false;
    for (int i = 0; i < jobs.size(); ++i) {
        QJsonObject job = jobs.at(i).toObject();
        if (!job.value("running").toBool())
            continue;
        const QDateTime started = parse_utc(job.value("last_started_at").toString());
        if (!started.isValid())
            continue;
        const int timeout = job_timeout_sec(job);
        if (started.addSecs(timeout + 15) > now)
            continue;
        QString run_id = job.value("current_run_id").toString();
        if (run_id.isEmpty())
            run_id = record_job_run_start(profile, job, QStringLiteral("scheduled"), started.toString(Qt::ISODateWithMs));
        record_job_run_finish(profile, run_id, QStringLiteral("stale-timeout"), -1, {}, {},
                              QStringLiteral("job exceeded timeout or daemon restarted while it was running"));
        job["running"] = false;
        job["last_status"] = QStringLiteral("stale-timeout");
        job["last_exit_code"] = -1;
        job["last_error"] = QStringLiteral("job exceeded timeout or daemon restarted while it was running");
        job["fail_count"] = job.value("fail_count").toInt() + 1;
        job["updated_at"] = now_utc();
        job["current_run_id"] = QString();
        if (job.value("interval_sec").toInt(0) > 0)
            job["next_run_at"] = now.toString(Qt::ISODateWithMs);
        jobs.replace(i, job);
        changed = true;
        append_job_log(profile, QStringLiteral("scheduled-stale id=%1 timeout=%2s")
                                    .arg(job.value("id").toString())
                                    .arg(timeout));
    }
    if (changed)
        jobs_save_update(profile, jobs);
}

QJsonObject parse_job_spec(QString kind,
                           QStringList args,
                           QString* name,
                           int* every_sec,
                           int* timeout_sec,
                           bool* enabled) {
    kind = kind.trimmed().toLower();
    QJsonObject spec;
    *name = {};
    *every_sec = 0;
    *timeout_sec = 0;
    *enabled = true;

    for (int i = 0; i < args.size(); ++i) {
        const QString flag = args.at(i);
        if (flag == "--name") {
            if (!consume_value(args, i, flag, name)) return {};
        } else if (flag == "--every-sec" || flag == "--interval-sec") {
            if (!consume_int(args, i, flag, every_sec)) return {};
        } else if (flag == "--timeout-sec") {
            if (!consume_int(args, i, flag, timeout_sec)) return {};
            if (*timeout_sec < 1) {
                std::fprintf(stderr, "--timeout-sec requires a positive integer\n");
                return {};
            }
        } else if (flag == "--disabled") {
            args.removeAt(i--);
            *enabled = false;
        }
    }

    if (kind == "command") {
        const int sep = args.indexOf(QStringLiteral("--"));
        QStringList command = sep >= 0 ? args.mid(sep + 1) : args;
        if (command.isEmpty()) {
            std::fprintf(stderr, "usage: daemon jobs add command [--name N] [--every-sec N] [--timeout-sec N] -- <cli args...>\n");
            return {};
        }
        spec["command"] = strings_to_json_array(command);
        if (name->isEmpty()) *name = QStringLiteral("command ") + command.join(' ').left(32);
        return spec;
    }

    auto take_named = [&](const QString& flag, QString key) -> bool {
        for (int i = 0; i < args.size(); ++i) {
            if (args.at(i) == flag) {
                QString val;
                if (!consume_value(args, i, flag, &val)) return false;
                spec[key] = val;
                return true;
            }
        }
        return false;
    };
    auto take_named_int = [&](const QString& flag, QString key) -> bool {
        for (int i = 0; i < args.size(); ++i) {
            if (args.at(i) == flag) {
                int val = 0;
                if (!consume_int(args, i, flag, &val)) return false;
                spec[key] = val;
                return true;
            }
        }
        return false;
    };

    if (kind == "brief" || kind == "risk" || kind == "thesis" || kind == "radar") {
        QString target;
        if (!take_named("--target", "target"))
            target = args.join(' ').trimmed();
        else
            target = spec.value("target").toString();
        if (target.isEmpty()) {
            std::fprintf(stderr, "usage: daemon jobs add %s <target> [--every-sec N]\n", qUtf8Printable(kind));
            return {};
        }
        spec["target"] = target;
        if (name->isEmpty()) *name = kind + QStringLiteral(" ") + target;
        return spec;
    }
    if (kind == "ai") {
        QString workflow;
        if (!take_named("--workflow", "workflow")) {
            workflow = args.isEmpty() ? QStringLiteral("brief") : args.takeFirst();
            spec["workflow"] = workflow;
        }
        QString target;
        if (!take_named("--target", "target"))
            target = args.join(' ').trimmed();
        else
            target = spec.value("target").toString();
        if (target.isEmpty()) {
            std::fprintf(stderr, "usage: daemon jobs add ai <brief|risk|thesis|radar> <target> [--every-sec N]\n");
            return {};
        }
        spec["target"] = target;
        if (name->isEmpty()) *name = spec.value("workflow").toString("brief") + QStringLiteral(" ") + target;
        return spec;
    }
    if (kind == "notebook") {
        take_named_int("--cell", "cell");
        QString selector;
        if (!take_named("--selector", "selector"))
            selector = args.join(' ').trimmed();
        else
            selector = spec.value("selector").toString();
        if (selector.isEmpty()) {
            std::fprintf(stderr, "usage: daemon jobs add notebook <id-title-or-path> [--cell N] [--every-sec N]\n");
            return {};
        }
        spec["selector"] = selector;
        if (name->isEmpty()) *name = QStringLiteral("notebook ") + selector;
        return spec;
    }
    if (kind == "paper-strategy" || kind == "paper") {
        kind = QStringLiteral("paper-strategy");
        QString strategy;
        if (!take_named("--strategy", "strategy")) {
            strategy = args.isEmpty() ? QStringLiteral("meanrev") : args.takeFirst();
            spec["strategy"] = strategy;
        }
        take_named("--symbols", "symbols");
        take_named_int("--max-iters", "max_iters");
        take_named_int("--interval-sec", "interval_sec");
        take_named("--max-notional", "max_notional");
        take_named("--max-position", "max_position");
        take_named("--max-aggregate", "max_aggregate");
        if (name->isEmpty()) *name = QStringLiteral("paper ") + spec.value("strategy").toString("meanrev");
        return spec;
    }
    if (kind == "chronos2" || kind == "chronos" || kind == "forecast-book") {
        kind = QStringLiteral("chronos2");
        if (args.removeAll(QStringLiteral("--publish")) > 0)
            spec["publish"] = true;
        take_named("--horizon", "horizon");
        take_named("--model", "model");
        take_named("--device", "device");
        take_named_int("--context-limit", "context_limit");
        if (take_named("--min-journal-edge-bps", "min_journal_edge_bps")) {
        } else if (take_named("--min-edge-bps", "min_journal_edge_bps")) {
        }
        QString symbol;
        if (!take_named("--symbol", "symbol")) {
            symbol = args.isEmpty() ? QStringLiteral("BTC-USD") : args.takeFirst();
            spec["symbol"] = symbol;
        }
        if (spec.value("horizon").toString().isEmpty())
            spec["horizon"] = QStringLiteral("15m");
        if (spec.value("min_journal_edge_bps").isUndefined())
            spec["min_journal_edge_bps"] = 15.0;
        else
            spec["min_journal_edge_bps"] = spec.value("min_journal_edge_bps").toString().toDouble();
        if (!args.isEmpty()) {
            std::fprintf(stderr, "unknown chronos2 job args: %s\n", qUtf8Printable(args.join(' ')));
            return {};
        }
        if (name->isEmpty()) {
            *name = QStringLiteral("chronos2 %1 %2")
                        .arg(spec.value("symbol").toString(QStringLiteral("BTC-USD")),
                             spec.value("horizon").toString(QStringLiteral("15m")));
        }
        return spec;
    }
    if (kind == "chronos2-equity" || kind == "chronos-equity" ||
        kind == "equity-chronos" || kind == "stock-chronos") {
        kind = QStringLiteral("chronos2-equity");
        if (args.removeAll(QStringLiteral("--publish")) > 0)
            spec["publish"] = true;
        take_named("--horizon", "horizon");
        take_named("--period", "period");
        take_named("--interval", "interval");
        take_named("--model", "model");
        take_named("--device", "device");
        take_named_int("--context-limit", "context_limit");
        if (take_named("--min-journal-edge-bps", "min_journal_edge_bps")) {
        } else if (take_named("--min-edge-bps", "min_journal_edge_bps")) {
        }
        QString symbol;
        if (!take_named("--symbol", "symbol")) {
            symbol = args.isEmpty() ? QStringLiteral("AAPL") : args.takeFirst();
            spec["symbol"] = symbol.trimmed().toUpper();
        }
        if (spec.value("horizon").toString().isEmpty())
            spec["horizon"] = QStringLiteral("1d");
        if (spec.value("period").toString().isEmpty())
            spec["period"] = QStringLiteral("2y");
        if (spec.value("min_journal_edge_bps").isUndefined())
            spec["min_journal_edge_bps"] = 50.0;
        else
            spec["min_journal_edge_bps"] = spec.value("min_journal_edge_bps").toString().toDouble();
        if (*every_sec <= 0)
            *every_sec = 86400;
        if (!args.isEmpty()) {
            std::fprintf(stderr, "unknown chronos2-equity job args: %s\n", qUtf8Printable(args.join(' ')));
            return {};
        }
        if (name->isEmpty()) {
            *name = QStringLiteral("chronos2 equity %1 %2")
                        .arg(spec.value("symbol").toString(QStringLiteral("AAPL")),
                             spec.value("horizon").toString(QStringLiteral("1d")));
        }
        return spec;
    }
    if (kind == "notify") {
        take_named("--provider", "provider");
        take_named("--level", "level");
        take_named("--title", "title");
        take_named("--message", "message");
        if (spec.value("title").toString().isEmpty()) {
            spec["title"] = args.isEmpty() ? QStringLiteral("OpenTerminal daemon") : args.takeFirst();
        }
        if (spec.value("message").toString().isEmpty()) {
            spec["message"] = args.join(' ').trimmed();
        }
        if (spec.value("message").toString().isEmpty()) {
            std::fprintf(stderr, "usage: daemon jobs add notify --title T --message M [--provider P]\n");
            return {};
        }
        if (name->isEmpty()) *name = QStringLiteral("notify ") + spec.value("title").toString();
        return spec;
    }
    if (kind == "health-check") {
        if (name->isEmpty()) *name = QStringLiteral("daemon health");
        return spec;
    }

    std::fprintf(stderr, "unknown daemon job kind: %s\n", qUtf8Printable(kind));
    return {};
}

int daemon_jobs_command(const QString& profile, bool json, QStringList args) {
    const QString sub = args.isEmpty() ? QStringLiteral("list") : args.takeFirst().trimmed().toLower();
    if (sub == "list" || sub == "ls")
        return emit_jobs_list(profile, json);
    if (sub == "diagnose" || sub == "health") {
        if (!args.isEmpty()) {
            std::fprintf(stderr, "usage: daemon jobs diagnose\n");
            return 2;
        }
        const QJsonObject summary = jobs_summary(profile);
        QJsonArray current_failures;
        QJsonArray stale_jobs;
        QJsonArray historical;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        for (const QJsonValue& value : load_jobs_doc(profile).value("jobs").toArray()) {
            const QJsonObject job = value.toObject();
            const bool enabled = job.value("enabled").toBool();
            const bool current_failure = job_has_current_failure(job);
            const QDateTime started = parse_utc(job.value("last_started_at").toString());
            const bool stale = job.value("running").toBool() && started.isValid() &&
                               started.addSecs(job_timeout_sec(job) + 15) < now;
            const QJsonObject row{{"id", job.value("id").toString()}, {"name", job.value("name").toString()},
                                  {"kind", job.value("kind").toString()}, {"enabled", enabled},
                                  {"last_status", job.value("last_status").toString()},
                                  {"fail_count", job.value("fail_count").toInt()}};
            if (enabled && current_failure)
                current_failures.append(row);
            else if (enabled && stale)
                stale_jobs.append(row);
            else if (job.value("fail_count").toInt() > 0)
                historical.append(row);
        }
        const QString state = !current_failures.isEmpty() || !stale_jobs.isEmpty()
                                  ? QStringLiteral("attention")
                                  : QStringLiteral("healthy");
        const QJsonObject out{{"state", state}, {"summary", summary},
                              {"current_failures", current_failures}, {"stale_jobs", stale_jobs},
                              {"historical_failures", historical},
                              {"rule", "Only enabled current failures and stale jobs affect daemon readiness. Historical and disabled failures remain visible for cleanup but do not block a healthy execution path."},
                              {"next_command", current_failures.isEmpty() && stale_jobs.isEmpty()
                                   ? QStringLiteral("daemon jobs clear-failures --all")
                                   : QStringLiteral("daemon jobs list")}};
        if (json)
            std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        else
            std::printf("DAEMON JOB DIAGNOSIS: %s\ncurrent enabled failures=%d stale=%d historical/disabled=%d\n%s\n",
                        qUtf8Printable(state.toUpper()), static_cast<int>(current_failures.size()),
                        static_cast<int>(stale_jobs.size()), static_cast<int>(historical.size()),
                        qUtf8Printable(out.value("rule").toString()));
        return 0;
    }
    if (sub == "history" || sub == "hist" || sub == "runs")
        return emit_jobs_history(profile, json, args);
    if (sub == "failures" || sub == "failed")
        return emit_jobs_history(profile, json, args, true);
    if (sub == "stats" || sub == "metrics")
        return emit_jobs_stats(profile, json, args);
    if (sub == "add" || sub == "create") {
        if (args.isEmpty()) {
        std::fprintf(stderr, "usage: daemon jobs add <command|brief|ai|notebook|paper-strategy|chronos2|notify|health-check> ... [--timeout-sec N]\n");
            return 2;
        }
        QString kind = args.takeFirst().trimmed().toLower();
        QString name;
        int every_sec = 0;
        int timeout_sec = 0;
        bool enabled = true;
        QJsonObject spec = parse_job_spec(kind, args, &name, &every_sec, &timeout_sec, &enabled);
        if (spec.isEmpty() && kind != "health-check")
            return 2;
        if (kind == "paper") kind = QStringLiteral("paper-strategy");
        if (kind == "chronos" || kind == "forecast-book") kind = QStringLiteral("chronos2");
        QJsonObject doc = load_jobs_doc(profile);
        QJsonArray jobs = doc.value("jobs").toArray();
        QJsonObject job = make_job(kind, name, spec, every_sec, timeout_sec, enabled);
        jobs.append(job);
        const int rc = jobs_save_update(profile, jobs);
        if (rc != 0) return rc;
        if (json) {
            std::printf("%s\n", QJsonDocument(QJsonObject{{"job", job}}).toJson(QJsonDocument::Compact).constData());
        } else {
            std::printf("added %s  %s\n", qUtf8Printable(job.value("id").toString()),
                        qUtf8Printable(job.value("name").toString()));
        }
        return 0;
    }
    if (sub == "show") {
        if (args.isEmpty()) { std::fprintf(stderr, "usage: daemon jobs show <id-or-name>\n"); return 2; }
        QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
        const int idx = find_job_index(jobs, args.join(' '));
        if (idx < 0) { std::fprintf(stderr, "job not found\n"); return 3; }
        const QJsonObject job = jobs.at(idx).toObject();
        if (json) std::printf("%s\n", QJsonDocument(job).toJson(QJsonDocument::Compact).constData());
        else std::printf("%s\n", QJsonDocument(job).toJson(QJsonDocument::Indented).constData());
        return 0;
    }
    if (sub == "remove" || sub == "rm" || sub == "delete") {
        if (args.isEmpty()) { std::fprintf(stderr, "usage: daemon jobs remove <id-or-name>\n"); return 2; }
        QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
        const int idx = find_job_index(jobs, args.join(' '));
        if (idx < 0) { std::fprintf(stderr, "job not found\n"); return 3; }
        const QJsonObject removed = jobs.at(idx).toObject();
        jobs.removeAt(idx);
        const int rc = jobs_save_update(profile, jobs);
        if (rc != 0) return rc;
        if (json) std::printf("%s\n", QJsonDocument(QJsonObject{{"removed", removed}}).toJson(QJsonDocument::Compact).constData());
        else std::printf("removed %s\n", qUtf8Printable(removed.value("id").toString()));
        return 0;
    }
    if (sub == "enable" || sub == "disable") {
        if (args.isEmpty()) { std::fprintf(stderr, "usage: daemon jobs %s <id-or-name>\n", qUtf8Printable(sub)); return 2; }
        QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
        const int idx = find_job_index(jobs, args.join(' '));
        if (idx < 0) { std::fprintf(stderr, "job not found\n"); return 3; }
        QJsonObject job = jobs.at(idx).toObject();
        job["enabled"] = (sub == "enable");
        job["updated_at"] = now_utc();
        jobs.replace(idx, job);
        const int rc = jobs_save_update(profile, jobs);
        if (rc != 0) return rc;
        if (json) std::printf("%s\n", QJsonDocument(QJsonObject{{"job", job}}).toJson(QJsonDocument::Compact).constData());
        else std::printf("%s %s\n", qUtf8Printable(sub), qUtf8Printable(job.value("id").toString()));
        return 0;
    }
    if (sub == "run") {
        if (args.isEmpty()) { std::fprintf(stderr, "usage: daemon jobs run <id-or-name>\n"); return 2; }
        QJsonObject doc = load_jobs_doc(profile);
        QJsonArray jobs = doc.value("jobs").toArray();
        const int idx = find_job_index(jobs, args.join(' '));
        if (idx < 0) { std::fprintf(stderr, "job not found\n"); return 3; }
        QJsonObject job = jobs.at(idx).toObject();
        const QString run_id = record_job_run_start(profile, job, QStringLiteral("manual"));
        append_job_log(profile, QStringLiteral("manual-start id=%1 name=\"%2\"").arg(job.value("id").toString(), job.value("name").toString()));
        const ProcessResult r = run_job_once_sync(profile, job);
        const QString status = r.exit_code == 0 ? QStringLiteral("ok") : QStringLiteral("failed");
        job["last_run_at"] = now_utc();
        job["last_exit_code"] = r.exit_code;
        job["last_status"] = status;
        job["last_output_tail"] = compact_tail(r.out + "\n" + r.err);
        job["running"] = false;
        job["current_run_id"] = QString();
        job["updated_at"] = now_utc();
        job["run_count"] = job.value("run_count").toInt() + 1;
        if (r.exit_code != 0) job["fail_count"] = job.value("fail_count").toInt() + 1;
        jobs.replace(idx, job);
        jobs_save_update(profile, jobs);
        record_job_run_finish(profile, run_id, status, r.exit_code, r.out, r.err,
                              r.exit_code == 0 ? QString() : compact_tail(r.err));
        append_job_log(profile, QStringLiteral("manual-finish id=%1 exit=%2").arg(job.value("id").toString()).arg(r.exit_code));
        if (json) std::printf("%s\n", QJsonDocument(QJsonObject{{"job", job}, {"stdout", r.out}, {"stderr", r.err}}).toJson(QJsonDocument::Compact).constData());
        else {
            std::printf("%s", qUtf8Printable(r.out));
            if (!r.err.isEmpty()) std::fprintf(stderr, "%s", qUtf8Printable(r.err));
        }
        return r.exit_code == 0 ? 0 : 5;
    }
    if (sub == "repair" || sub == "clear-running") {
        QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
        for (int i = 0; i < jobs.size(); ++i) {
            QJsonObject job = jobs.at(i).toObject();
            if (job.value("running").toBool()) {
                job["running"] = false;
                job["last_status"] = QStringLiteral("repaired");
                job["updated_at"] = now_utc();
                jobs.replace(i, job);
            }
        }
        return jobs_save_update(profile, jobs);
    }
    if (sub == "clear-failures" || sub == "clear-fails" || sub == "ack" || sub == "acknowledge") {
        const bool force = args.removeAll(QStringLiteral("--force")) > 0;
        const bool all = args.removeAll(QStringLiteral("--all")) > 0 || args.isEmpty();
        const QString selector = args.join(' ').trimmed();
        QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
        const int target_idx = all ? -1 : find_job_index(jobs, selector);
        if (!all && target_idx < 0) {
            std::fprintf(stderr, "job not found\n");
            return 3;
        }
        int cleared = 0;
        int skipped = 0;
        QJsonArray changed_jobs;
        for (int i = 0; i < jobs.size(); ++i) {
            QJsonObject job = jobs.at(i).toObject();
            const bool selected = all || target_idx == i;
            if (!selected)
                continue;
            if (job.value("fail_count").toInt() <= 0)
                continue;
            if (!force && job_has_current_failure(job)) {
                ++skipped;
                continue;
            }
            job["fail_count"] = 0;
            if (!job_has_current_failure(job))
                job["last_error"] = QString();
            job["updated_at"] = now_utc();
            jobs.replace(i, job);
            changed_jobs.append(job);
            ++cleared;
        }
        if (!all && selector.isEmpty()) {
            std::fprintf(stderr, "usage: daemon jobs clear-failures [--all|<id-or-name>] [--force]\n");
            return 2;
        }
        const int rc = jobs_save_update(profile, jobs);
        if (rc != 0)
            return rc;
        append_job_log(profile, QStringLiteral("failures-cleared count=%1 skipped=%2 force=%3")
                                    .arg(cleared)
                                    .arg(skipped)
                                    .arg(force ? "yes" : "no"));
        if (json) {
            std::printf("%s\n", QJsonDocument(QJsonObject{{"cleared", cleared},
                                                          {"skipped_current_failures", skipped},
                                                          {"force", force},
                                                          {"jobs", changed_jobs}})
                                    .toJson(QJsonDocument::Compact)
                                    .constData());
        } else {
            std::printf("cleared failure history for %d job%s", cleared, cleared == 1 ? "" : "s");
            if (skipped > 0)
                std::printf(" (%d current failure%s skipped; use --force to clear anyway)",
                            skipped, skipped == 1 ? "" : "s");
            std::printf("\n");
        }
        return 0;
    }
    std::fprintf(stderr, "usage: daemon jobs list|diagnose|history|failures|stats|add|show|run|enable|disable|remove|repair|clear-failures\n");
    return 2;
}

// Returns true only when the mutation was PERSISTED (job found under the
// "jobs" lock and the save succeeded). False means the write was dropped
// (lock contention, job id gone, or save failure) -- callers whose next
// action depends on the persisted state (launch_scheduled_job's running=true
// double-launch guard) must not proceed as if it landed. lock_timeout_ms is
// a test seam; the daemon uses the short kJobsLockTimeoutMs default (see its
// comment for the event-loop-stall rationale).
bool update_job_by_id(const QString& profile, const QString& id,
                      const std::function<void(QJsonObject&)>& fn,
                      int lock_timeout_ms = kJobsLockTimeoutMs) {
    automation::StateLock lock(profile, QStringLiteral("jobs"), lock_timeout_ms);
    if (!lock.locked()) {
        append_job_log(profile, QStringLiteral("update_job_by_id lock busy id=%1 (scheduler retries next scan)").arg(id));
        return false;
    }
    QJsonObject doc = load_jobs_doc(profile);
    QJsonArray jobs = doc.value("jobs").toArray();
    for (int i = 0; i < jobs.size(); ++i) {
        QJsonObject job = jobs.at(i).toObject();
        if (job.value("id").toString() != id)
            continue;
        fn(job);
        job["updated_at"] = now_utc();
        jobs.replace(i, job);
        return jobs_save_update_locked(profile, jobs) == 0;
    }
    return false;
}

void launch_scheduled_job(const QString& profile, const QJsonObject& job) {
    const QString id = job.value("id").toString();
    const QString name = job.value("name").toString();
    const int timeout_sec = job_timeout_sec(job);
    const QString run_id = record_job_run_start(profile, job, QStringLiteral("scheduled"));
    const bool marked_running = update_job_by_id(profile, id, [run_id](QJsonObject& j) {
        j["running"] = true;
        j["last_status"] = QStringLiteral("running");
        j["last_started_at"] = now_utc();
        j["last_error"] = QString();
        j["current_run_id"] = run_id;
    });
    if (!marked_running) {
        // Double-launch guard: if the running=true persist was dropped (jobs
        // lock contention or save failure), the on-disk job still looks idle
        // and the NEXT scheduler scan would launch it again while this
        // process ran -- for a live executor that means two concurrent order
        // processes. Do not start the process at all; close the just-opened
        // history row and let the scheduler retry next tick.
        record_job_run_finish(profile, run_id, QStringLiteral("failed"), -1, {}, {},
                              QStringLiteral("jobs lock busy; launch skipped"));
        append_job_log(profile, QStringLiteral("scheduled-skip id=%1 reason=running-flag-not-persisted "
                                               "(scheduler retries next scan)").arg(id));
        return;
    }
    append_job_log(profile, QStringLiteral("scheduled-start id=%1 name=\"%2\" timeout=%3s")
                                .arg(id, name)
                                .arg(timeout_sec));

    auto* p = new QProcess(qApp);
    auto* timeout = new QTimer(p);
    timeout->setSingleShot(true);
    QStringList args{QStringLiteral("--profile"), profile};
    args << json_array_to_strings(job.value("command").toArray());
    p->setProgram(QCoreApplication::applicationFilePath());
    p->setArguments(args);
    p->setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(timeout, &QTimer::timeout, p, [profile, id, p, timeout_sec]() {
        if (p->state() == QProcess::NotRunning)
            return;
        p->setProperty("daemon_timeout", true);
        append_job_log(profile, QStringLiteral("scheduled-timeout id=%1 timeout=%2s").arg(id).arg(timeout_sec));
        p->kill();
    });
    QObject::connect(p, &QProcess::errorOccurred, qApp, [profile, id, p, run_id](QProcess::ProcessError) {
        p->setProperty("daemon_history_recorded", true);
        update_job_by_id(profile, id, [](QJsonObject& j) {
            j["running"] = false;
            j["last_status"] = QStringLiteral("failed");
            j["last_error"] = QStringLiteral("process start error");
            j["fail_count"] = j.value("fail_count").toInt() + 1;
            j["current_run_id"] = QString();
        });
        record_job_run_finish(profile, run_id, QStringLiteral("failed"), -1, {}, {},
                              QStringLiteral("process start error"));
        append_job_log(profile, QStringLiteral("scheduled-error id=%1").arg(id));
        p->deleteLater();
    });
    QObject::connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     qApp, [profile, id, p, run_id](int exit_code, QProcess::ExitStatus) {
        const QString out = QString::fromUtf8(p->readAllStandardOutput());
        const QString err = QString::fromUtf8(p->readAllStandardError());
        const bool timed_out = p->property("daemon_timeout").toBool();
        const bool history_recorded = p->property("daemon_history_recorded").toBool();
        update_job_by_id(profile, id, [exit_code, out, err, timed_out](QJsonObject& j) {
            const int every = j.value("interval_sec").toInt(0);
            j["running"] = false;
            j["last_run_at"] = now_utc();
            j["last_exit_code"] = timed_out ? -1 : exit_code;
            j["last_status"] = timed_out ? QStringLiteral("timeout")
                                          : (exit_code == 0 ? QStringLiteral("ok") : QStringLiteral("failed"));
            if (timed_out)
                j["last_error"] = QStringLiteral("job exceeded timeout_sec");
            j["last_output_tail"] = compact_tail(out + "\n" + err);
            j["run_count"] = j.value("run_count").toInt() + 1;
            if (timed_out || exit_code != 0)
                j["fail_count"] = j.value("fail_count").toInt() + 1;
            if (every > 0)
                j["next_run_at"] = QDateTime::currentDateTimeUtc().addSecs(every).toString(Qt::ISODateWithMs);
            j["current_run_id"] = QString();
        });
        const QString status = timed_out ? QStringLiteral("timeout")
                                         : (exit_code == 0 ? QStringLiteral("ok") : QStringLiteral("failed"));
        if (!history_recorded) {
            record_job_run_finish(profile, run_id, status, timed_out ? -1 : exit_code, out, err,
                                  timed_out ? QStringLiteral("job exceeded timeout_sec")
                                            : (exit_code == 0 ? QString() : compact_tail(err)));
        }
        append_job_log(profile, timed_out
                                    ? QStringLiteral("scheduled-finish id=%1 status=timeout").arg(id)
                                    : QStringLiteral("scheduled-finish id=%1 exit=%2").arg(id).arg(exit_code));
        p->deleteLater();
    });
    p->start();
    timeout->start(timeout_sec * 1000);
}

void scan_daemon_jobs(const QString& profile) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    reconcile_stale_running_jobs(profile, now);
    const QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
    // Every scheduled command opens the same local profile database. Starting
    // the full due set together creates lock storms that make short accounting
    // jobs appear unhealthy. Keep a bounded queue and let overdue work drain
    // oldest-first over later scheduler scans.
    constexpr int kMaxConcurrentScheduledJobs = 4;
    int running = 0;
    QVector<QPair<QDateTime, QJsonObject>> due_jobs;
    for (const auto& v : jobs) {
        const QJsonObject job = v.toObject();
        if (!job.value("enabled").toBool())
            continue;
        if (job.value("running").toBool()) {
            ++running;
            continue;
        }
        if (job.value("schedule").toString() != QStringLiteral("interval"))
            continue;
        QDateTime due = parse_utc(job.value("next_run_at").toString());
        if (!due.isValid())
            due = now;
        if (due <= now)
            due_jobs.append(qMakePair(due, job));
    }
    std::sort(due_jobs.begin(), due_jobs.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });
    for (const auto& due_job : due_jobs) {
        if (running >= kMaxConcurrentScheduledJobs)
            break;
        launch_scheduled_job(profile, due_job.second);
        ++running;
    }
}

void start_daemon_job_scheduler(const QString& profile) {
    QDir().mkpath(daemon_state_dir(profile));
    QDir().mkpath(daemon_logs_dir(profile));
    append_job_log(profile, QStringLiteral("scheduler-start profile=%1").arg(profile));
    auto* timer = new QTimer(qApp);
    timer->setInterval(5000);
    QObject::connect(timer, &QTimer::timeout, qApp, [profile]() { scan_daemon_jobs(profile); });
    timer->start();
    QTimer::singleShot(0, qApp, [profile]() { scan_daemon_jobs(profile); });
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp,
                     [profile]() { append_job_log(profile, QStringLiteral("scheduler-stop profile=%1").arg(profile)); });
}

int daemon_monitors_command(const QString& profile, bool json, QStringList args) {
    const QString sub = args.isEmpty() ? QStringLiteral("status") : args.takeFirst().trimmed().toLower();
    if (sub == "status" || sub == "list") {
        QJsonObject out{{"profile", profile}, {"jobs", jobs_summary(profile)}, {"health", daemon_health_object(profile)}};
        if (json) {
            std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        } else {
            const QJsonObject j = out.value("jobs").toObject();
            std::printf("monitors/jobs  total=%d enabled=%d running=%d failed=%d stale=%d history=%d interval=%d\n",
                        j.value("total").toInt(), j.value("enabled").toInt(), j.value("running").toInt(),
                        j.value("failed").toInt(), j.value("stale").toInt(),
                        j.value("failed_history").toInt(), j.value("interval").toInt());
        }
        return 0;
    }
    if (sub == "repair" || sub == "restart") {
        return daemon_jobs_command(profile, json, {QStringLiteral("repair")});
    }
    std::fprintf(stderr, "usage: daemon monitors status|repair\n");
    return 2;
}

struct CollectorJobSpec {
    QString key;
    QString name;
    QString description;
    int every_sec = 60;
    int timeout_sec = 60;
    QStringList command;
};

QList<CollectorJobSpec> collector_job_specs() {
    return {
        CollectorJobSpec{
            QStringLiteral("btc-tick-collector"),
            QStringLiteral("collector BTC ticks"),
            QStringLiteral("Keep local BTC ticks warm from Coinbase, Kraken, Binance perpetuals, and advisory Bitcointicker."),
            20,
            30,
            {QStringLiteral("edge"), QStringLiteral("collect"), QStringLiteral("BTC"),
             QStringLiteral("--sources"), QStringLiteral("coinbase,kraken,binanceperp,bitcointicker"),
             QStringLiteral("--stream-ms"), QStringLiteral("8000"),
             QStringLiteral("--timeout-ms"), QStringLiteral("5000")}},
        CollectorJobSpec{
            QStringLiteral("kalshi-snapshot-collector"),
            QStringLiteral("collector Kalshi snapshots"),
            QStringLiteral("Store live Kalshi prediction-market snapshots for edge readiness and audit history."),
            300,
            75,
            {QStringLiteral("edge"), QStringLiteral("snapshot-kalshi"),
             QStringLiteral("--limit"), QStringLiteral("100"),
             QStringLiteral("--timeout-ms"), QStringLiteral("20000")}},
        CollectorJobSpec{
            QStringLiteral("btc5m-market-collector"),
            QStringLiteral("collector BTC 5m market"),
            QStringLiteral("Store current BTC five-minute prediction-market snapshots when available."),
            60,
            45,
            {QStringLiteral("edge"), QStringLiteral("snapshot-btc5m-live"),
             QStringLiteral("--timeout-ms"), QStringLiteral("15000")}},
        CollectorJobSpec{
            QStringLiteral("crypto-universe-recommendation-collector"),
            QStringLiteral("collector crypto universe recommendations"),
            QStringLiteral("Scan the major crypto universe, store cost-aware buy/no-buy recommendations, and keep them available for outcome scoring."),
            300,
            180,
            {QStringLiteral("edge"), QStringLiteral("crypto-universe"),
             QStringLiteral("--venue"), QStringLiteral("coinbase"),
             QStringLiteral("--horizon-sec"), QStringLiteral("60"),
             QStringLiteral("--duration-ms"), QStringLiteral("1500"),
             QStringLiteral("--min-edge-bps"), QStringLiteral("25")}},
        CollectorJobSpec{
            QStringLiteral("crypto-spot-swing-gate-collector"),
            QStringLiteral("collector crypto spot swing gate"),
            QStringLiteral("Scan major spot crypto for larger 1h move candidates using round-trip fees plus a profit buffer, not scalp economics."),
            900,
            120,
            {QStringLiteral("edge"), QStringLiteral("spot-swing-gate"),
             QStringLiteral("--venue"), QStringLiteral("coinbase_advanced"),
             QStringLiteral("--horizon"), QStringLiteral("1h"),
             QStringLiteral("--duration-ms"), QStringLiteral("1500"),
             QStringLiteral("--min-profit-bps"), QStringLiteral("50"),
             QStringLiteral("--max-symbols"), QStringLiteral("5")}},
        CollectorJobSpec{
            QStringLiteral("crypto-scalp-btc-gate"),
            QStringLiteral("collector BTC scalp gate"),
            QStringLiteral("Run the strict BTC scalping gate against live microstructure and journal every pre-trade verdict for later proof scoring."),
            15,
            20,
            {QStringLiteral("edge"), QStringLiteral("scalp-gate"), QStringLiteral("BTC-USD"),
             QStringLiteral("--venue"), QStringLiteral("coinbase"),
             QStringLiteral("--horizon-sec"), QStringLiteral("15"),
             QStringLiteral("--duration-ms"), QStringLiteral("2500"),
             QStringLiteral("--allow-warmup")}},
        CollectorJobSpec{
            QStringLiteral("crypto-scalp-eth-gate"),
            QStringLiteral("collector ETH scalp gate"),
            QStringLiteral("Run the strict ETH scalping gate as a BTC-correlated alt-coin lane and journal every pre-trade verdict for later proof scoring."),
            30,
            20,
            {QStringLiteral("edge"), QStringLiteral("scalp-gate"), QStringLiteral("ETH-USD"),
             QStringLiteral("--venue"), QStringLiteral("coinbase"),
             QStringLiteral("--horizon-sec"), QStringLiteral("15"),
             QStringLiteral("--duration-ms"), QStringLiteral("2500"),
             QStringLiteral("--allow-warmup")}},
        CollectorJobSpec{
            QStringLiteral("crypto-scalp-sol-gate"),
            QStringLiteral("collector SOL scalp gate"),
            QStringLiteral("Run the strict SOL scalping gate as a high-beta crypto lane and journal every pre-trade verdict for later proof scoring."),
            30,
            20,
            {QStringLiteral("edge"), QStringLiteral("scalp-gate"), QStringLiteral("SOL-USD"),
             QStringLiteral("--venue"), QStringLiteral("coinbase"),
             QStringLiteral("--horizon-sec"), QStringLiteral("15"),
             QStringLiteral("--duration-ms"), QStringLiteral("2500"),
             QStringLiteral("--allow-warmup")}},
        CollectorJobSpec{
            QStringLiteral("crypto-universe-recommendation-scorer"),
            QStringLiteral("collector crypto universe scorer"),
            QStringLiteral("Resolve matured crypto recommendations against later ticks so accuracy and post-cost profitability stay current."),
            60,
            120,
            {QStringLiteral("edge"), QStringLiteral("journal"), QStringLiteral("score-crypto"),
             QStringLiteral("--limit"), QStringLiteral("1000"),
             QStringLiteral("--max-age-hours"), QStringLiteral("48")}},
        CollectorJobSpec{
            QStringLiteral("crypto-universe-paper-simulator"),
            QStringLiteral("collector crypto paper simulator"),
            QStringLiteral("Replay resolved crypto BUY recommendations as fixed-size paper trades after estimated fees, spread, and slippage."),
            300,
            90,
            {QStringLiteral("edge"), QStringLiteral("journal"), QStringLiteral("paper-sim"),
             QStringLiteral("--horizon"), QStringLiteral("60s"),
             QStringLiteral("--amount-usd"), QStringLiteral("100"),
             QStringLiteral("--max-age-hours"), QStringLiteral("48")}},
        CollectorJobSpec{
            QStringLiteral("crypto-proof-loop-scoreboard"),
            QStringLiteral("collector crypto proof loop"),
            QStringLiteral("Join crypto signals, no-trade decisions, broker audit events, and matured outcomes into one post-cost proof scoreboard."),
            120,
            90,
            {QStringLiteral("edge"), QStringLiteral("journal"), QStringLiteral("proof-loop"),
             QStringLiteral("--horizon"), QStringLiteral("60s"),
             QStringLiteral("--amount-usd"), QStringLiteral("100"),
             QStringLiteral("--max-age-hours"), QStringLiteral("48"),
             QStringLiteral("--limit"), QStringLiteral("500")}},
        CollectorJobSpec{
            QStringLiteral("crypto-scalp-proof-loop-scoreboard"),
            QStringLiteral("collector scalp proof loop"),
            QStringLiteral("Join 15-second scalp gate verdicts with later ticks so the daemon can prove whether micro-scalp signals beat costs."),
            60,
            90,
            {QStringLiteral("edge"), QStringLiteral("journal"), QStringLiteral("proof-loop"),
             QStringLiteral("--horizon"), QStringLiteral("15s"),
             QStringLiteral("--amount-usd"), QStringLiteral("100"),
             QStringLiteral("--max-age-hours"), QStringLiteral("24"),
             QStringLiteral("--limit"), QStringLiteral("1000")}},
        CollectorJobSpec{
            QStringLiteral("crypto-scalp-trust-scorecard"),
            QStringLiteral("collector scalp trust scorecard"),
            QStringLiteral("Refresh 15-second per-symbol trust scores so the scalp gate can separate useful symbols from noise."),
            120,
            90,
            {QStringLiteral("edge"), QStringLiteral("journal"), QStringLiteral("trust"),
             QStringLiteral("--horizon"), QStringLiteral("15s"),
             QStringLiteral("--max-age-hours"), QStringLiteral("72")}},
        CollectorJobSpec{
            QStringLiteral("crypto-universe-trust-scorecard"),
            QStringLiteral("collector crypto trust scorecard"),
            QStringLiteral("Refresh per-symbol trust scores so weak/noisy coins can be ignored and stronger symbols can be watched."),
            300,
            90,
            {QStringLiteral("edge"), QStringLiteral("journal"), QStringLiteral("trust"),
             QStringLiteral("--horizon"), QStringLiteral("60s"),
             QStringLiteral("--max-age-hours"), QStringLiteral("168")}},
        CollectorJobSpec{
            QStringLiteral("crypto-universe-regime-scorecard"),
            QStringLiteral("collector crypto regime scorecard"),
            QStringLiteral("Bucket crypto recommendation quality by trend, chop, conflict, spread, and thin-data regimes."),
            300,
            90,
            {QStringLiteral("edge"), QStringLiteral("journal"), QStringLiteral("regimes"),
             QStringLiteral("--horizon"), QStringLiteral("60s"),
             QStringLiteral("--max-age-hours"), QStringLiteral("168")}},
        CollectorJobSpec{
            QStringLiteral("crypto-universe-no-trade-dashboard"),
            QStringLiteral("collector crypto no-trade dashboard"),
            QStringLiteral("Keep the latest do-nothing reasons visible: weak signal, stale data, cost, conflict, or low confidence."),
            300,
            90,
            {QStringLiteral("edge"), QStringLiteral("journal"), QStringLiteral("no-trade"),
             QStringLiteral("--limit"), QStringLiteral("25"),
             QStringLiteral("--max-age-hours"), QStringLiteral("24")}},
        CollectorJobSpec{
            QStringLiteral("crypto-universe-rare-edge-alerts"),
            QStringLiteral("collector crypto rare edge alerts"),
            QStringLiteral("Scan fresh recommendations for rare post-cost BUY candidates that pass confidence and trust gates."),
            60,
            60,
            {QStringLiteral("edge"), QStringLiteral("journal"), QStringLiteral("rare-alerts"),
             QStringLiteral("--min-edge-bps"), QStringLiteral("50"),
             QStringLiteral("--min-confidence"), QStringLiteral("45"),
             QStringLiteral("--max-age-min"), QStringLiteral("10")}},
        CollectorJobSpec{
            QStringLiteral("combined-decision-cockpit"),
            QStringLiteral("collector combined decision cockpit"),
            QStringLiteral("Compare crypto spot decisions against BTC 5-minute and Kalshi prediction-market lanes so trade type ambiguity is visible."),
            60,
            60,
            {QStringLiteral("edge"), QStringLiteral("decision-cockpit"),
             QStringLiteral("--symbols"), QStringLiteral("BTC,ETH,SOL"),
             QStringLiteral("--max-age-hours"), QStringLiteral("24")}},
        CollectorJobSpec{
            QStringLiteral("local-lake-edge-mirror"),
            QStringLiteral("collector data lake mirror"),
            QStringLiteral("Mirror recent local edge ticks and model outputs into the profile data lake for DuckDB research."),
            60,
            45,
            {QStringLiteral("data"), QStringLiteral("lake"), QStringLiteral("mirror-edge"),
             QStringLiteral("--symbol"), QStringLiteral("BTC")}},
        CollectorJobSpec{
            QStringLiteral("local-lake-decision-mirror"),
            QStringLiteral("collector data lake decisions"),
            QStringLiteral("Mirror recent edge decision journal rows into the profile data lake."),
            120,
            45,
            {QStringLiteral("data"), QStringLiteral("lake"), QStringLiteral("mirror-decisions"),
             QStringLiteral("--limit"), QStringLiteral("5000")}},
        CollectorJobSpec{
            QStringLiteral("local-lake-broker-event-mirror"),
            QStringLiteral("collector data lake broker events"),
            QStringLiteral("Mirror recent broker/order audit events into the profile data lake."),
            120,
            45,
            {QStringLiteral("data"), QStringLiteral("lake"), QStringLiteral("mirror-broker-events"),
             QStringLiteral("--limit"), QStringLiteral("5000")}},
    };
}

bool job_matches_collector(const QJsonObject& job, const CollectorJobSpec& spec) {
    return job.value(QStringLiteral("managed_by")).toString() == QStringLiteral("daemon-collectors") &&
           job.value(QStringLiteral("collector_key")).toString() == spec.key;
}

QJsonObject make_collector_job(const CollectorJobSpec& spec, const QJsonObject& existing = {}) {
    QJsonObject job = existing.isEmpty()
                          ? make_job(QStringLiteral("command"),
                                     spec.name,
                                     QJsonObject{{"command", strings_to_json_array(spec.command)}},
                                     spec.every_sec,
                                     spec.timeout_sec,
                                     true)
                          : existing;
    const QString now = now_utc();
    job["name"] = spec.name;
    job["kind"] = QStringLiteral("command");
    job["enabled"] = true;
    job["schedule"] = QStringLiteral("interval");
    job["interval_sec"] = spec.every_sec;
    job["timeout_sec"] = spec.timeout_sec;
    job["spec"] = QJsonObject{{"command", strings_to_json_array(spec.command)}};
    job["command"] = strings_to_json_array(spec.command);
    job["managed_by"] = QStringLiteral("daemon-collectors");
    job["collector_key"] = spec.key;
    job["description"] = spec.description;
    job["updated_at"] = now;
    if (job.value(QStringLiteral("created_at")).toString().isEmpty())
        job["created_at"] = now;
    if (job.value(QStringLiteral("id")).toString().isEmpty())
        job["id"] = QStringLiteral("job_%1").arg(QUuid::createUuid().toString(QUuid::Id128).left(12));
    if (job.value(QStringLiteral("next_run_at")).toString().isEmpty())
        job["next_run_at"] = now;
    job["running"] = false;
    job["current_run_id"] = QString();
    return job;
}

QJsonObject collector_state_for_job(const CollectorJobSpec& spec, const QJsonArray& jobs) {
    for (const auto& v : jobs) {
        const QJsonObject job = v.toObject();
        if (!job_matches_collector(job, spec))
            continue;
        return QJsonObject{{"key", spec.key},
                           {"name", spec.name},
                           {"description", spec.description},
                           {"status", "found"},
                           {"id", job.value("id")},
                           {"enabled", job.value("enabled").toBool()},
                           {"running", job.value("running").toBool()},
                           {"interval_sec", job.value("interval_sec").toInt()},
                           {"timeout_sec", job.value("timeout_sec").toInt()},
                           {"last_status", job.value("last_status").toString("-")},
                           {"last_run_at", job.value("last_run_at").toString()},
                           {"next_run_at", job.value("next_run_at").toString()},
                           {"run_count", job.value("run_count").toInt()},
                           {"fail_count", job.value("fail_count").toInt()},
                           {"command", job.value("command").toArray()}};
    }
    return QJsonObject{{"key", spec.key},
                       {"name", spec.name},
                       {"description", spec.description},
                       {"status", "missing"},
                       {"enabled", false},
                       {"running", false},
                       {"interval_sec", spec.every_sec},
                       {"timeout_sec", spec.timeout_sec},
                       {"command", strings_to_json_array(spec.command)}};
}

QJsonObject collectors_status_object(const QString& profile) {
    const QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
    QJsonArray rows;
    int found = 0;
    int enabled = 0;
    int failed = 0;
    int stale = 0;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const auto& spec : collector_job_specs()) {
        const QJsonObject row = collector_state_for_job(spec, jobs);
        if (row.value("status").toString() == QStringLiteral("found"))
            ++found;
        if (row.value("enabled").toBool())
            ++enabled;
        if (row.value("last_status").toString() == QStringLiteral("failed") ||
            row.value("last_status").toString() == QStringLiteral("timeout") ||
            row.value("last_status").toString() == QStringLiteral("stale-timeout"))
            ++failed;
        const QDateTime last = parse_utc(row.value("last_run_at").toString());
        if (last.isValid() && last.addSecs(std::max(120, row.value("interval_sec").toInt() * 3)) < now)
            ++stale;
        rows.append(row);
    }
    return QJsonObject{{"profile", profile},
                       {"total", collector_job_specs().size()},
                       {"found", found},
                       {"enabled", enabled},
                       {"failed", failed},
                       {"stale", stale},
                       {"collectors", rows}};
}

QJsonObject repair_collectors(const QString& profile) {
    QJsonObject doc = load_jobs_doc(profile);
    QJsonArray jobs = doc.value("jobs").toArray();
    QJsonArray changed;
    int created = 0;
    int updated = 0;
    for (const auto& spec : collector_job_specs()) {
        int idx = -1;
        for (int i = 0; i < jobs.size(); ++i) {
            if (job_matches_collector(jobs.at(i).toObject(), spec)) {
                idx = i;
                break;
            }
        }
        if (idx >= 0) {
            const QJsonObject fixed = make_collector_job(spec, jobs.at(idx).toObject());
            jobs.replace(idx, fixed);
            changed.append(fixed);
            ++updated;
        } else {
            const QJsonObject created_job = make_collector_job(spec);
            jobs.append(created_job);
            changed.append(created_job);
            ++created;
        }
    }
    const int rc = jobs_save_update(profile, jobs);
    append_job_log(profile, QStringLiteral("collectors-repair created=%1 updated=%2 rc=%3")
                                .arg(created)
                                .arg(updated)
                                .arg(rc));
    return QJsonObject{{"profile", profile},
                       {"created", created},
                       {"updated", updated},
                       {"ok", rc == 0},
                       {"jobs", changed},
                       {"status", collectors_status_object(profile)}};
}

QJsonArray tail_jsonl_objects(const QString& path, int limit, const QString& symbol_filter = {}) {
    QJsonArray rows;
    const QString text = tail_text(path, std::max(1, limit) * 4);
    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        const QJsonObject obj = doc.object();
        if (!symbol_filter.isEmpty() &&
            obj.value(QStringLiteral("symbol")).toString().compare(symbol_filter, Qt::CaseInsensitive) != 0)
            continue;
        rows.append(obj);
    }
    while (rows.size() > limit)
        rows.removeFirst();
    return rows;
}

int daemon_scalp_command(const QString& profile, bool json, QStringList args) {
    const QString sub = args.isEmpty() ? QStringLiteral("status") : args.takeFirst().trimmed().toLower();
    if (sub == "venues" || sub == "costs" || sub == "compare") {
        QString liquidity_raw;
        QString slippage_raw;
        QString safety_raw;
        QString min_profit_raw;
        for (int i = 0; i < args.size(); ++i) {
            const QString flag = args.at(i);
            if (flag == QStringLiteral("--liquidity")) {
                if (!consume_value(args, i, flag, &liquidity_raw)) return 2;
            } else if (flag == QStringLiteral("--slippage-bps")) {
                if (!consume_value(args, i, flag, &slippage_raw)) return 2;
            } else if (flag == QStringLiteral("--safety-bps")) {
                if (!consume_value(args, i, flag, &safety_raw)) return 2;
            } else if (flag == QStringLiteral("--min-profit-bps") || flag == QStringLiteral("--minimum-profit-bps")) {
                if (!consume_value(args, i, flag, &min_profit_raw)) return 2;
            } else if (flag == QStringLiteral("--maker") || flag == QStringLiteral("--post-only")) {
                args.removeAt(i--);
                liquidity_raw = QStringLiteral("maker");
            } else if (flag == QStringLiteral("--taker") || flag == QStringLiteral("--market")) {
                args.removeAt(i--);
                liquidity_raw = QStringLiteral("taker");
            }
        }
        if (!args.isEmpty()) {
            std::fprintf(stderr, "unknown args: %s\n", qUtf8Printable(args.join(' ')));
            return 2;
        }
        auto parse_double = [](const QString& raw, double fallback, const char* label, bool* ok_out) {
            if (raw.trimmed().isEmpty())
                return fallback;
            bool ok = false;
            const double v = raw.toDouble(&ok);
            if (!ok || !std::isfinite(v) || v < 0.0 || v > 1000.0) {
                std::fprintf(stderr, "%s must be 0.00..1000.00\n", label);
                *ok_out = false;
                return fallback;
            }
            return v;
        };
        bool ok = true;
        const QString liquidity_mode = scalp_liquidity_mode(liquidity_raw);
        const double slippage_bps = parse_double(slippage_raw, 0.0, "--slippage-bps", &ok);
        const double safety_bps = parse_double(safety_raw, 5.0, "--safety-bps", &ok);
        const double min_profit_bps = parse_double(min_profit_raw, 10.0, "--min-profit-bps", &ok);
        if (!ok)
            return 2;
        const QStringList venues = scalp_default_venue_ladder();
        QJsonArray rows;
        for (const QString& venue : venues) {
            const ScalpVenueFeeProfile profile = scalp_fee_profile(venue);
            const double fee_bps = liquidity_mode == QLatin1String("maker") ? profile.maker_bps : profile.taker_bps;
            const double round_trip = (fee_bps * 2.0) + (slippage_bps * 2.0) + safety_bps;
            rows.append(QJsonObject{{"venue", venue},
                                    {"source", profile.source},
                                    {"liquidity", liquidity_mode},
                                    {"entry_fee_bps", fee_bps},
                                    {"exit_fee_bps", fee_bps},
                                    {"slippage_bps", slippage_bps},
                                    {"safety_bps", safety_bps},
                                    {"round_trip_cost_bps", round_trip},
                                    {"minimum_profit_bps", min_profit_bps},
                                    {"required_edge_bps", round_trip + min_profit_bps}});
        }
        if (json) {
            std::printf("%s\n", QJsonDocument(QJsonObject{{"profile", profile}, {"rows", rows}})
                                    .toJson(QJsonDocument::Compact).constData());
        } else {
            std::printf("SCALP VENUE COST FLOOR (%s)\n", qUtf8Printable(liquidity_mode));
            std::printf("%-18s %-9s %-9s %-9s %-9s %-9s\n",
                        "VENUE", "FEE", "RT_COST", "MIN_PROF", "REQ_EDGE", "NOTE");
            for (const auto& v : rows) {
                const QJsonObject o = v.toObject();
                std::printf("%-18s %-9.2f %-9.2f %-9.2f %-9.2f %s\n",
                            qUtf8Printable(o.value("venue").toString()),
                            o.value("entry_fee_bps").toDouble(),
                            o.value("round_trip_cost_bps").toDouble(),
                            o.value("minimum_profit_bps").toDouble(),
                            o.value("required_edge_bps").toDouble(),
                            o.value("required_edge_bps").toDouble() <= 35.0 ? "paper-testable" : "high hurdle");
            }
        }
        return 0;
    }

    if (sub == "explore" || sub == "viability" || sub == "micro" || sub == "can-scalp") {
        QString amount_raw;
        QString venues_raw;
        QString liquidity_raw;
        QString slippage_raw;
        QString safety_raw;
        QString min_profit_raw;
        QString min_profit_cents_raw;
        QString confidence_raw;
        QString capture_raw;
        QString target_raw;
        QString style_raw;
        for (int i = 0; i < args.size(); ++i) {
            const QString flag = args.at(i);
            if (flag == QStringLiteral("--amount") || flag == QStringLiteral("--amount-usd") ||
                flag == QStringLiteral("--notional")) {
                if (!consume_value(args, i, flag, &amount_raw)) return 2;
            } else if (flag == QStringLiteral("--style") || flag == QStringLiteral("--mode")) {
                if (!consume_value(args, i, flag, &style_raw)) return 2;
            } else if (flag == QStringLiteral("--venues") || flag == QStringLiteral("--venue")) {
                if (!consume_value(args, i, flag, &venues_raw)) return 2;
            } else if (flag == QStringLiteral("--liquidity")) {
                if (!consume_value(args, i, flag, &liquidity_raw)) return 2;
            } else if (flag == QStringLiteral("--slippage-bps")) {
                if (!consume_value(args, i, flag, &slippage_raw)) return 2;
            } else if (flag == QStringLiteral("--safety-bps")) {
                if (!consume_value(args, i, flag, &safety_raw)) return 2;
            } else if (flag == QStringLiteral("--min-profit-bps") || flag == QStringLiteral("--minimum-profit-bps")) {
                if (!consume_value(args, i, flag, &min_profit_raw)) return 2;
            } else if (flag == QStringLiteral("--min-profit-cents")) {
                if (!consume_value(args, i, flag, &min_profit_cents_raw)) return 2;
            } else if (flag == QStringLiteral("--confidence") || flag == QStringLiteral("--min-confidence")) {
                if (!consume_value(args, i, flag, &confidence_raw)) return 2;
            } else if (flag == QStringLiteral("--capture-ratio")) {
                if (!consume_value(args, i, flag, &capture_raw)) return 2;
            } else if (flag == QStringLiteral("--target-bps") || flag == QStringLiteral("--target-move-bps") ||
                       flag == QStringLiteral("--observed-move-bps")) {
                if (!consume_value(args, i, flag, &target_raw)) return 2;
            } else if (flag == QStringLiteral("--maker") || flag == QStringLiteral("--post-only")) {
                args.removeAt(i--);
                liquidity_raw = QStringLiteral("maker");
            } else if (flag == QStringLiteral("--taker") || flag == QStringLiteral("--market")) {
                args.removeAt(i--);
                liquidity_raw = QStringLiteral("taker");
            }
        }
        if (!args.isEmpty()) {
            std::fprintf(stderr, "unknown args: %s\n", qUtf8Printable(args.join(' ')));
            return 2;
        }
        auto parse_double = [](const QString& raw,
                               double fallback,
                               double min_v,
                               double max_v,
                               const char* label,
                               bool* ok_out) {
            if (raw.trimmed().isEmpty())
                return fallback;
            bool ok = false;
            const double v = raw.toDouble(&ok);
            if (!ok || !std::isfinite(v) || v < min_v || v > max_v) {
                std::fprintf(stderr, "%s must be %.2f..%.2f\n", label, min_v, max_v);
                *ok_out = false;
                return fallback;
            }
            return v;
        };
        bool ok = true;
        QString style = style_raw.trimmed().toLower();
        if (style.isEmpty())
            style = QStringLiteral("scalp");
        if (style == QLatin1String("spot-swing") || style == QLatin1String("swing") ||
            style == QLatin1String("intrahour") || style == QLatin1String("buy-sell"))
            style = QStringLiteral("spot");
        if (style != QLatin1String("scalp") && style != QLatin1String("spot")) {
            std::fprintf(stderr, "--style must be scalp or spot\n");
            return 2;
        }
        const bool spot_style = style == QLatin1String("spot");
        const double amount_usd = parse_double(amount_raw, 50.0, 1.0, 1000000.0, "--amount", &ok);
        const QString liquidity_mode = scalp_liquidity_mode(liquidity_raw);
        const double slippage_bps = parse_double(slippage_raw, 0.0, 0.0, 1000.0, "--slippage-bps", &ok);
        const double safety_bps = parse_double(safety_raw, 5.0, 0.0, 1000.0, "--safety-bps", &ok);
        double min_profit_bps = parse_double(min_profit_raw, spot_style ? 25.0 : 10.0,
                                             0.0, 1000.0, "--min-profit-bps", &ok);
        const double min_profit_cents = parse_double(min_profit_cents_raw, 0.0, 0.0, 100000.0,
                                                     "--min-profit-cents", &ok);
        if (min_profit_cents > 0.0)
            min_profit_bps = std::max(min_profit_bps, (min_profit_cents / 100.0) / amount_usd * 10000.0);
        const double confidence = normalize_confidence_gate(
            parse_double(confidence_raw, 80.0, 0.0, 100.0, "--confidence", &ok));
        const double capture_ratio = parse_double(capture_raw, spot_style ? 0.55 : 0.35,
                                                  0.01, 1.0, "--capture-ratio", &ok);
        QVector<double> targets;
        const QStringList target_parts = split_csv(target_raw.trimmed().isEmpty()
                                                       ? (spot_style ? QStringLiteral("50,100,250,500,1000")
                                                                     : QStringLiteral("25,50,100,250"))
                                                       : target_raw);
        for (const QString& raw : target_parts) {
            bool target_ok = false;
            const double target = raw.toDouble(&target_ok);
            if (!target_ok || !std::isfinite(target) || target < 0.0 || target > 10000.0) {
                std::fprintf(stderr, "--target-bps values must be 0.00..10000.00\n");
                ok = false;
                break;
            }
            targets << target;
        }
        if (targets.isEmpty())
            targets = spot_style ? QVector<double>{50.0, 100.0, 250.0, 500.0, 1000.0}
                                 : QVector<double>{25.0, 50.0, 100.0, 250.0};
        if (!ok)
            return 2;

        QStringList venues = split_csv(venues_raw);
        if (venues.isEmpty())
            venues = scalp_default_venue_ladder();
        for (QString& venue : venues)
            venue = scalp_fee_venue(venue);
        venues.removeDuplicates();

        QJsonArray rows;
        const double signal_factor = confidence * capture_ratio;
        for (const QString& venue : venues) {
            const ScalpVenueFeeProfile fee_profile = scalp_fee_profile(venue);
            const double fee_bps = liquidity_mode == QLatin1String("maker") ? fee_profile.maker_bps
                                                                            : fee_profile.taker_bps;
            const double round_trip_bps = (fee_bps * 2.0) + (slippage_bps * 2.0) + safety_bps;
            const double required_edge_bps = round_trip_bps + min_profit_bps;
            const double min_observed_move_bps = signal_factor > 0.0
                                                     ? required_edge_bps / signal_factor
                                                     : std::numeric_limits<double>::infinity();
            QJsonArray target_rows;
            for (const double target : targets) {
                const double expected_captured_bps = target * signal_factor;
                const double expected_net_bps = expected_captured_bps - required_edge_bps;
                target_rows.append(QJsonObject{{"observed_move_bps", target},
                                               {"expected_captured_bps", expected_captured_bps},
                                               {"expected_net_bps", expected_net_bps},
                                               {"expected_net_usd", amount_usd * expected_net_bps / 10000.0},
                                               {"passes", expected_net_bps >= 0.0}});
            }
            rows.append(QJsonObject{{"venue", venue},
                                    {"source", fee_profile.source},
                                    {"liquidity", liquidity_mode},
                                    {"entry_fee_bps", fee_bps},
                                    {"exit_fee_bps", fee_bps},
                                    {"slippage_bps", slippage_bps},
                                    {"safety_bps", safety_bps},
                                    {"minimum_profit_bps", min_profit_bps},
                                    {"round_trip_cost_bps", round_trip_bps},
                                    {"required_edge_bps", required_edge_bps},
                                    {"required_edge_usd", amount_usd * required_edge_bps / 10000.0},
                                    {"round_trip_cost_usd", amount_usd * round_trip_bps / 10000.0},
                                    {"minimum_profit_usd", amount_usd * min_profit_bps / 10000.0},
                                    {"confidence", confidence},
                                    {"capture_ratio", capture_ratio},
                                    {"min_observed_move_bps", min_observed_move_bps},
                                    {"verdict", scalp_viability_label(min_observed_move_bps)},
                                    {"target_checks", target_rows}});
        }

        const QJsonObject out{{"profile", profile},
                              {"style", style},
                              {"amount_usd", amount_usd},
                              {"liquidity", liquidity_mode},
                              {"confidence", confidence},
                              {"capture_ratio", capture_ratio},
                              {"signal_factor", signal_factor},
                              {"note", QStringLiteral("Observed move is discounted by confidence * capture_ratio before costs.")},
                              {"rows", rows}};
        if (json) {
            std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        } else {
            std::printf("SCALP MICRO-PROFIT EXPLORER\n");
            std::printf("style=%s amount=$%.2f liquidity=%s confidence=%.0f%% capture=%.0f%% min_profit=%.2fbps ($%.4f)\n",
                        qUtf8Printable(style),
                        amount_usd,
                        qUtf8Printable(liquidity_mode),
                        confidence * 100.0,
                        capture_ratio * 100.0,
                        min_profit_bps,
                        amount_usd * min_profit_bps / 10000.0);
            std::printf("Rule: expected captured move must beat fee + slippage + safety + desired profit.\n\n");
            if (spot_style)
                std::printf("Spot mode assumes slower buy/sell attempts, so it tests larger target moves and a larger desired profit buffer.\n\n");
            std::printf("%-18s %-7s %-8s %-8s %-10s %-10s %-16s %s\n",
                        "VENUE", "FEE", "RT_COST", "REQ", "$REQ", "$COST", "OBS_MOVE_NEEDED", "VERDICT");
            for (const auto& v : rows) {
                const QJsonObject o = v.toObject();
                std::printf("%-18s %-7.2f %-8.2f %-8.2f $%-9.4f $%-9.4f %-16.2f %s\n",
                            qUtf8Printable(o.value("venue").toString()),
                            o.value("entry_fee_bps").toDouble(),
                            o.value("round_trip_cost_bps").toDouble(),
                            o.value("required_edge_bps").toDouble(),
                            o.value("required_edge_usd").toDouble(),
                            o.value("round_trip_cost_usd").toDouble(),
                            o.value("min_observed_move_bps").toDouble(),
                            qUtf8Printable(o.value("verdict").toString()));
            }
            std::printf("\nTARGET MOVE CHECK (net after cost for observed move bps)\n");
            for (const auto& v : rows) {
                const QJsonObject o = v.toObject();
                QStringList parts;
                for (const auto& t : o.value("target_checks").toArray()) {
                    const QJsonObject check = t.toObject();
                    parts << QStringLiteral("%1bps:%2$%3")
                                 .arg(QString::number(check.value("observed_move_bps").toDouble(), 'f', 0),
                                      check.value("passes").toBool() ? QStringLiteral("+") : QString(),
                                      QString::number(check.value("expected_net_usd").toDouble(), 'f', 4));
                }
                std::printf("%-18s %s\n", qUtf8Printable(o.value("venue").toString()),
                            qUtf8Printable(parts.join(QStringLiteral("  "))));
            }
            const double penny_bps = 0.01 / amount_usd * 10000.0;
            std::printf("\nAt $%.2f, one cent is %.2fbps. If OBS_MOVE_NEEDED is far above that, penny scalping is fee-blocked.\n",
                        amount_usd, penny_bps);
        }
        return 0;
    }

    if (sub == "start" || sub == "enable" || sub == "run") {
        QString symbols_raw;
        QString sources_raw;
        QString amounts_raw;
        QString cadence_raw;
        QString venue_raw;
        QString liquidity_raw;
        QString fee_raw;
        QString slippage_raw;
        QString safety_raw;
        QString min_profit_raw;
        QString min_net_raw;
        QString min_confidence_raw;
        QString capture_raw;
        QString max_age_raw;
        QString max_spread_raw;
        QString min_sources_raw;
        bool paper = true;
        for (int i = 0; i < args.size(); ++i) {
            const QString flag = args.at(i);
            if (flag == QStringLiteral("--symbols")) {
                if (!consume_value(args, i, flag, &symbols_raw)) return 2;
            } else if (flag == QStringLiteral("--sources")) {
                if (!consume_value(args, i, flag, &sources_raw)) return 2;
            } else if (flag == QStringLiteral("--amounts") || flag == QStringLiteral("--amount-usd") ||
                       flag == QStringLiteral("--paper-amounts")) {
                if (!consume_value(args, i, flag, &amounts_raw)) return 2;
            } else if (flag == QStringLiteral("--cadence-ms") || flag == QStringLiteral("--interval-ms")) {
                if (!consume_value(args, i, flag, &cadence_raw)) return 2;
            } else if (flag == QStringLiteral("--venue")) {
                if (!consume_value(args, i, flag, &venue_raw)) return 2;
            } else if (flag == QStringLiteral("--liquidity")) {
                if (!consume_value(args, i, flag, &liquidity_raw)) return 2;
            } else if (flag == QStringLiteral("--fee-bps")) {
                if (!consume_value(args, i, flag, &fee_raw)) return 2;
            } else if (flag == QStringLiteral("--slippage-bps")) {
                if (!consume_value(args, i, flag, &slippage_raw)) return 2;
            } else if (flag == QStringLiteral("--safety-bps")) {
                if (!consume_value(args, i, flag, &safety_raw)) return 2;
            } else if (flag == QStringLiteral("--min-profit-bps") || flag == QStringLiteral("--minimum-profit-bps")) {
                if (!consume_value(args, i, flag, &min_profit_raw)) return 2;
            } else if (flag == QStringLiteral("--min-net-bps")) {
                if (!consume_value(args, i, flag, &min_net_raw)) return 2;
            } else if (flag == QStringLiteral("--min-confidence") || flag == QStringLiteral("--confidence")) {
                if (!consume_value(args, i, flag, &min_confidence_raw)) return 2;
            } else if (flag == QStringLiteral("--capture-ratio")) {
                if (!consume_value(args, i, flag, &capture_raw)) return 2;
            } else if (flag == QStringLiteral("--max-age-ms")) {
                if (!consume_value(args, i, flag, &max_age_raw)) return 2;
            } else if (flag == QStringLiteral("--max-spread-bps")) {
                if (!consume_value(args, i, flag, &max_spread_raw)) return 2;
            } else if (flag == QStringLiteral("--min-live-sources")) {
                if (!consume_value(args, i, flag, &min_sources_raw)) return 2;
            } else if (flag == QStringLiteral("--paper")) {
                args.removeAt(i--);
                paper = true;
            } else if (flag == QStringLiteral("--maker") || flag == QStringLiteral("--post-only")) {
                args.removeAt(i--);
                liquidity_raw = QStringLiteral("maker");
            } else if (flag == QStringLiteral("--taker") || flag == QStringLiteral("--market")) {
                args.removeAt(i--);
                liquidity_raw = QStringLiteral("taker");
            } else if (flag == QStringLiteral("--live")) {
                std::fprintf(stderr, "daemon scalp is paper-only in this release; live execution is intentionally blocked\n");
                return 2;
            }
        }

        QStringList symbols = split_csv(symbols_raw);
        if (symbols.isEmpty())
            symbols = args.isEmpty() ? QStringList{QStringLiteral("BTC-USD")} : args;
        for (QString& symbol : symbols)
            symbol = services::crypto_latency::CryptoLatencyService::normalize_symbol(symbol);
        symbols.removeDuplicates();

        QStringList sources = split_csv(sources_raw);
        if (sources.isEmpty())
            sources = services::crypto_latency::CryptoLatencyService::default_sources();
        for (QString& source : sources)
            source = source.trimmed().toLower();
        sources.removeDuplicates();

        QVector<double> paper_amounts;
        for (const QString& raw : split_csv(amounts_raw)) {
            bool amount_ok = false;
            const double amount = raw.toDouble(&amount_ok);
            if (!amount_ok || amount <= 0.0 || amount > 1000000.0) {
                std::fprintf(stderr, "--amounts values must be positive USD notionals\n");
                return 2;
            }
            paper_amounts << amount;
        }
        if (paper_amounts.isEmpty())
            paper_amounts = {25.0, 50.0};
        std::sort(paper_amounts.begin(), paper_amounts.end());
        paper_amounts.erase(std::unique(paper_amounts.begin(), paper_amounts.end()), paper_amounts.end());

        auto parse_int = [](const QString& raw, int fallback, int min_v, int max_v, const char* label, bool* ok_out) {
            if (raw.trimmed().isEmpty())
                return fallback;
            bool ok = false;
            const int v = raw.toInt(&ok);
            if (!ok || v < min_v || v > max_v) {
                std::fprintf(stderr, "%s must be %d..%d\n", label, min_v, max_v);
                *ok_out = false;
                return fallback;
            }
            return v;
        };
        auto parse_double = [](const QString& raw,
                               double fallback,
                               double min_v,
                               double max_v,
                               const char* label,
                               bool* ok_out) {
            if (raw.trimmed().isEmpty())
                return fallback;
            bool ok = false;
            const double v = raw.toDouble(&ok);
            if (!ok || !std::isfinite(v) || v < min_v || v > max_v) {
                std::fprintf(stderr, "%s must be %.2f..%.2f\n", label, min_v, max_v);
                *ok_out = false;
                return fallback;
            }
            return v;
        };

        bool ok = true;
        const int cadence_ms = parse_int(cadence_raw, 250, 50, 5000, "--cadence-ms", &ok);
        const int max_age_ms = parse_int(max_age_raw, 1000, 50, 30000, "--max-age-ms", &ok);
        const int min_live_sources = parse_int(min_sources_raw, 2, 1, 10, "--min-live-sources", &ok);
        const QString fee_venue = scalp_fee_venue(venue_raw);
        const QString liquidity_mode = scalp_liquidity_mode(liquidity_raw);
        const double fee_bps = parse_double(fee_raw, scalp_default_fee_bps(fee_venue, liquidity_mode),
                                            0.0, 1000.0, "--fee-bps", &ok);
        const double slippage_bps = parse_double(slippage_raw, 0.0, 0.0, 1000.0, "--slippage-bps", &ok);
        const double safety_bps = parse_double(safety_raw, 5.0, 0.0, 1000.0, "--safety-bps", &ok);
        if (min_profit_raw.trimmed().isEmpty() && !min_net_raw.trimmed().isEmpty())
            min_profit_raw = min_net_raw;
        const double min_profit_bps = parse_double(min_profit_raw, 10.0, 0.0, 1000.0, "--min-profit-bps", &ok);
        const double min_confidence = normalize_confidence_gate(
            parse_double(min_confidence_raw, 0.0, 0.0, 100.0, "--min-confidence", &ok));
        const double capture_ratio = parse_double(capture_raw, 0.35, 0.01, 1.0, "--capture-ratio", &ok);
        const double max_spread_bps = parse_double(max_spread_raw, 8.0, 0.0, 1000.0, "--max-spread-bps", &ok);
        if (!ok)
            return 2;

        const QJsonObject cfg{{"enabled", true},
                              {"paper", paper},
                              {"symbols", QJsonArray::fromStringList(symbols)},
                              {"sources", QJsonArray::fromStringList(sources)},
                              {"paper_amounts_usd", double_list_json(paper_amounts)},
                              {"cadence_ms", cadence_ms},
                              {"venue", fee_venue},
                              {"liquidity", liquidity_mode},
                              {"fee_bps", fee_bps},
                              {"slippage_bps", slippage_bps},
                              {"safety_bps", safety_bps},
                              {"minimum_profit_bps", min_profit_bps},
                              {"min_net_bps", min_profit_bps},
                              {"min_confidence", min_confidence},
                              {"capture_ratio", capture_ratio},
                              {"max_age_ms", max_age_ms},
                              {"max_spread_bps", max_spread_bps},
                              {"min_live_sources", min_live_sources},
                              {"updated_at", now_utc()}};
        QString error;
        if (!write_json_object_file(daemon_scalp_config_path(profile), cfg, &error)) {
            std::fprintf(stderr, "scalp config write failed: %s\n", qUtf8Printable(error));
            return 7;
        }

        const QJsonObject daemon = daemon_status_object(profile);
        const QJsonObject out{{"profile", profile},
                              {"config", cfg},
                              {"daemon", daemon},
                              {"state", read_json_object_file(daemon_scalp_state_path(profile))}};
        if (json) {
            std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        } else {
            std::printf("scalp engine  enabled paper cadence=%dms symbols=%s\n",
                        cadence_ms, qUtf8Printable(symbols.join(',')));
            std::printf("amounts       $%s\n", qUtf8Printable([&]() {
                            QStringList parts;
                            for (const double amount : paper_amounts)
                                parts << QString::number(amount, 'f', amount == std::floor(amount) ? 0 : 2);
                            return parts.join(QStringLiteral(", $"));
                        }()));
            std::printf("sources       %s\n", qUtf8Printable(sources.join(',')));
            std::printf("cost model    %s %s entry=%.2fbps exit=%.2fbps slippage=%.2fbps safety=%.2fbps min_profit=%.2fbps min_confidence=%.0f%%\n",
                        qUtf8Printable(fee_venue),
                        qUtf8Printable(liquidity_mode),
                        fee_bps, fee_bps, slippage_bps, safety_bps, min_profit_bps, min_confidence * 100.0);
            for (const QString& symbol : symbols)
                std::printf("effective    %s -> %s\n",
                            qUtf8Printable(symbol),
                            qUtf8Printable(scalp_sources_for_symbol(sources, symbol).join(',')));
            std::printf("daemon        %s mode=%s\n",
                        daemon.value("running").toBool() ? "running" : "not running",
                        qUtf8Printable(daemon.value("mode").toString()));
            if (!daemon.value("running").toBool())
                std::printf("note          start the daemon with `daemon start` or `daemon install --start`\n");
        }
        return 0;
    }

    if (sub == "stop" || sub == "disable") {
        QJsonObject cfg = read_json_object_file(daemon_scalp_config_path(profile));
        cfg["enabled"] = false;
        cfg["updated_at"] = now_utc();
        QString error;
        if (!write_json_object_file(daemon_scalp_config_path(profile), cfg, &error)) {
            std::fprintf(stderr, "scalp config write failed: %s\n", qUtf8Printable(error));
            return 7;
        }
        if (json)
            std::printf("%s\n", QJsonDocument(QJsonObject{{"profile", profile}, {"config", cfg}})
                                    .toJson(QJsonDocument::Compact).constData());
        else
            std::printf("scalp engine  disabled\n");
        return 0;
    }

    if (sub == "status" || sub == "state") {
        const QJsonObject cfg = read_json_object_file(daemon_scalp_config_path(profile));
        const QJsonObject state = read_json_object_file(daemon_scalp_state_path(profile));
        const QJsonObject daemon = daemon_status_object(profile);
        const QJsonObject guard = automation::read_json_object(automation::live_guard_path(profile));
        if (json) {
            std::printf("%s\n", QJsonDocument(QJsonObject{{"profile", profile},
                                                          {"config", cfg},
                                                          {"state", state},
                                                          {"daemon", daemon},
                                                          {"live_guard", guard}})
                                    .toJson(QJsonDocument::Compact)
                                    .constData());
        } else {
            const QJsonObject effective_cfg = state.value("config").toObject(cfg);
            QStringList symbols;
            for (const auto& v : effective_cfg.value("symbols").toArray())
                symbols << v.toString();
            std::printf("scalp engine  %s daemon=%s mode=%s\n",
                        effective_cfg.value("enabled").toBool() ? "enabled" : "disabled",
                        daemon.value("running").toBool() ? "running" : "not running",
                        qUtf8Printable(daemon.value("mode").toString()));
            std::printf("live guard    %s\n", guard.value("enabled").toBool() ? "ARMED" : "off");
            std::printf("cadence       %dms paper=yes heartbeat=%s\n",
                        effective_cfg.value("cadence_ms").toInt(),
                        qUtf8Printable(state.value("heartbeat_at").toString("-")));
            std::printf("symbols       %s\n", qUtf8Printable(symbols.join(',')));
            QStringList amounts;
            for (const auto& v : effective_cfg.value("paper_amounts_usd").toArray())
                amounts << QString::number(v.toDouble(), 'f', v.toDouble() == std::floor(v.toDouble()) ? 0 : 2);
            if (!amounts.isEmpty())
                std::printf("amounts       $%s\n", qUtf8Printable(amounts.join(QStringLiteral(", $"))));
            std::printf("cost model    %s %s entry=%.2fbps exit=%.2fbps slippage=%.2fbps safety=%.2fbps min_profit=%.2fbps min_confidence=%.0f%%\n",
                        qUtf8Printable(effective_cfg.value("venue").toString("coinbase")),
                        qUtf8Printable(effective_cfg.value("liquidity").toString("maker")),
                        effective_cfg.value("fee_bps").toDouble(),
                        effective_cfg.value("fee_bps").toDouble(),
                        effective_cfg.value("slippage_bps").toDouble(),
                        effective_cfg.value("safety_bps").toDouble(),
                        effective_cfg.value("minimum_profit_bps").toDouble(
                            effective_cfg.value("min_net_bps").toDouble()),
                        normalize_confidence_gate(effective_cfg.value("min_confidence").toDouble()) * 100.0);
            const QJsonArray decisions = state.value("decisions").toArray();
            if (!decisions.isEmpty()) {
                std::printf("%-10s %-22s %-8s %-10s %-9s %-9s %-9s %-10s %s\n",
                            "SYMBOL", "VERDICT", "DIR", "PRICE", "REQ_BPS", "NET_BPS", "SURPLUS", "AGE_MS", "BLOCKERS");
                for (const auto& v : decisions) {
                    const QJsonObject d = v.toObject();
                    QStringList blockers;
                    for (const auto& b : d.value("blockers").toArray())
                        blockers << b.toString();
                    std::printf("%-10s %-22s %-8s %-10.2f %-9.2f %-9.2f %-9.2f %-10lld %s\n",
                                qUtf8Printable(d.value("symbol").toString()),
                                qUtf8Printable(d.value("verdict").toString().left(22)),
                                qUtf8Printable(d.value("direction").toString()),
                                d.value("reference_price").toDouble(),
                                d.value("required_edge_bps").toDouble(),
                                d.value("net_after_cost_bps").toDouble(),
                                d.value("edge_surplus_bps").toDouble(),
                                static_cast<long long>(d.value("freshest_age_ms").toInteger()),
                                qUtf8Printable(blockers.join(QStringLiteral("; ")).left(120)));
                }
            }
        }
        return 0;
    }

    if (sub == "tape" || sub == "ticks" || sub == "decisions" || sub == "journal") {
        QString limit_raw;
        QString symbol_raw;
        const bool use_decisions = sub == "decisions" || sub == "journal" ||
                                   args.removeAll(QStringLiteral("--decisions")) > 0;
        for (int i = 0; i < args.size(); ++i) {
            const QString flag = args.at(i);
            if (flag == QStringLiteral("--limit")) {
                if (!consume_value(args, i, flag, &limit_raw)) return 2;
            } else if (flag == QStringLiteral("--symbol")) {
                if (!consume_value(args, i, flag, &symbol_raw)) return 2;
            }
        }
        if (!args.isEmpty() && symbol_raw.isEmpty())
            symbol_raw = args.takeFirst();
        if (!args.isEmpty()) {
            std::fprintf(stderr, "unknown args: %s\n", qUtf8Printable(args.join(' ')));
            return 2;
        }
        bool ok = true;
        const int limit = limit_raw.isEmpty() ? 20 : limit_raw.toInt(&ok);
        if (!ok || limit < 1 || limit > 500) {
            std::fprintf(stderr, "--limit must be 1..500\n");
            return 2;
        }
        const QString symbol = symbol_raw.trimmed().isEmpty()
                                   ? QString()
                                   : services::crypto_latency::CryptoLatencyService::normalize_symbol(symbol_raw);
        const QString path = use_decisions ? daemon_scalp_decisions_path(profile) : daemon_scalp_ticks_path(profile);
        const QJsonArray rows = tail_jsonl_objects(path, limit, symbol);
        if (json) {
            std::printf("%s\n", QJsonDocument(QJsonObject{{"profile", profile},
                                                          {"kind", use_decisions ? "decisions" : "ticks"},
                                                          {"path", path},
                                                          {"rows", rows}})
                                    .toJson(QJsonDocument::Compact)
                                    .constData());
        } else {
            std::printf("%s\n", use_decisions ? "SCALP PAPER DECISIONS" : "SCALP TICK TAPE");
            for (const auto& v : rows) {
                const QJsonObject o = v.toObject();
                if (use_decisions) {
                    std::printf("%s %-8s %-22s price=%.2f rt=%.2fbps req=%.2fbps surplus=%.2fbps age=%lldms\n",
                                qUtf8Printable(o.value("ts").toString()),
                                qUtf8Printable(o.value("symbol").toString()),
                                qUtf8Printable(o.value("verdict").toString().left(22)),
                                o.value("reference_price").toDouble(),
                                o.value("round_trip_cost_bps").toDouble(),
                                o.value("required_edge_bps").toDouble(),
                                o.value("edge_surplus_bps").toDouble(),
                                static_cast<long long>(o.value("freshest_age_ms").toInteger()));
                } else {
                    std::printf("%s %-8s %-12s price=%.2f bid=%.2f ask=%.2f seq=%s\n",
                                qUtf8Printable(o.value("received_ts_ms").toString()),
                                qUtf8Printable(o.value("symbol").toString()),
                                qUtf8Printable(o.value("source").toString()),
                                o.value("price").toDouble(),
                                o.value("best_bid").toDouble(),
                                o.value("best_ask").toDouble(),
                                qUtf8Printable(o.value("sequence").toString()));
                }
            }
        }
        return 0;
    }

    std::fprintf(stderr,
                 "usage: daemon scalp start [BTC-USD] [--symbols S] [--cadence-ms N] [--sources S]\n"
                 "       [--amounts 25,50] [--venue coinbase] [--liquidity maker|taker]\n"
                 "       [--min-profit-bps N] [--min-confidence 80] [--paper]\n"
                 "       daemon scalp venues [--maker|--taker] [--min-profit-bps N]\n"
                 "       daemon scalp explore [--style scalp|spot] [--amount 50] [--confidence 80] [--target-bps 25,50,100]\n"
                 "       daemon scalp status\n"
                 "       daemon scalp tape [SYMBOL] [--limit N] [--decisions]\n"
                 "       daemon scalp stop\n");
    return 2;
}

QJsonObject make_automation_247_job(const QString& symbol,
                                    const QStringList& command,
                                    int every_sec,
                                    int timeout_sec,
                                    bool live,
                                    const QJsonObject& existing = {}) {
    QJsonObject job = existing.isEmpty()
                          ? make_job(QStringLiteral("command"),
                                     QStringLiteral("24/7 %1 %2 executor")
                                         .arg(live ? QStringLiteral("live") : QStringLiteral("paper"), symbol),
                                     QJsonObject{{"command", strings_to_json_array(command)}},
                                     every_sec,
                                     timeout_sec,
                                     true)
                          : existing;
    const QString now = now_utc();
    job["name"] = QStringLiteral("24/7 %1 %2 executor")
                      .arg(live ? QStringLiteral("live") : QStringLiteral("paper"), symbol);
    job["kind"] = QStringLiteral("command");
    job["enabled"] = true;
    job["schedule"] = QStringLiteral("interval");
    job["interval_sec"] = every_sec;
    job["timeout_sec"] = timeout_sec;
    job["spec"] = QJsonObject{{"command", strings_to_json_array(command)}};
    job["command"] = strings_to_json_array(command);
    job["managed_by"] = QStringLiteral("automation-24-7");
    job["automation_symbol"] = symbol;
    job["automation_mode"] = live ? QStringLiteral("live") : QStringLiteral("paper");
    job["description"] = live
                             ? QStringLiteral("Attempts one armed live post-only order from the latest approved local candidate.")
                             : QStringLiteral("Records dry-run post-only orders from the latest approved local candidate.");
    job["updated_at"] = now;
    if (job.value(QStringLiteral("created_at")).toString().isEmpty())
        job["created_at"] = now;
    if (job.value(QStringLiteral("id")).toString().isEmpty())
        job["id"] = QStringLiteral("job_%1").arg(QUuid::createUuid().toString(QUuid::Id128).left(12));
    if (job.value(QStringLiteral("next_run_at")).toString().isEmpty())
        job["next_run_at"] = now;
    job["running"] = false;
    job["current_run_id"] = QString();
    return job;
}

QJsonArray automation_247_jobs_for_profile(const QString& profile) {
    QJsonArray rows;
    const QJsonArray jobs = load_jobs_doc(profile).value(QStringLiteral("jobs")).toArray();
    for (const QJsonValue& v : jobs) {
        const QJsonObject job = v.toObject();
        if (job.value(QStringLiteral("managed_by")).toString() == QStringLiteral("automation-24-7"))
            rows.append(job);
    }
    return rows;
}

int disable_automation_247_jobs(const QString& profile, int* disabled_out) {
    QJsonObject doc = load_jobs_doc(profile);
    QJsonArray jobs = doc.value(QStringLiteral("jobs")).toArray();
    int disabled = 0;
    for (int i = 0; i < jobs.size(); ++i) {
        QJsonObject job = jobs.at(i).toObject();
        if (job.value(QStringLiteral("managed_by")).toString() != QStringLiteral("automation-24-7"))
            continue;
        if (job.value(QStringLiteral("enabled")).toBool()) {
            job["enabled"] = false;
            ++disabled;
        }
        job["running"] = false;
        job["current_run_id"] = QString();
        job["updated_at"] = now_utc();
        jobs.replace(i, job);
    }
    if (disabled_out)
        *disabled_out = disabled;
    return jobs_save_update(profile, jobs);
}

// --- Strategy Sandbox daemon jobs (Task 7) ----------------------------------
// Managed proof-stack jobs, tagged managed_by:"strategy-sandbox" and matched for
// idempotent re-install by the "sandbox_job" key (mirrors how
// make_automation_247_job matches on automation_symbol):
//   - producers keep ticks and decision-journal rows warm.
//   - tick advances paper fills/exits.
//   - score-now refreshes the strategy leaderboard.
QJsonObject make_sandbox_job(const QString& sandbox_job_key,
                             const QString& name,
                             const QString& description,
                             const QStringList& command,
                             int every_sec,
                             int timeout_sec,
                             const QJsonObject& existing = {}) {
    QJsonObject job = existing.isEmpty()
                          ? make_job(QStringLiteral("command"), name,
                                     QJsonObject{{"command", strings_to_json_array(command)}},
                                     every_sec, timeout_sec, true)
                          : existing;
    const QString now = now_utc();
    job["name"] = name;
    job["kind"] = QStringLiteral("command");
    job["enabled"] = true;
    job["schedule"] = QStringLiteral("interval");
    job["interval_sec"] = every_sec;
    job["timeout_sec"] = timeout_sec;
    job["spec"] = QJsonObject{{"command", strings_to_json_array(command)}};
    job["command"] = strings_to_json_array(command);
    job["managed_by"] = QStringLiteral("strategy-sandbox");
    job["sandbox_job"] = sandbox_job_key;
    job["description"] = description;
    job["updated_at"] = now;
    if (job.value(QStringLiteral("created_at")).toString().isEmpty())
        job["created_at"] = now;
    if (job.value(QStringLiteral("id")).toString().isEmpty())
        job["id"] = QStringLiteral("job_%1").arg(QUuid::createUuid().toString(QUuid::Id128).left(12));
    if (job.value(QStringLiteral("next_run_at")).toString().isEmpty())
        job["next_run_at"] = now;
    job["running"] = false;
    job["current_run_id"] = QString();
    return job;
}

struct SandboxJobSpec {
    QString key;
    QString name;
    QString description;
    QStringList command;
    int every_sec;
    int timeout_sec;
};

QVector<SandboxJobSpec> sandbox_job_specs() {
    return {{QStringLiteral("btc-tick-collector"), QStringLiteral("Strategy sandbox BTC ticks"),
             QStringLiteral("Keep BTC tick tail warm for sandbox fills, exits, and Chronos context."),
             {QStringLiteral("edge"), QStringLiteral("collect"), QStringLiteral("BTC"),
              QStringLiteral("--sources"), QStringLiteral("coinbase,kraken,binanceperp,bitcointicker"),
              QStringLiteral("--stream-ms"), QStringLiteral("8000"),
              QStringLiteral("--timeout-ms"), QStringLiteral("5000")},
             20, 30},
            // Real-horizon reshape (task 3): the season-1 spot books trade
            // 1h/4h/1d swings (see SandboxRegistry.cpp's spot_1h/4h/1d
            // seeds), not a 60s scalp-adjacent horizon -- crypto-universe
            // --horizon-sec 60 never matched any of them (min_horizon_sec
            // 3600 always rejected it). `edge spot-swing-gate` forwards to
            // the same crypto-universe producer (source='edge crypto-
            // recommend', CommandDispatch.cpp:14491 -> :13611) with a cost-
            // aware fee/target profile computed from --horizon, so one job
            // per spot book horizon now feeds all three real books.
            {QStringLiteral("spot-swing-1h-decisions"), QStringLiteral("Strategy sandbox spot swing 1h decisions"),
             QStringLiteral("Produce Coinbase spot swing recommendation rows (1h horizon) for the spot_1h strategy book."),
             {QStringLiteral("edge"), QStringLiteral("spot-swing-gate"),
              QStringLiteral("--symbols"), QStringLiteral("BTC,ETH,SOL"),
              QStringLiteral("--venue"), QStringLiteral("coinbase"),
              QStringLiteral("--horizon"), QStringLiteral("1h"),
              QStringLiteral("--duration-ms"), QStringLiteral("1500")},
             300, 180},
            {QStringLiteral("spot-swing-4h-decisions"), QStringLiteral("Strategy sandbox spot swing 4h decisions"),
             QStringLiteral("Produce Coinbase spot swing recommendation rows (4h horizon) for the spot_4h strategy book."),
             {QStringLiteral("edge"), QStringLiteral("spot-swing-gate"),
              QStringLiteral("--symbols"), QStringLiteral("BTC,ETH,SOL"),
              QStringLiteral("--venue"), QStringLiteral("coinbase"),
              QStringLiteral("--horizon"), QStringLiteral("4h"),
              QStringLiteral("--duration-ms"), QStringLiteral("1500")},
             900, 180},
            {QStringLiteral("spot-swing-1d-decisions"), QStringLiteral("Strategy sandbox spot swing 1d decisions"),
             QStringLiteral("Produce Coinbase spot swing recommendation rows (1d horizon) for the spot_1d strategy book."),
             {QStringLiteral("edge"), QStringLiteral("spot-swing-gate"),
              QStringLiteral("--symbols"), QStringLiteral("BTC,ETH,SOL"),
              QStringLiteral("--venue"), QStringLiteral("coinbase"),
              QStringLiteral("--horizon"), QStringLiteral("1d"),
              QStringLiteral("--duration-ms"), QStringLiteral("1500")},
             3600, 180},
            // Real-horizon reshape (task 3): the btc5m book is retired (no
            // venue / no edge, SandboxRegistry.cpp no longer seeds it), so its
            // producer job is removed too -- nothing consumes source='edge
            // journal-evaluate-btc5m-live' rows anymore, and leaving the 60s
            // job in place would burn compute and grow edge_decision_journal
            // with orphaned rows forever.
            {QStringLiteral("coinbase-long-short-decisions"), QStringLiteral("Strategy sandbox Coinbase long/short"),
             QStringLiteral("Produce guarded Coinbase long/short paper decision rows for the long_short book."),
             {QStringLiteral("edge"), QStringLiteral("long-short-strategy"), QStringLiteral("BTC-USD"),
              QStringLiteral("--venue"), QStringLiteral("coinbase_perps"),
              QStringLiteral("--horizon-sec"), QStringLiteral("300"),
              QStringLiteral("--duration-ms"), QStringLiteral("8000"),
              QStringLiteral("--leverage"), QStringLiteral("3"),
              QStringLiteral("--margin-usd"), QStringLiteral("10"),
              QStringLiteral("--fee-bps"), QStringLiteral("5"),
              QStringLiteral("--slippage-bps"), QStringLiteral("2"),
              QStringLiteral("--min-edge-bps"), QStringLiteral("50"),
              QStringLiteral("--target-bps"), QStringLiteral("100"),
              QStringLiteral("--stop-bps"), QStringLiteral("45")},
             300, 60},
            {QStringLiteral("kalshi-auto-recovery-watchdog"), QStringLiteral("Kalshi event-engine recovery watchdog"),
             QStringLiteral("Slow recovery pass for BTC 15-minute and hourly evidence if the persistent Kalshi WebSocket is disconnected; normal decisions are event-driven."),
             {QStringLiteral("kalshi"), QStringLiteral("auto"), QStringLiteral("run"),
              QStringLiteral("--category"), QStringLiteral("Crypto#BTC@live"),
              QStringLiteral("--max-positions"), QStringLiteral("5"),
              QStringLiteral("--max-cost"), QStringLiteral("125"),
              QStringLiteral("--min-edge"), QStringLiteral("0.03")},
             60, 60},
            {QStringLiteral("kalshi-auto-daily-plan"), QStringLiteral("Kalshi daily auto cockpit plan"),
             QStringLiteral("Build the coherent BTC daily surface and journal early, middle, and late paper evidence separately."),
             {QStringLiteral("kalshi"), QStringLiteral("auto"), QStringLiteral("run"),
              QStringLiteral("--category"), QStringLiteral("Crypto#BTC@daily"),
              QStringLiteral("--max-positions"), QStringLiteral("5"),
              QStringLiteral("--max-cost"), QStringLiteral("125"),
              QStringLiteral("--min-edge"), QStringLiteral("0.03")},
             300, 60},
            {QStringLiteral("kalshi-live-account-reconcile"), QStringLiteral("Kalshi live account evidence"),
             QStringLiteral("Read authenticated Kalshi positions and fills into the local evidence ledger; never submits an order."),
             {QStringLiteral("kalshi"), QStringLiteral("auto"), QStringLiteral("positions")},
             30, 60},
            {QStringLiteral("btc-news-hourly-pulse"), QStringLiteral("Bitcoin hourly narrative pulse"),
             QStringLiteral("Scan local RSS sources, deduplicate Bitcoin stories, create one-paragraph summaries, and record advisory UP/DOWN/NEUTRAL context."),
             {QStringLiteral("news"), QStringLiteral("bitcoin-pulse"),
              QStringLiteral("--range"), QStringLiteral("24H"),
              QStringLiteral("--limit"), QStringLiteral("60"),
              QStringLiteral("--force")},
             3600, 180},
            {QStringLiteral("btc-news-hourly-score"), QStringLiteral("Bitcoin narrative outcome scorer"),
             QStringLiteral("Resolve matured 1h/6h/24h narrative calls against locally recorded BTC ticks."),
             {QStringLiteral("news"), QStringLiteral("bitcoin-score"),
              QStringLiteral("--limit"), QStringLiteral("500")},
             3600, 60},
            {QStringLiteral("btc-evidence-hourly-intelligence"), QStringLiteral("Bitcoin hourly evidence engine"),
             QStringLiteral("Combine narrative memory, market reaction, regime, cross-market confirmation, Kalshi lag, replay, abstention, and time-conditioned calibration."),
             {QStringLiteral("news"), QStringLiteral("bitcoin-intelligence"),
              QStringLiteral("--limit"), QStringLiteral("1000")},
             3600, 90},
            {QStringLiteral("btc-evidence-hourly-score"), QStringLiteral("Bitcoin evidence outcome scorer"),
             QStringLiteral("Resolve matured evidence snapshots and update calibration, adaptive weights, and abstention value without lookahead."),
             {QStringLiteral("news"), QStringLiteral("bitcoin-intelligence-score"),
              QStringLiteral("--limit"), QStringLiteral("500")},
             3600, 60},
            {QStringLiteral("kalshi-decision-settlements"), QStringLiteral("Strategy sandbox Kalshi settlement scorer"),
             QStringLiteral("Resolve matured Kalshi paper decisions from the official final YES/NO market result."),
             {QStringLiteral("edge"), QStringLiteral("resolve-kalshi-decisions"),
              QStringLiteral("--limit"), QStringLiteral("150"),
              QStringLiteral("--timeout-ms"), QStringLiteral("12000")},
             60, 45},
            // Real-horizon reshape (task 3): the chronos2_5m book is retired
            // (no venue / no edge, SandboxRegistry.cpp's seed no longer
            // registers it) -- its forecast producer job is removed too, so
            // no installed job spends a Chronos-2 call on a 5m horizon.
            {QStringLiteral("chronos2-btc-15m"), QStringLiteral("Strategy sandbox Chronos BTC 15m"),
             QStringLiteral("Run and publish the current Chronos-2 BTC forecast while journaling price-forecast candidates for sandbox proofing."),
             {QStringLiteral("edge"), QStringLiteral("chronos2"), QStringLiteral("forecast"),
              QStringLiteral("BTC-USD"),
              QStringLiteral("--horizon"), QStringLiteral("15m"),
              QStringLiteral("--publish"),
              QStringLiteral("--journal"),
              QStringLiteral("--min-journal-edge-bps"), QStringLiteral("15")},
             900, 300},
            {QStringLiteral("chronos2-btc-1h"), QStringLiteral("Strategy sandbox Chronos BTC 1h"),
             QStringLiteral("Run and publish the current Chronos-2 BTC hourly forecast while journaling price-forecast candidates."),
             {QStringLiteral("edge"), QStringLiteral("chronos2"), QStringLiteral("forecast"),
              QStringLiteral("BTC-USD"),
              QStringLiteral("--horizon"), QStringLiteral("1h"),
              QStringLiteral("--publish"),
              QStringLiteral("--journal"),
              QStringLiteral("--min-journal-edge-bps"), QStringLiteral("35")},
             3600, 420},
            {QStringLiteral("chronos2-btc-1d"), QStringLiteral("Strategy sandbox Chronos BTC 1d"),
             QStringLiteral("Run and publish the current Chronos-2 BTC daily forecast while journaling price-forecast candidates."),
             {QStringLiteral("edge"), QStringLiteral("chronos2"), QStringLiteral("forecast"),
              QStringLiteral("BTC-USD"),
              QStringLiteral("--horizon"), QStringLiteral("1d"),
              QStringLiteral("--publish"),
              QStringLiteral("--journal"),
              QStringLiteral("--min-journal-edge-bps"), QStringLiteral("75")},
             86400, 600},
            {QStringLiteral("local-model-btc-publish"), QStringLiteral("Local BTC model output publisher"),
             QStringLiteral("Publish fresh timestamped local BTC forecasts for every supported horizon; advisory evidence only and never submits an order."),
             {QStringLiteral("edge"), QStringLiteral("publish-horizons"),
              QStringLiteral("--symbol"), QStringLiteral("BTC"),
              QStringLiteral("--market-prob"), QStringLiteral("0.50"),
              QStringLiteral("--spread"), QStringLiteral("0.03"),
              QStringLiteral("--min-samples"), QStringLiteral("30")},
             60, 30},
            {QStringLiteral("chronos2-equity-spy-1d"), QStringLiteral("Strategy sandbox Chronos SPY 1d"),
             QStringLiteral("Run Chronos-2 SPY daily forecast and journal equity price-forecast candidates."),
             {QStringLiteral("edge"), QStringLiteral("chronos2"), QStringLiteral("equity"),
              QStringLiteral("SPY"),
              QStringLiteral("--horizon"), QStringLiteral("1d"),
              QStringLiteral("--period"), QStringLiteral("2y"),
              QStringLiteral("--journal"),
              QStringLiteral("--min-journal-edge-bps"), QStringLiteral("50")},
             86400, 600},
            {QStringLiteral("tick"), QStringLiteral("Strategy sandbox tick"),
             QStringLiteral("Fills/expires hypothetical sandbox positions off the latest tick tail."),
             {QStringLiteral("sandbox"), QStringLiteral("tick")}, 45, 45},
            {QStringLiteral("score-now"), QStringLiteral("Strategy sandbox score"),
             QStringLiteral("Scores resolved sandbox outcomes and refreshes the strategy leaderboard."),
             {QStringLiteral("sandbox"), QStringLiteral("score-now")}, 21600, 120}};
}

int install_sandbox_jobs(const QString& profile, QJsonArray* managed_out, int* retired_out) {
    QJsonObject doc = load_jobs_doc(profile);
    QJsonArray jobs = doc.value(QStringLiteral("jobs")).toArray();
    QJsonArray managed;
    QStringList spec_keys;
    for (const SandboxJobSpec& spec : sandbox_job_specs()) {
        spec_keys << spec.key;
        int idx = -1;
        for (int i = 0; i < jobs.size(); ++i) {
            const QJsonObject job = jobs.at(i).toObject();
            if (job.value(QStringLiteral("managed_by")).toString() == QStringLiteral("strategy-sandbox") &&
                job.value(QStringLiteral("sandbox_job")).toString() == spec.key) {
                idx = i;
                break;
            }
        }
        const QJsonObject job = make_sandbox_job(spec.key, spec.name, spec.description, spec.command,
                                                 spec.every_sec, spec.timeout_sec,
                                                 idx >= 0 ? jobs.at(idx).toObject() : QJsonObject{});
        if (idx >= 0)
            jobs.replace(idx, job);
        else
            jobs.append(job);
        managed.append(job);
    }
    // Reconcile: a managed job whose sandbox_job key has dropped OUT of the
    // current spec (e.g. a producer retired by a reshape) must be disabled
    // rather than left running forever with no consumer.
    int retired = 0;
    for (int i = 0; i < jobs.size(); ++i) {
        QJsonObject job = jobs.at(i).toObject();
        if (job.value(QStringLiteral("managed_by")).toString() != QStringLiteral("strategy-sandbox"))
            continue;
        if (spec_keys.contains(job.value(QStringLiteral("sandbox_job")).toString()))
            continue;
        if (!job.value(QStringLiteral("enabled")).toBool())
            continue;
        job[QStringLiteral("enabled")] = false;
        job[QStringLiteral("running")] = false;
        job[QStringLiteral("current_run_id")] = QString();
        job[QStringLiteral("updated_at")] = now_utc();
        jobs.replace(i, job);
        ++retired;
    }
    if (managed_out)
        *managed_out = managed;
    if (retired_out)
        *retired_out = retired;
    return jobs_save_update(profile, jobs);
}

int disable_sandbox_jobs(const QString& profile, int* disabled_out) {
    QJsonObject doc = load_jobs_doc(profile);
    QJsonArray jobs = doc.value(QStringLiteral("jobs")).toArray();
    int disabled = 0;
    for (int i = 0; i < jobs.size(); ++i) {
        QJsonObject job = jobs.at(i).toObject();
        if (job.value(QStringLiteral("managed_by")).toString() != QStringLiteral("strategy-sandbox"))
            continue;
        if (job.value(QStringLiteral("enabled")).toBool()) {
            job["enabled"] = false;
            ++disabled;
        }
        job["running"] = false;
        job["current_run_id"] = QString();
        job["updated_at"] = now_utc();
        jobs.replace(i, job);
    }
    if (disabled_out)
        *disabled_out = disabled;
    return jobs_save_update(profile, jobs);
}

int daemon_sandbox_jobs_command(const QString& profile, bool json, QStringList args) {
    const QString sub = args.isEmpty() ? QString() : args.takeFirst().trimmed().toLower();
    if (sub == QStringLiteral("install-jobs")) {
        QJsonArray managed;
        int retired = 0;
        const int rc = install_sandbox_jobs(profile, &managed, &retired);
        if (rc != 0)
            return rc;
        if (json) {
            std::printf("%s\n", QJsonDocument(QJsonObject{{"profile", profile}, {"jobs", managed},
                                                           {"retired", retired}})
                                    .toJson(QJsonDocument::Compact).constData());
        } else {
            std::printf("installed sandbox daemon jobs:\n");
            for (const QJsonValue& v : managed) {
                const QJsonObject job = v.toObject();
                std::printf("  %-12s every %6ds timeout %4ds  %s\n",
                            qUtf8Printable(job.value("sandbox_job").toString()),
                            job.value("interval_sec").toInt(), job.value("timeout_sec").toInt(),
                            qUtf8Printable(job.value("id").toString()));
            }
            if (retired > 0)
                std::printf("retired %d stale sandbox job%s\n", retired, retired == 1 ? "" : "s");
        }
        return 0;
    }
    if (sub == QStringLiteral("remove-jobs")) {
        int disabled = 0;
        const int rc = disable_sandbox_jobs(profile, &disabled);
        if (rc != 0)
            return rc;
        if (json) {
            std::printf("%s\n", QJsonDocument(QJsonObject{{"profile", profile}, {"disabled", disabled}})
                                    .toJson(QJsonDocument::Compact).constData());
        } else {
            std::printf("disabled %d sandbox daemon job%s\n", disabled, disabled == 1 ? "" : "s");
        }
        return 0;
    }
    std::fprintf(stderr, "usage: daemon sandbox install-jobs|remove-jobs\n");
    return 2;
}

int daemon_automation_stop_command(const QString& profile, bool json) {
    QJsonObject cfg = read_json_object_file(daemon_scalp_config_path(profile));
    cfg["enabled"] = false;
    cfg["updated_at"] = now_utc();
    QString error;
    if (!write_json_object_file(daemon_scalp_config_path(profile), cfg, &error)) {
        std::fprintf(stderr, "scalp config write failed: %s\n", qUtf8Printable(error));
        return 7;
    }
    int disabled_jobs = 0;
    const int jobs_rc = disable_automation_247_jobs(profile, &disabled_jobs);
    if (jobs_rc != 0)
        return jobs_rc;

    QJsonObject guard = automation::read_json_object(automation::live_guard_path(profile));
    const bool was_armed = guard.value(QStringLiteral("enabled")).toBool();
    if (was_armed) {
        guard[QStringLiteral("enabled")] = false;
        guard[QStringLiteral("disarmed_at")] = now_utc();
        guard[QStringLiteral("disarmed_by")] = QStringLiteral("automation stop");
        QString guard_error;
        if (!automation::write_json_object(automation::live_guard_path(profile), guard, &guard_error)) {
            std::fprintf(stderr, "guard disarm failed: %s\n", qUtf8Printable(guard_error));
            return 7;
        }
    }

    const QJsonObject out{{"profile", profile},
                          {"config", cfg},
                          {"forever_jobs_disabled", disabled_jobs},
                          {"jobs", automation_247_jobs_for_profile(profile)},
                          {"live_guard_disarmed", was_armed},
                          {"live_guard", guard}};
    if (json) {
        std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
    } else {
        std::printf("automation    stopped\n");
        std::printf("scanner       disabled\n");
        std::printf("forever jobs  disabled=%d\n", disabled_jobs);
        if (was_armed)
            std::printf("live guard    disarmed\n");
    }
    return 0;
}

int daemon_automation_247_command(const QString& profile, bool json, QStringList args) {
    QString preset = QStringLiteral("btc");
    if (!args.isEmpty() && !args.first().startsWith('-'))
        preset = args.takeFirst().trimmed().toLower();

    QString symbols_raw;
    QString amount_raw = QStringLiteral("50");
    QString cadence_raw = QStringLiteral("1000");
    QString every_raw = QStringLiteral("15");
    QString timeout_raw = QStringLiteral("20");
    QString venue_raw = QStringLiteral("coinbase_advanced");
    QString min_profit_raw = QStringLiteral("10");
    QString min_confidence_raw = QStringLiteral("80");
    QString max_spread_raw = QStringLiteral("8");
    QString min_sources_raw = QStringLiteral("2");
    QString max_age_raw = QStringLiteral("15");
    QString entry_offset_raw = QStringLiteral("1");
    QString min_profit_cents_raw;
    bool live = false;
    bool yes = false;
    bool understand = false;
    bool fast = false;

    for (int i = 0; i < args.size(); ++i) {
        const QString flag = args.at(i);
        if (flag == QStringLiteral("--symbols")) {
            if (!consume_value(args, i, flag, &symbols_raw)) return 2;
        } else if (flag == QStringLiteral("--amount") || flag == QStringLiteral("--amount-usd") ||
                   flag == QStringLiteral("--amounts")) {
            if (!consume_value(args, i, flag, &amount_raw)) return 2;
        } else if (flag == QStringLiteral("--cadence-ms") || flag == QStringLiteral("--interval-ms")) {
            if (!consume_value(args, i, flag, &cadence_raw)) return 2;
        } else if (flag == QStringLiteral("--every-sec") || flag == QStringLiteral("--executor-every-sec")) {
            if (!consume_value(args, i, flag, &every_raw)) return 2;
        } else if (flag == QStringLiteral("--timeout-sec")) {
            if (!consume_value(args, i, flag, &timeout_raw)) return 2;
        } else if (flag == QStringLiteral("--venue")) {
            if (!consume_value(args, i, flag, &venue_raw)) return 2;
        } else if (flag == QStringLiteral("--min-profit-bps")) {
            if (!consume_value(args, i, flag, &min_profit_raw)) return 2;
        } else if (flag == QStringLiteral("--min-profit-cents")) {
            if (!consume_value(args, i, flag, &min_profit_cents_raw)) return 2;
        } else if (flag == QStringLiteral("--min-confidence") || flag == QStringLiteral("--confidence")) {
            if (!consume_value(args, i, flag, &min_confidence_raw)) return 2;
        } else if (flag == QStringLiteral("--max-spread-bps")) {
            if (!consume_value(args, i, flag, &max_spread_raw)) return 2;
        } else if (flag == QStringLiteral("--min-live-sources")) {
            if (!consume_value(args, i, flag, &min_sources_raw)) return 2;
        } else if (flag == QStringLiteral("--max-age-sec")) {
            if (!consume_value(args, i, flag, &max_age_raw)) return 2;
        } else if (flag == QStringLiteral("--entry-offset-bps")) {
            if (!consume_value(args, i, flag, &entry_offset_raw)) return 2;
        } else if (flag == QStringLiteral("--fast")) {
            args.removeAt(i--);
            fast = true;
        } else if (flag == QStringLiteral("--safe") || flag == QStringLiteral("--paper")) {
            args.removeAt(i--);
            live = false;
        } else if (flag == QStringLiteral("--forever") || flag == QStringLiteral("--perpetuum-mobile")) {
            args.removeAt(i--);
        } else if (flag == QStringLiteral("--live")) {
            args.removeAt(i--);
            live = true;
        } else if (flag == QStringLiteral("--yes") || flag == QStringLiteral("-y")) {
            args.removeAt(i--);
            yes = true;
        } else if (flag == QStringLiteral("--i-understand-live-risk")) {
            args.removeAt(i--);
            understand = true;
        }
    }
    if (!args.isEmpty()) {
        std::fprintf(stderr, "unknown args: %s\n", qUtf8Printable(args.join(' ')));
        return 2;
    }
    if (live && (!yes || !understand)) {
        std::fprintf(stderr,
                     "24/7 live executor setup requires --yes --i-understand-live-risk. "
                     "Paper mode does not require this.\n");
        return 2;
    }

    if (symbols_raw.trimmed().isEmpty()) {
        if (preset == QStringLiteral("btc") || preset == QStringLiteral("bitcoin"))
            symbols_raw = QStringLiteral("BTC-USD");
        else
            symbols_raw = QStringLiteral("BTC-USD,ETH-USD,SOL-USD");
    }

    QStringList symbols = split_csv(symbols_raw);
    for (QString& symbol : symbols)
        symbol = services::crypto_latency::CryptoLatencyService::normalize_symbol(symbol);
    symbols.removeDuplicates();
    if (symbols.isEmpty()) {
        std::fprintf(stderr, "24/7 automation needs at least one symbol\n");
        return 2;
    }

    QVector<double> paper_amounts;
    for (const QString& raw : split_csv(amount_raw)) {
        bool amount_ok = false;
        const double amount = raw.toDouble(&amount_ok);
        if (!amount_ok || amount <= 0.0 || amount > 1000000.0) {
            std::fprintf(stderr, "--amount values must be positive USD notionals\n");
            return 2;
        }
        paper_amounts << amount;
    }
    if (paper_amounts.isEmpty())
        paper_amounts = {50.0};
    std::sort(paper_amounts.begin(), paper_amounts.end());
    paper_amounts.erase(std::unique(paper_amounts.begin(), paper_amounts.end()), paper_amounts.end());

    auto parse_int = [](const QString& raw, int fallback, int min_v, int max_v, const char* label, bool* ok_out) {
        if (raw.trimmed().isEmpty())
            return fallback;
        bool ok = false;
        const int v = raw.toInt(&ok);
        if (!ok || v < min_v || v > max_v) {
            std::fprintf(stderr, "%s must be %d..%d\n", label, min_v, max_v);
            *ok_out = false;
            return fallback;
        }
        return v;
    };
    auto parse_double = [](const QString& raw,
                           double fallback,
                           double min_v,
                           double max_v,
                           const char* label,
                           bool* ok_out) {
        if (raw.trimmed().isEmpty())
            return fallback;
        bool ok = false;
        const double v = raw.toDouble(&ok);
        if (!ok || !std::isfinite(v) || v < min_v || v > max_v) {
            std::fprintf(stderr, "%s must be %.2f..%.2f\n", label, min_v, max_v);
            *ok_out = false;
            return fallback;
        }
        return v;
    };

    bool ok = true;
    const int cadence_ms = parse_int(cadence_raw, fast ? 250 : 1000, 50, 5000, "--cadence-ms", &ok);
    const int every_sec = parse_int(every_raw, 15, 1, 3600, "--every-sec", &ok);
    const int timeout_sec = parse_int(timeout_raw, 20, 1, 300, "--timeout-sec", &ok);
    const int max_age_sec = parse_int(max_age_raw, 15, 1, 300, "--max-age-sec", &ok);
    const int min_live_sources = parse_int(min_sources_raw, 2, 1, 10, "--min-live-sources", &ok);
    const double max_spread_bps = parse_double(max_spread_raw, 8.0, 0.0, 1000.0, "--max-spread-bps", &ok);
    const double entry_offset_bps = parse_double(entry_offset_raw, 1.0, 0.0, 1000.0, "--entry-offset-bps", &ok);
    double min_profit_bps = parse_double(min_profit_raw, 10.0, 0.0, 1000.0, "--min-profit-bps", &ok);
    const double min_confidence = normalize_confidence_gate(
        parse_double(min_confidence_raw, 80.0, 0.0, 100.0, "--min-confidence", &ok));
    const double min_profit_cents = parse_double(min_profit_cents_raw, 0.0, 0.0, 100000.0,
                                                 "--min-profit-cents", &ok);
    if (min_profit_cents > 0.0 && !paper_amounts.isEmpty()) {
        const double cents_bps = (min_profit_cents / 100.0) / paper_amounts.first() * 10000.0;
        min_profit_bps = std::max(min_profit_bps, cents_bps);
    }
    if (!ok)
        return 2;

    const QString fee_venue = scalp_fee_venue(venue_raw);
    const QString liquidity_mode = QStringLiteral("maker");
    const double fee_bps = scalp_default_fee_bps(fee_venue, liquidity_mode);
    const QStringList sources = services::crypto_latency::CryptoLatencyService::default_sources();
    const QJsonObject cfg{{"enabled", true},
                          {"paper", true},
                          {"symbols", QJsonArray::fromStringList(symbols)},
                          {"sources", QJsonArray::fromStringList(sources)},
                          {"paper_amounts_usd", double_list_json(paper_amounts)},
                          {"cadence_ms", cadence_ms},
                          {"venue", fee_venue},
                          {"liquidity", liquidity_mode},
                          {"fee_bps", fee_bps},
                          {"slippage_bps", 0.0},
                          {"safety_bps", 5.0},
                          {"minimum_profit_bps", min_profit_bps},
                          {"min_net_bps", min_profit_bps},
                          {"min_confidence", min_confidence},
                          {"capture_ratio", 0.35},
                          {"max_age_ms", 1000},
                          {"max_spread_bps", max_spread_bps},
                          {"min_live_sources", min_live_sources},
                          {"updated_at", now_utc()}};
    QString error;
    if (!write_json_object_file(daemon_scalp_config_path(profile), cfg, &error)) {
        std::fprintf(stderr, "scalp config write failed: %s\n", qUtf8Printable(error));
        return 7;
    }

    QJsonObject doc = load_jobs_doc(profile);
    QJsonArray jobs = doc.value(QStringLiteral("jobs")).toArray();
    QJsonArray managed;
    int created = 0;
    int updated = 0;
    for (const QString& symbol : symbols) {
        QStringList command{QStringLiteral("automation"), QStringLiteral("execute-next"),
                            QStringLiteral("--symbol"), symbol,
                            QStringLiteral("--max-age-sec"), QString::number(max_age_sec),
                            QStringLiteral("--entry-offset-bps"), QString::number(entry_offset_bps, 'f', 2)};
        if (live)
            command << QStringLiteral("--yes");
        else
            command << QStringLiteral("--dry-run");

        int idx = -1;
        for (int i = 0; i < jobs.size(); ++i) {
            const QJsonObject job = jobs.at(i).toObject();
            if (job.value(QStringLiteral("managed_by")).toString() == QStringLiteral("automation-24-7") &&
                job.value(QStringLiteral("automation_symbol")).toString().compare(symbol, Qt::CaseInsensitive) == 0) {
                idx = i;
                break;
            }
        }
        const QJsonObject job = make_automation_247_job(symbol, command, every_sec, timeout_sec, live,
                                                        idx >= 0 ? jobs.at(idx).toObject() : QJsonObject{});
        if (idx >= 0) {
            jobs.replace(idx, job);
            ++updated;
        } else {
            jobs.append(job);
            ++created;
        }
        managed.append(job);
    }

    const int rc = jobs_save_update(profile, jobs);
    append_job_log(profile, QStringLiteral("automation-24-7 configured live=%1 created=%2 updated=%3 rc=%4 symbols=%5")
                                .arg(live ? QStringLiteral("yes") : QStringLiteral("no"))
                                .arg(created)
                                .arg(updated)
                                .arg(rc)
                                .arg(symbols.join(',')));
    if (rc != 0)
        return rc;

    const QJsonObject daemon = daemon_status_object(profile);
    const QJsonObject out{{"profile", profile},
                          {"mode", live ? QStringLiteral("live") : QStringLiteral("paper")},
                          {"symbols", QJsonArray::fromStringList(symbols)},
                          {"config", cfg},
                          {"jobs_created", created},
                          {"jobs_updated", updated},
                          {"jobs", managed},
                          {"daemon", daemon},
                          {"live_requires_arm", true},
                          {"live_note", QStringLiteral("Live jobs only submit while automation arm-live and global trading gates are active.")}};
    if (json) {
        std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
    } else {
        QStringList amounts;
        for (const double amount : paper_amounts)
            amounts << QStringLiteral("$%1").arg(QString::number(amount, 'f', amount == std::floor(amount) ? 0 : 2));
        std::printf("24/7 automation configured (%s)\n", live ? "live executor gated" : "paper/dry-run");
        std::printf("loop          forever until `automation stop`\n");
        std::printf("symbols       %s\n", qUtf8Printable(symbols.join(',')));
        std::printf("notional      %s\n", qUtf8Printable(amounts.join(QStringLiteral(", "))));
        std::printf("scanner       cadence=%dms min_profit=%.2fbps min_confidence=%.0f%% max_spread=%.2fbps venue=%s maker\n",
                    cadence_ms, min_profit_bps, min_confidence * 100.0, max_spread_bps, qUtf8Printable(fee_venue));
        std::printf("executor      every=%ds timeout=%ds max_candidate_age=%ds entry_offset=%.2fbps\n",
                    every_sec, timeout_sec, max_age_sec, entry_offset_bps);
        std::printf("jobs          created=%d updated=%d\n", created, updated);
        std::printf("daemon        %s mode=%s\n",
                    daemon.value("running").toBool() ? "running" : "not running",
                    qUtf8Printable(daemon.value("mode").toString()));
        if (!daemon.value("running").toBool())
            std::printf("next          run `daemon start` or use Profile > Automation > START\n");
        if (live) {
            std::printf("live safety   executor jobs are installed, but submit only when `automation arm-live` "
                        "and the global trading gates are active\n");
        }
    }
    return 0;
}

int daemon_automation_command(const QString& profile, bool json, QStringList args) {
    const QString sub = args.isEmpty() ? QStringLiteral("guide") : args.takeFirst().trimmed().toLower();
    auto print_guide = []() {
        std::printf(
            "Simple automation:\n"
            "  automation forever [btc|major] [--amount 50]\n"
            "  automation perpetuum-mobile [btc|major] [--amount 50]\n"
            "  automation 24-7 [btc|major] [--amount 50] [--min-confidence 80] [--every-sec 15]\n"
            "  automation start [btc|major] [--amount 25|50] [--min-confidence 80]\n"
            "  automation status\n"
            "  automation recent [--limit N]\n"
            "  automation costs [--maker|--taker]\n"
            "  automation explore [--style scalp|spot] [--amount 50] [--confidence 80] [--target-bps 25,50,100]\n"
            "  automation arm-bot --venue coinbase --strategies scalp,spot,long-short --max-order-usd N --symbols BTC-USD --yes --i-understand-live-risk\n"
            "  automation live-status\n"
            "  automation execute-next [--mode scalp|spot|long-short] [--symbol BTC-USD] [--dry-run] [--yes]\n"
            "  automation disarm-live --yes\n"
            "  automation stop\n"
            "\n"
            "What it does:\n"
            "  Watches live crypto microstructure, estimates fee/spread/slippage cost,\n"
            "  writes paper decisions, and refuses weak trades. Live execution requires\n"
            "  a separate short-lived arm plus GUI security gates. Long/short is watch/journal only\n"
            "  until a protected leveraged execution router is added.\n"
            "\n"
            "Good first run:\n"
            "  automation forever btc --amount 50\n"
            "  daemon start\n");
    };

    if (sub == "guide" || sub == "help" || sub == "--help") {
        if (json) {
            std::printf("%s\n", QJsonDocument(QJsonObject{
                                    {"profile", profile},
                                     {"paper_only", true},
                                     {"commands", QJsonArray{
                                                     "automation forever [btc|major] [--amount 50]",
                                                     "automation perpetuum-mobile [btc|major] [--amount 50]",
                                                     "automation 24-7 [btc|major] [--amount 50] [--min-confidence 80]",
                                                     "automation start [btc|major] [--amount 25|50] [--min-confidence 80]",
                                                     "automation status",
                                                     "automation recent [--limit N]",
                                                     "automation costs",
                                                     "automation explore [--style scalp|spot] [--amount 50] [--confidence 80]",
                                                     "automation stop"}}})
                                    .toJson(QJsonDocument::Compact).constData());
        } else {
            print_guide();
        }
        return 0;
    }

    if (sub == "live" || sub == "arm-live" || sub == "real") {
        if (json) {
            std::printf("%s\n", QJsonDocument(QJsonObject{{"profile", profile},
                                                          {"live_trading", false},
                                                          {"blocked", true},
                                                          {"reason", "live automation requires a separate arm/confirm/kill-switch flow"}})
                                    .toJson(QJsonDocument::Compact).constData());
        } else {
            std::fprintf(stderr,
                         "Live automation is intentionally blocked in simple mode.\n"
                         "Use paper automation first; live trading needs an explicit arm/confirm/kill-switch flow.\n");
        }
        return 2;
    }

    if (sub == "24-7" || sub == "247" || sub == "always-on" || sub == "penny-bot" ||
        sub == "forever" || sub == "perpetuum-mobile" || sub == "perpetual")
        return daemon_automation_247_command(profile, json, args);

    if (sub == "status" || sub == "state")
        return daemon_scalp_command(profile, json, {QStringLiteral("status")});
    if (sub == "stop" || sub == "disable" || sub == "off")
        return daemon_automation_stop_command(profile, json);
    if (sub == "recent" || sub == "decisions" || sub == "history") {
        args.prepend(QStringLiteral("decisions"));
        return daemon_scalp_command(profile, json, args);
    }
    if (sub == "tape" || sub == "ticks") {
        args.prepend(QStringLiteral("tape"));
        return daemon_scalp_command(profile, json, args);
    }
    if (sub == "costs" || sub == "venues" || sub == "fees") {
        args.prepend(QStringLiteral("venues"));
        return daemon_scalp_command(profile, json, args);
    }
    if (sub == "explore" || sub == "viability" || sub == "micro" || sub == "can-scalp") {
        args.prepend(QStringLiteral("explore"));
        return daemon_scalp_command(profile, json, args);
    }

    if (sub == "start" || sub == "paper" || sub == "on" || sub == "btc-dip" ||
        sub == "crypto-dip" || sub == "major") {
        QString preset = (sub == "btc-dip") ? QStringLiteral("btc")
                         : (sub == "crypto-dip" || sub == "major") ? QStringLiteral("major")
                                                                    : QStringLiteral("major");
        if (!args.isEmpty() && !args.first().startsWith('-')) {
            preset = args.takeFirst().trimmed().toLower();
        }

        QString symbols_raw;
        QString amount_raw;
        QString cadence_raw;
        QString venue_raw = QStringLiteral("coinbase_advanced");
        QString min_profit_raw = QStringLiteral("10");
        QString min_confidence_raw;
        QString max_spread_raw = QStringLiteral("8");
        QString min_sources_raw = QStringLiteral("2");
        bool fast = false;
        for (int i = 0; i < args.size(); ++i) {
            const QString flag = args.at(i);
            if (flag == QStringLiteral("--symbols")) {
                if (!consume_value(args, i, flag, &symbols_raw)) return 2;
            } else if (flag == QStringLiteral("--amount") || flag == QStringLiteral("--amount-usd") ||
                       flag == QStringLiteral("--amounts")) {
                if (!consume_value(args, i, flag, &amount_raw)) return 2;
            } else if (flag == QStringLiteral("--cadence-ms") || flag == QStringLiteral("--interval-ms")) {
                if (!consume_value(args, i, flag, &cadence_raw)) return 2;
            } else if (flag == QStringLiteral("--venue")) {
                if (!consume_value(args, i, flag, &venue_raw)) return 2;
            } else if (flag == QStringLiteral("--min-profit-bps")) {
                if (!consume_value(args, i, flag, &min_profit_raw)) return 2;
            } else if (flag == QStringLiteral("--min-confidence") || flag == QStringLiteral("--confidence")) {
                if (!consume_value(args, i, flag, &min_confidence_raw)) return 2;
            } else if (flag == QStringLiteral("--max-spread-bps")) {
                if (!consume_value(args, i, flag, &max_spread_raw)) return 2;
            } else if (flag == QStringLiteral("--min-live-sources")) {
                if (!consume_value(args, i, flag, &min_sources_raw)) return 2;
            } else if (flag == QStringLiteral("--fast")) {
                args.removeAt(i--);
                fast = true;
            } else if (flag == QStringLiteral("--safe")) {
                args.removeAt(i--);
                fast = false;
            } else if (flag == QStringLiteral("--live")) {
                std::fprintf(stderr, "automation start is paper-only; live automation is blocked\n");
                return 2;
            }
        }
        if (!args.isEmpty()) {
            std::fprintf(stderr, "unknown args: %s\n", qUtf8Printable(args.join(' ')));
            return 2;
        }

        if (symbols_raw.trimmed().isEmpty()) {
            if (preset == QStringLiteral("btc") || preset == QStringLiteral("bitcoin"))
                symbols_raw = QStringLiteral("BTC-USD");
            else
                symbols_raw = QStringLiteral("BTC-USD,ETH-USD,SOL-USD");
        }
        if (amount_raw.trimmed().isEmpty())
            amount_raw = QStringLiteral("50");
        if (cadence_raw.trimmed().isEmpty())
            cadence_raw = fast ? QStringLiteral("250") : QStringLiteral("1000");

        QStringList scalp_args{QStringLiteral("start"),
                               QStringLiteral("--symbols"), symbols_raw,
                               QStringLiteral("--amounts"), amount_raw,
                               QStringLiteral("--cadence-ms"), cadence_raw,
                               QStringLiteral("--venue"), venue_raw,
                               QStringLiteral("--maker"),
                               QStringLiteral("--min-profit-bps"), min_profit_raw,
                               QStringLiteral("--max-spread-bps"), max_spread_raw,
                               QStringLiteral("--min-live-sources"), min_sources_raw,
                               QStringLiteral("--paper")};
        if (!min_confidence_raw.trimmed().isEmpty())
            scalp_args << QStringLiteral("--min-confidence") << min_confidence_raw;
        if (!json) {
            std::printf("Paper automation is ON.\n");
            std::printf("It will watch %s with $%s paper orders and will not place live trades.\n",
                        qUtf8Printable(symbols_raw), qUtf8Printable(amount_raw));
            std::printf("Start or keep the daemon running with `daemon start` so it can keep watching.\n\n");
        }
        return daemon_scalp_command(profile, json, scalp_args);
    }

    std::fprintf(stderr, "usage: automation 24-7|start|status|recent|costs|explore|stop\n");
    return 2;
}

int daemon_collectors_command(const QString& profile, bool json, QStringList args) {
    const QString sub = args.isEmpty() ? QStringLiteral("status") : args.takeFirst().trimmed().toLower();
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: daemon collectors status|repair|run\n");
        return 2;
    }
    if (sub == "status" || sub == "list" || sub == "ls") {
        const QJsonObject out = collectors_status_object(profile);
        if (json) {
            std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        } else {
            std::printf("collectors   found=%d/%d enabled=%d failed=%d stale=%d\n",
                        out.value("found").toInt(),
                        out.value("total").toInt(),
                        out.value("enabled").toInt(),
                        out.value("failed").toInt(),
                        out.value("stale").toInt());
            std::printf("%-28s %-10s %-8s %-8s %-8s %s\n",
                        "NAME", "STATUS", "ENABLED", "LAST", "EVERY", "NEXT");
            for (const auto& v : out.value("collectors").toArray()) {
                const QJsonObject row = v.toObject();
                const QString every = QStringLiteral("%1s").arg(row.value("interval_sec").toInt());
                std::printf("%-28s %-10s %-8s %-8s %-8s %s\n",
                            qUtf8Printable(row.value("name").toString().left(28)),
                            qUtf8Printable(row.value("status").toString()),
                            row.value("enabled").toBool() ? "yes" : "no",
                            qUtf8Printable(row.value("last_status").toString("-").left(8)),
                            qUtf8Printable(every),
                            qUtf8Printable(row.value("next_run_at").toString("-")));
            }
        }
        return 0;
    }
    if (sub == "repair" || sub == "install" || sub == "ensure") {
        const QJsonObject out = repair_collectors(profile);
        if (json) {
            std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        } else {
            std::printf("collectors repaired: created=%d updated=%d\n",
                        out.value("created").toInt(), out.value("updated").toInt());
            const QJsonObject status = out.value("status").toObject();
            std::printf("collectors status: found=%d/%d enabled=%d failed=%d stale=%d\n",
                        status.value("found").toInt(),
                        status.value("total").toInt(),
                        status.value("enabled").toInt(),
                        status.value("failed").toInt(),
                        status.value("stale").toInt());
        }
        return out.value("ok").toBool() ? 0 : 7;
    }
    if (sub == "run") {
        QJsonObject repaired = repair_collectors(profile);
        QJsonArray rows = repaired.value("status").toObject().value("collectors").toArray();
        QJsonArray results;
        int failures = 0;
        for (const auto& row_value : rows) {
            const QJsonObject row = row_value.toObject();
            const QString id = row.value("id").toString();
            if (id.isEmpty())
                continue;
            QJsonObject doc = load_jobs_doc(profile);
            QJsonArray jobs = doc.value("jobs").toArray();
            const int idx = find_job_index(jobs, id);
            if (idx < 0)
                continue;
            QJsonObject job = jobs.at(idx).toObject();
            const QString run_id = record_job_run_start(profile, job, QStringLiteral("manual-collectors"));
            ProcessResult r = run_job_once_sync(profile, job);
            const QString status = r.exit_code == 0 ? QStringLiteral("ok") : QStringLiteral("failed");
            job["last_run_at"] = now_utc();
            job["last_exit_code"] = r.exit_code;
            job["last_status"] = status;
            job["last_output_tail"] = compact_tail(r.out + "\n" + r.err);
            job["running"] = false;
            job["current_run_id"] = QString();
            job["updated_at"] = now_utc();
            job["run_count"] = job.value("run_count").toInt() + 1;
            if (r.exit_code != 0) {
                job["fail_count"] = job.value("fail_count").toInt() + 1;
                ++failures;
            }
            jobs.replace(idx, job);
            jobs_save_update(profile, jobs);
            record_job_run_finish(profile, run_id, status, r.exit_code, r.out, r.err,
                                  r.exit_code == 0 ? QString() : compact_tail(r.err));
            results.append(QJsonObject{{"id", id},
                                       {"name", job.value("name")},
                                       {"status", status},
                                       {"exit_code", r.exit_code},
                                       {"stdout_tail", compact_tail(r.out)},
                                       {"stderr_tail", compact_tail(r.err)}});
            if (!json) {
                std::printf("[%s] %s\n", qUtf8Printable(status), qUtf8Printable(job.value("name").toString()));
                if (!r.out.trimmed().isEmpty())
                    std::printf("%s\n", qUtf8Printable(compact_tail(r.out)));
                if (!r.err.trimmed().isEmpty())
                    std::fprintf(stderr, "%s\n", qUtf8Printable(compact_tail(r.err)));
            }
        }
        if (json)
            std::printf("%s\n", QJsonDocument(QJsonObject{{"results", results},
                                                          {"failures", failures}})
                                    .toJson(QJsonDocument::Compact)
                                    .constData());
        return failures == 0 ? 0 : 5;
    }
    std::fprintf(stderr, "usage: daemon collectors status|repair|run\n");
    return 2;
}

int daemon_add_template_job(const QString& profile, bool json, const QString& kind, QStringList args) {
    QString name;
    int every_sec = 0;
    int timeout_sec = 0;
    bool enabled = true;
    QJsonObject spec = parse_job_spec(kind, args, &name, &every_sec, &timeout_sec, &enabled);
    if (spec.isEmpty() && kind != "health-check")
        return 2;
    QString normalized_kind = kind == "paper" ? QStringLiteral("paper-strategy") : kind;
    if (normalized_kind == "chronos" || normalized_kind == "forecast-book")
        normalized_kind = QStringLiteral("chronos2");
    if (normalized_kind == "chronos-equity" || normalized_kind == "equity-chronos" ||
        normalized_kind == "stock-chronos")
        normalized_kind = QStringLiteral("chronos2-equity");
    QJsonObject job = make_job(normalized_kind, name, spec, every_sec, timeout_sec, enabled);
    QJsonObject doc = load_jobs_doc(profile);
    QJsonArray jobs = doc.value("jobs").toArray();
    jobs.append(job);
    const int rc = jobs_save_update(profile, jobs);
    if (rc != 0) return rc;
    if (json)
        std::printf("%s\n", QJsonDocument(QJsonObject{{"job", job}}).toJson(QJsonDocument::Compact).constData());
    else
        std::printf("added %s  %s\n", qUtf8Printable(job.value("id").toString()),
                    qUtf8Printable(job.value("name").toString()));
    return 0;
}

int daemon_notify_command(const QString& profile, bool json, QStringList args) {
    const bool as_job = args.removeAll(QStringLiteral("--job")) > 0;
    if (as_job)
        return daemon_add_template_job(profile, json, QStringLiteral("notify"), args);
    QString name;
    int every_sec = 0;
    int timeout_sec = 0;
    bool enabled = true;
    QJsonObject spec = parse_job_spec(QStringLiteral("notify"), args, &name, &every_sec, &timeout_sec, &enabled);
    if (spec.isEmpty())
        return 2;
    QJsonObject job = make_job(QStringLiteral("notify"), name, spec, 0, timeout_sec, true);
    const ProcessResult r = run_job_once_sync(profile, job, 30000);
    if (json)
        std::printf("%s\n", QJsonDocument(QJsonObject{{"exit_code", r.exit_code},
                                                      {"stdout", r.out},
                                                      {"stderr", r.err}})
                                .toJson(QJsonDocument::Compact)
                                .constData());
    else {
        std::printf("%s", qUtf8Printable(r.out));
        if (!r.err.isEmpty()) std::fprintf(stderr, "%s", qUtf8Printable(r.err));
    }
    return r.exit_code == 0 ? 0 : 5;
}

bool wait_for_owner_clear(const QString& profile, qint64 pid, int timeout_ms) {
    const int sleep_ms = 100;
    const int tries = std::max(1, timeout_ms / sleep_ms);
    for (int i = 0; i < tries; ++i) {
        auto info = read_bridge_file(profile_root_for(profile));
        if (!info || !is_pid_alive(info->pid) || info->pid != pid)
            return true;
        QThread::msleep(sleep_ms);
    }
    auto info = read_bridge_file(profile_root_for(profile));
    return !info || !is_pid_alive(info->pid) || info->pid != pid;
}

int signal_profile_owner(const QString& profile, const QJsonObject& owner,
                         [[maybe_unused]] bool force_kill, QString* error) {
    const qint64 pid = static_cast<qint64>(owner.value(QStringLiteral("active_owner_pid")).toDouble());
    if (pid <= 0) {
        if (error) *error = QStringLiteral("active owner has no usable pid");
        return 7;
    }
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process) {
        if (error) *error = QStringLiteral("failed to open owner pid %1 for termination").arg(pid);
        return 7;
    }
    if (!TerminateProcess(process, 0)) {
        CloseHandle(process);
        if (error) *error = QStringLiteral("failed to terminate owner pid %1").arg(pid);
        return 7;
    }
    WaitForSingleObject(process, 5000);
    CloseHandle(process);
    remove_bridge_file(profile_root_for(profile));
    return 0;
#else
    if (::kill(static_cast<pid_t>(pid), SIGTERM) != 0) {
        if (error) *error = QStringLiteral("failed to signal owner pid %1").arg(pid);
        return 7;
    }
    if (wait_for_owner_clear(profile, pid, 5000)) {
        remove_bridge_file(profile_root_for(profile));
        return 0;
    }
    if (!force_kill) {
        if (error) *error = QStringLiteral("owner pid %1 did not exit after SIGTERM").arg(pid);
        return 7;
    }
    if (::kill(static_cast<pid_t>(pid), SIGKILL) != 0) {
        if (error) *error = QStringLiteral("failed to force-stop owner pid %1").arg(pid);
        return 7;
    }
    if (!wait_for_owner_clear(profile, pid, 3000)) {
        if (error) *error = QStringLiteral("owner pid %1 is still alive after SIGKILL").arg(pid);
        return 7;
    }
    remove_bridge_file(profile_root_for(profile));
    return 0;
#endif
}

int daemon_owner_command(const QString& profile, bool json) {
    const QJsonObject o = daemon_status_object(profile);
    if (json) {
        std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("profile  %s\n", qUtf8Printable(profile));
    std::printf("mode     %s\n", qUtf8Printable(o.value("mode").toString()));
    if (o.value("profile_owner_reachable").toBool()) {
        std::printf("owner    %s pid=%lld endpoint=%s\n",
                    qUtf8Printable(o.value("active_owner_kind").toString()),
                    static_cast<long long>(o.value("active_owner_pid").toDouble()),
                    qUtf8Printable(o.value("active_endpoint").toString()));
    } else {
        std::printf("owner    none\n");
    }
    std::printf("daemon   installed=%s loaded=%s running=%s\n",
                o.value("installed").toBool() ? "yes" : "no",
                o.value("loaded").toBool() ? "yes" : "no",
                o.value("scheduler_running").toBool() ? "yes" : "no");
    if (o.value("scheduler_running").toBool()) {
        const qint64 scheduler_pid = o.contains("daemon_process_pid")
                                         ? static_cast<qint64>(o.value("daemon_process_pid").toDouble())
                                         : static_cast<qint64>(o.value("pid").toDouble());
        std::printf("scheduler %s pid=%lld\n",
                    qUtf8Printable(o.value("daemon_process_mode").toString(
                        o.value("daemon_bridge_owner").toBool() ? QStringLiteral("owner") : QStringLiteral("warm"))),
                    static_cast<long long>(scheduler_pid));
    }
    return 0;
}

int daemon_takeover_command(const QString& profile, bool json, QStringList args) {
    auto has_flag = [&](const QString& flag) { return args.removeAll(flag) > 0; };
    const bool yes = has_flag(QStringLiteral("--yes"));
    const bool force = has_flag(QStringLiteral("--force"));
    const bool force_kill = has_flag(QStringLiteral("--kill"));
    const bool install = has_flag(QStringLiteral("--install"));
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: daemon takeover [--install] [--force --yes] [--kill]\n");
        return 2;
    }

    if (!QFileInfo::exists(daemon_plist_path(profile))) {
        if (!install) {
            std::fprintf(stderr, "daemon is not installed for profile '%s'; run daemon install or daemon takeover --install --yes\n",
                         qUtf8Printable(profile));
            return 3;
        }
        if (!yes) {
            std::fprintf(stderr, "usage: daemon takeover --install --yes\n");
            return 2;
        }
#if defined(Q_OS_MACOS)
        QString error;
        if (!write_daemon_plist(profile, &error)) {
            std::fprintf(stderr, "%s\n", qUtf8Printable(error));
            return 7;
        }
#else
        std::fprintf(stderr, "daemon install/start is currently supported on macOS launchd only\n");
        return 2;
#endif
    }

    QJsonObject current = daemon_status_object(profile);
    if (current.value("daemon_running").toBool()) {
        if (json) {
            current["takeover"] = QStringLiteral("already_daemon_owner");
            std::printf("%s\n", QJsonDocument(current).toJson(QJsonDocument::Compact).constData());
        } else {
            std::printf("CLI daemon already owns profile '%s'\n", qUtf8Printable(profile));
        }
        return 0;
    }

    if (current.value("profile_owner_reachable").toBool()) {
        const QString owner = current.value("active_owner_kind").toString();
        if (!force || !yes) {
            if (json) {
                current["takeover"] = QStringLiteral("blocked");
                current["reason"] = QStringLiteral("profile is owned by %1; rerun with --force --yes to move ownership")
                                        .arg(owner);
                std::printf("%s\n", QJsonDocument(current).toJson(QJsonDocument::Compact).constData());
            } else {
                std::fprintf(stderr,
                             "profile '%s' is owned by %s pid=%lld at %s\n"
                             "rerun `daemon takeover --force --yes` to terminate that owner and start the CLI daemon\n",
                             qUtf8Printable(profile),
                             qUtf8Printable(owner),
                             static_cast<long long>(current.value("active_owner_pid").toDouble()),
                             qUtf8Printable(current.value("active_endpoint").toString()));
            }
            return 3;
        }
        QString error;
        const int rc = signal_profile_owner(profile, current, force_kill, &error);
        if (rc != 0) {
            if (json) {
                current["takeover"] = QStringLiteral("failed_to_clear_owner");
                current["error"] = error;
                std::printf("%s\n", QJsonDocument(current).toJson(QJsonDocument::Compact).constData());
            } else {
                std::fprintf(stderr, "%s\n", qUtf8Printable(error));
            }
            return rc;
        }
    }

    return daemon_start_impl(profile, json);
}

int daemon_command(const QString& profile, bool json, QStringList args) {
    const QString sub = args.isEmpty() ? QStringLiteral("status") : args.takeFirst().trimmed().toLower();
    [[maybe_unused]] auto has_flag = [&](const QString& flag) { return args.removeAll(flag) > 0; };

    if (sub == "status" || sub == "check")
        return emit_daemon_status(profile, json);
    if (sub == "owner" || sub == "who")
        return daemon_owner_command(profile, json);
    if (sub == "takeover" || sub == "claim" || sub == "own")
        return daemon_takeover_command(profile, json, args);
    if (sub == "release")
        return daemon_stop_impl(profile, json);
    if (sub == "health")
        return emit_daemon_health(profile, json);
    if (sub == "readiness" || sub == "ready" || sub == "safety" || sub == "trade-gate")
        return emit_daemon_readiness(profile, json, args);
    if (sub == "logs" || sub == "log")
        return emit_daemon_logs(profile, json, args);
    if (sub == "audit" || sub == "security")
        return emit_daemon_audit(profile, json);
    if (sub == "jobs" || sub == "job")
        return daemon_jobs_command(profile, json, args);
    if (sub == "monitors" || sub == "monitor")
        return daemon_monitors_command(profile, json, args);
    if (sub == "scalp" || sub == "scalper" || sub == "microstructure-engine")
        return daemon_scalp_command(profile, json, args);
    if (sub == "automation" || sub == "auto" || sub == "bot" || sub == "trade-bot")
        return daemon_automation_command(profile, json, args);
    if (sub == "collectors" || sub == "collector" || sub == "feeds")
        return daemon_collectors_command(profile, json, args);
    if (sub == "sandbox" || sub == "strategy-sandbox")
        return daemon_sandbox_jobs_command(profile, json, args);
    if (sub == "notify" || sub == "notification")
        return daemon_notify_command(profile, json, args);
    if (sub == "ai") {
        if (args.isEmpty()) {
            std::fprintf(stderr, "usage: daemon ai <brief|risk|thesis|radar> <target> [--every-sec N]\n");
            return 2;
        }
        args.prepend(QStringLiteral("ai"));
        return daemon_jobs_command(profile, json, QStringList{QStringLiteral("add")} + args);
    }
    if (sub == "paper" || sub == "paper-strategy") {
        args.prepend(QStringLiteral("paper-strategy"));
        return daemon_jobs_command(profile, json, QStringList{QStringLiteral("add")} + args);
    }
    if (sub == "chronos2" || sub == "chronos" || sub == "forecast-book") {
        args.prepend(QStringLiteral("chronos2"));
        return daemon_jobs_command(profile, json, QStringList{QStringLiteral("add")} + args);
    }
    if (sub == "chronos2-equity" || sub == "chronos-equity" ||
        sub == "equity-chronos" || sub == "stock-chronos") {
        args.prepend(QStringLiteral("chronos2-equity"));
        return daemon_jobs_command(profile, json, QStringList{QStringLiteral("add")} + args);
    }

    if (sub == "install") {
#if !defined(Q_OS_MACOS)
        std::fprintf(stderr, "daemon install is currently supported on macOS launchd only\n");
        return 2;
#else
        const bool replace = has_flag(QStringLiteral("--replace")) || has_flag(QStringLiteral("--force"));
        const bool start = has_flag(QStringLiteral("--start"));
        const bool dry_run = has_flag(QStringLiteral("--dry-run"));
        if (!args.isEmpty()) {
            std::fprintf(stderr, "usage: daemon install [--replace] [--start] [--dry-run]\n");
            return 2;
        }
        const QString path = daemon_plist_path(profile);
        if (QFileInfo::exists(path) && !replace && !dry_run) {
            std::fprintf(stderr, "daemon already installed for profile '%s'; use --replace\n",
                         qUtf8Printable(profile));
            return 2;
        }
        if (!dry_run) {
            QString error;
            if (!write_daemon_plist(profile, &error)) {
                std::fprintf(stderr, "%s\n", qUtf8Printable(error));
                return 7;
            }
        }
        if (json) {
            QJsonObject o = daemon_status_object(profile);
            o["installed"] = !dry_run || QFileInfo::exists(path);
            o["dry_run"] = dry_run;
            o["program"] = QCoreApplication::applicationFilePath();
            std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
        } else {
            std::printf("%s %s\n", dry_run ? "would install" : "installed", qUtf8Printable(path));
        }
        if (start && !dry_run)
            return daemon_start_impl(profile, json);
        return 0;
#endif
    }

    if (sub == "uninstall" || sub == "remove" || sub == "rm") {
#if !defined(Q_OS_MACOS)
        std::fprintf(stderr, "daemon uninstall is currently supported on macOS launchd only\n");
        return 2;
#else
        const bool dry_run = has_flag(QStringLiteral("--dry-run"));
        if (!args.isEmpty()) {
            std::fprintf(stderr, "usage: daemon uninstall [--dry-run]\n");
            return 2;
        }
        const QString path = daemon_plist_path(profile);
        const bool was_installed = QFileInfo::exists(path);
        bool stopped = false;
        if (!dry_run && (launchd_loaded(profile) || daemon_status_object(profile).value("running").toBool())) {
            const int rc = daemon_stop_impl(profile, false, true);
            if (rc != 0 && rc != 3)
                return rc;
            stopped = true;
        }
        if (!dry_run && was_installed && !QFile::remove(path)) {
            std::fprintf(stderr, "failed to remove %s\n", qUtf8Printable(path));
            return 7;
        }
        if (json) {
            QJsonObject o{{"profile", profile},
                          {"label", daemon_label(profile)},
                          {"plist", path},
                          {"removed", was_installed && !dry_run},
                          {"would_remove", dry_run && was_installed},
                          {"stopped", stopped},
                          {"dry_run", dry_run}};
            std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
        } else if (dry_run) {
            std::printf("%s %s\n", was_installed ? "would remove" : "not installed", qUtf8Printable(path));
        } else if (was_installed) {
            std::printf("removed %s\n", qUtf8Printable(path));
        } else {
            std::fprintf(stderr, "daemon is not installed for profile '%s'\n", qUtf8Printable(profile));
            return 3;
        }
        return 0;
#endif
    }

    if (sub == "start")
        return daemon_start_impl(profile, json);
    if (sub == "stop")
        return daemon_stop_impl(profile, json);
    if (sub == "restart") {
        const int stop_rc = daemon_stop_impl(profile, false, true);
        if (stop_rc != 0 && stop_rc != 3)
            return stop_rc;
        for (int i = 0; i < 40; ++i) {
            const bool loaded = launchd_loaded(profile);
            const bool running = daemon_status_object(profile).value("running").toBool();
            if (!loaded && !running)
                break;
            QThread::msleep(250);
        }
        QThread::msleep(1000);
        return daemon_start_impl(profile, json);
    }
    if (sub == "plist" || sub == "path") {
        const QString path = daemon_plist_path(profile);
        if (json)
            std::printf("%s\n", QJsonDocument(QJsonObject{{"profile", profile},
                                                          {"label", daemon_label(profile)},
                                                          {"plist", path}})
                                    .toJson(QJsonDocument::Compact)
                                    .constData());
        else
            std::printf("%s\n", qUtf8Printable(path));
        return 0;
    }

    std::fprintf(stderr,
                 "usage: daemon status|owner|takeover|release|health|readiness|logs|audit|jobs|monitors|collectors|notify|ai|paper|"
                 "chronos2|chronos2-equity|install|uninstall|start|stop|restart|plist\n");
    return 2;
}

int sync_command(const QString& profile, bool json, QStringList args) {
    const QString sub = args.isEmpty() ? QStringLiteral("status") : args.takeFirst().trimmed().toLower();
    if (sub != QStringLiteral("status") && sub != QStringLiteral("check")) {
        std::fprintf(stderr, "usage: sync status\n");
        return 2;
    }
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: sync status\n");
        return 2;
    }

    const QJsonObject daemon = daemon_status_object(profile);
    DaemonReadinessOptions opt;
    const QJsonObject readiness = build_daemon_readiness(profile, opt);
    const bool owner_live = daemon.value("profile_owner_reachable").toBool();
    const QString owner = daemon.value("active_owner_kind").toString();
    const bool scheduler_running = daemon.value("scheduler_running").toBool();
    const bool daemon_bridge_owner = daemon.value("daemon_bridge_owner").toBool();

    QString overall = QStringLiteral("in_sync_for_interactive");
    QStringList notes;
    QStringList recommendations;
    if (!owner_live) {
        overall = QStringLiteral("no_owner");
        notes << QStringLiteral("no GUI or daemon currently owns the profile endpoint");
        recommendations << QStringLiteral("start the GUI for interactive work or start the daemon for unattended collection");
    } else if (daemon_bridge_owner) {
        overall = QStringLiteral("daemon_owner");
        notes << QStringLiteral("daemon owns the profile endpoint; CLI can attach and unattended jobs can run");
    } else if (owner == QStringLiteral("gui") && scheduler_running) {
        overall = QStringLiteral("gui_owner_daemon_warm");
        notes << QStringLiteral("GUI owns the profile endpoint; daemon scheduler is warm and can run background jobs");
    } else if (owner == QStringLiteral("gui")) {
        overall = QStringLiteral("gui_owner_daemon_standby");
        notes << QStringLiteral("GUI owns the profile endpoint; start daemon for warm background collection");
        recommendations << QStringLiteral("run `daemon start` to keep the scheduler warm while the GUI stays open");
    } else {
        overall = QStringLiteral("external_owner");
        notes << QStringLiteral("another process owns the profile endpoint");
        recommendations << QStringLiteral("inspect the owner before starting unattended daemon jobs");
    }

    if (readiness.value("status").toString() != QStringLiteral("ready"))
        recommendations << QStringLiteral("run `daemon readiness` for collector/model blockers");
    recommendations.removeDuplicates();

    QJsonObject out{{"profile", profile},
                    {"overall", overall},
                    {"daemon", daemon},
                    {"daemon_readiness", readiness},
                    {"notes", QJsonArray::fromStringList(notes)},
                    {"recommendations", QJsonArray::fromStringList(recommendations)},
                    {"generated_at", now_utc()}};

    if (json) {
        std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        return 0;
    }

    std::printf("sync         %s\n", qUtf8Printable(overall));
    std::printf("profile      %s\n", qUtf8Printable(profile));
    if (owner_live) {
        std::printf("owner        %s pid=%lld endpoint=%s\n",
                    qUtf8Printable(owner),
                    static_cast<long long>(daemon.value("active_owner_pid").toDouble()),
                    qUtf8Printable(daemon.value("active_endpoint").toString()));
    } else {
        std::printf("owner        none\n");
    }
    std::printf("daemon       %s installed=%s loaded=%s\n",
                scheduler_running ? (daemon_bridge_owner ? "owner" : "warm") : "standby",
                daemon.value("installed").toBool() ? "yes" : "no",
                daemon.value("loaded").toBool() ? "yes" : "no");
    std::printf("readiness    %s  trade gate=%s\n",
                qUtf8Printable(readiness.value("overall").toString()),
                qUtf8Printable(readiness.value("trade_gate").toString()));
    if (!notes.isEmpty()) {
        std::printf("\nNOTES\n");
        for (const QString& n : notes)
            std::printf("- %s\n", qUtf8Printable(n));
    }
    if (!recommendations.isEmpty()) {
        std::printf("\nNEXT\n");
        for (const QString& r : recommendations)
            std::printf("- %s\n", qUtf8Printable(r));
    }
    return 0;
}

} // namespace openmarketterminal::cli
