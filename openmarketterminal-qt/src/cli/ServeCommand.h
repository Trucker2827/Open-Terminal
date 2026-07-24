#pragma once
#include <QDir>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QVector>
namespace openmarketterminal::cli {
// Run the daemon for `profile`. Blocks in the event loop until SIGTERM/SIGINT
// or a fatal init error. Returns a process exit code (0 clean, 3 already-owned,
// 7 init failure). status()/stop() are the management subcommands.
int serve_run(const QString& profile);
int serve_status(const QString& profile, bool json);
int serve_stop(const QString& profile);
int daemon_command(const QString& profile, bool json, QStringList args);
int sync_command(const QString& profile, bool json, QStringList args);

// Pure watchdog predicates kept public for deterministic regression tests.
bool kalshi_event_stream_needs_recovery(bool workload_active, bool connected,
                                        int subscribed_assets, qint64 liveness_age_ms,
                                        qint64 market_data_age_ms,
                                        qint64 dead_after_ms);
bool daemon_tick_sample_due(qint64 received_at_ms, qint64 last_sampled_ms,
                            qint64 minimum_interval_ms);
bool kalshi_account_reconciliation_due(bool live_session_active, bool pending,
                                       qint64 last_completed_ms, qint64 now_ms,
                                       qint64 interval_ms);
bool kalshi_universe_request_timed_out(bool pending, qint64 request_age_ms,
                                      qint64 timeout_ms);
bool kalshi_planner_process_timed_out(bool active, qint64 process_age_ms,
                                      qint64 timeout_ms);
bool kalshi_non_execution_process_timed_out(bool active, qint64 process_age_ms,
                                            qint64 timeout_ms);
qint64 kalshi_event_cycle_delay_ms(bool live_session_active, bool paper_active,
                                   qint64 elapsed_ms);
struct KalshiBookReceipt {
    QString signature;
    QJsonObject snapshot;
    bool meaningful_change = false;
};
KalshiBookReceipt kalshi_book_receipt(const QString& previous_signature,
                                     const QString& asset_id,
                                     double bid, double ask,
                                     double bid_size, double ask_size,
                                     qint64 exchange_observed_at_ms,
                                     qint64 received_at_ms,
                                     const QString& source = QStringLiteral("kalshi_websocket"));
enum class KalshiBookReceiptAuthority {
    FreshnessOnly,
    PlannerTrigger,
};
// Applies the receipt to the same evidence/signature state used by the live
// engine. REST is freshness-only; only WebSocket receipts may change trigger
// state and request a planner wakeup.
bool kalshi_accept_book_receipt(QJsonObject& snapshots,
                                QHash<QString, QString>& signatures,
                                const QString& asset_id,
                                const KalshiBookReceipt& receipt,
                                KalshiBookReceiptAuthority authority);
// The event engine operates on the nearest live crypto settlement cohorts. Keep
// the child planner bounded enough to finish before another quote snapshot ages
// out; this is deliberately separate from the broader manual CLI planner.
QStringList kalshi_event_planner_args();

// Scalp/spot style — ONE engine, two parameter presets. Normalization and
// style-dependent defaults are shared by `daemon scalp start` and the
// `scalp explore` analyzer, and kept public for regression tests.
// normalize: ""/aliases -> "scalp"/"spot"; unknown -> "" (caller errors).
QString scalp_style_normalize(const QString& raw);
double scalp_style_default_min_profit_bps(const QString& style);
double scalp_style_default_capture_ratio(const QString& style);
// Only independent exchange ticks are eligible to keep the Kalshi planner's
// spot reference fresh. Persistence is rate-limited so the daemon gets a
// current executable reference without turning the feature store into a raw
// websocket dump.
bool kalshi_should_persist_independent_spot_tick(const QString& source, double price,
                                                 qint64 received_ts_ms,
                                                 qint64 last_persisted_ts_ms,
                                                 qint64 minimum_interval_ms);

// A compact, advisory-only view of the public Kalshi event stream. These are
// contract quantities, never a count of distinct people or an execution signal.
struct KalshiFlowLevel {
    double price = 0.0;
    double quantity = 0.0;
};

struct KalshiFlowTrade {
    qint64 ts_ms = 0;
    QString outcome; // yes or no
    double quantity = 0.0;
};

struct KalshiFlowDelta {
    qint64 ts_ms = 0;
    QString outcome; // yes or no
    double quantity_delta = 0.0; // positive = added, negative = removed
};

struct KalshiFlowMetrics {
    qint64 observed_at_ms = 0;
    double yes_bid_depth = 0.0;
    double no_bid_depth = 0.0;
    double yes_taker_quantity = 0.0;
    double no_taker_quantity = 0.0;
    double yes_added_quantity = 0.0;
    double no_added_quantity = 0.0;
    double yes_removed_quantity = 0.0;
    double no_removed_quantity = 0.0;
    int recent_trade_count = 0;
    int recent_delta_count = 0;
    double depth_imbalance = 0.0;
    double taker_imbalance = 0.0;
    double wall_imbalance = 0.0;
    double combined_pressure = 0.0;
    QString signal = QStringLiteral("MIXED");
    QString confidence = QStringLiteral("LOW");
};

struct KalshiFlowPriceSample {
    qint64 ts_ms = 0;
    double price = 0.0;
};

struct KalshiFlowQuote {
    double bid = 0.0;
    double ask = 0.0;
    double bid_size = 0.0;
    double ask_size = 0.0;
};

KalshiFlowMetrics kalshi_flow_metrics(const QVector<KalshiFlowLevel>& yes_bid_levels,
                                      const QVector<KalshiFlowLevel>& no_bid_levels,
                                      const QVector<KalshiFlowTrade>& trades,
                                      const QVector<KalshiFlowDelta>& deltas,
                                      qint64 now_ms,
                                      qint64 lookback_ms = 30000,
                                      int depth_levels = 5);
QJsonObject kalshi_flow_to_json(const KalshiFlowMetrics& metrics);
// Multi-window context and economics are deliberately pure and advisory. They
// make the exact evidence visible to CLI/GUI/journals; they do not authorize
// an order.
QJsonObject kalshi_flow_windows_to_json(const QVector<KalshiFlowLevel>& yes_bid_levels,
                                        const QVector<KalshiFlowLevel>& no_bid_levels,
                                        const QVector<KalshiFlowTrade>& trades,
                                        const QVector<KalshiFlowDelta>& deltas,
                                        qint64 now_ms);
QJsonObject kalshi_flow_divergence_to_json(double spot_change_bps,
                                           double contract_change_cents,
                                           const KalshiFlowMetrics& short_window);
QJsonObject kalshi_flow_execution_to_json(const KalshiFlowQuote& yes,
                                          const KalshiFlowQuote& no,
                                          double yes_fee_per_contract,
                                          double no_fee_per_contract);

// Cross-exchange spot microstructure is a confirmation/abstention layer for a
// Kalshi candidate. It can veto a proposed side but can never create trading
// authority or manufacture model edge on its own.
struct KalshiSpotConfirmation {
    bool eligible = false;
    QString side;
    QString direction;
    double confidence = 0.0;
    double book_pressure = 0.0;
    double tape_pressure = 0.0;
    double aggressor_pressure = 0.0;
    double aggressor_coverage = 0.0;
    int classified_trades = 0;
    double cross_source_spread_bps = 0.0;
    int live_sources = 0;
    int top_book_sources = 0;
    QStringList blockers;

    QJsonObject to_json() const;
};

KalshiSpotConfirmation kalshi_spot_microstructure_confirmation(
    const QJsonObject& execution_snapshot, const QString& selected_side,
    qint64 decision_ts_ms, qint64 maximum_snapshot_age_ms = 5000);

// Hysteresis prevents a single noisy snapshot from flipping autonomous state.
struct KalshiSignalTransition {
    QString state = QStringLiteral("WARMING");
    QString pending_state;
    int consecutive = 0;
    bool changed = false;
    QJsonObject to_json() const;
};
KalshiSignalTransition kalshi_signal_transition(const QString& current_state,
                                                const QString& pending_state,
                                                int consecutive,
                                                const QString& observation,
                                                int confirmations_required = 3);
QJsonObject kalshi_contract_horizon(double spot, double floor_strike, double cap_strike,
                                    qint64 seconds_left, double realized_move_30s_bps,
                                    const QString& settlement_source);

// The shared advisory-evidence directory (kalshi-ws-books.json,
// quant-signals.json, calibrator.json). Header-inline so every evidence
// consumer (daemon, scalp gate, tests) resolves the same location without
// needing ServeCommand.cpp in its link.
inline QString kalshi_evidence_path(const QString& filename) {
    const QString override_dir = qEnvironmentVariable("OPENTERMINAL_KALSHI_EVIDENCE_DIR").trimmed();
    const QString dir = override_dir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
              QStringLiteral("/Open Terminal/Open Terminal")
        : override_dir;
    QDir().mkpath(dir);
    return QDir(dir).filePath(filename);
}

// GUI daemon indicator: daemon liveness is classified from the
// kalshi-ws-books.json heartbeat IN-PROCESS, so indicator truth never depends
// on shelling out to openterminalcli. Three honest states:
//   "fresh" — heartbeat within fresh_within_ms (daemon is writing evidence now)
//   "stale" — a heartbeat exists but is older than fresh_within_ms
//   "none"  — no parseable heartbeat (missing file, zero, or a future
//             timestamp, which is mistrusted rather than clamped)
// Header-inline for the same reason as kalshi_evidence_path: the GUI does not
// link ServeCommand.cpp.
struct KalshiDaemonEvidenceFreshness {
    QString state = QStringLiteral("none");
    qint64 age_ms = -1;  // -1 when no valid heartbeat exists
};
inline KalshiDaemonEvidenceFreshness kalshi_daemon_evidence_freshness(
    qint64 updated_at_ms, qint64 now_ms, qint64 fresh_within_ms = 30'000) {
    KalshiDaemonEvidenceFreshness out;
    if (updated_at_ms <= 0 || now_ms <= 0 || updated_at_ms > now_ms) return out;
    out.age_ms = now_ms - updated_at_ms;
    out.state = out.age_ms <= fresh_within_ms ? QStringLiteral("fresh")
                                              : QStringLiteral("stale");
    return out;
}
// write_book_snapshot() serializes updated_at_ms as a decimal STRING; accept a
// numeric value too so a future schema change cannot silently read as "none".
inline qint64 kalshi_ws_books_heartbeat_ms(const QJsonObject& root) {
    const QJsonValue value = root.value(QStringLiteral("updated_at_ms"));
    if (value.isString()) return value.toString().toLongLong();
    if (value.isDouble()) return static_cast<qint64>(value.toDouble());
    return 0;
}

// Pure daemon-job-spec -> CLI-args builder, kept public for deterministic
// regression tests.
QStringList command_for_job_kind(const QString& kind, const QJsonObject& spec);
}
