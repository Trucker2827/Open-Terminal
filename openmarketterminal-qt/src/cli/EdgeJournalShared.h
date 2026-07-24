#pragma once
// Shared surface for edge-journal CLI commands compiled OUTSIDE the huge
// CommandDispatch.cpp translation unit. CommandDispatch.cpp (~27k lines)
// deterministically crashes the MSVC FRONT-END (C1001, msc1.cpp:1589) while
// parsing near edge_journal_rare_alerts_command — position-dependent, not
// content-dependent (survived brace-init removal and optimisation pragmas;
// recurred across v0.3.25/26/29). Splitting the function into its own small
// TU (EdgeJournalRareAlerts.cpp) is the structural fix; these declarations
// give the split TU access to CommandDispatch's helpers without duplicating
// them. Definitions stay in CommandDispatch.cpp (now external linkage).

#include "cli/CommandDispatch.h"

#include "core/result/Result.h"
#include "services/crypto/CryptoFees.h"
#include "services/edge_radar/CryptoMicrostructureRadar.h"

#include <QSqlQuery>
#include "services/notifications/NotificationService.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

namespace openmarketterminal::cli {

struct EdgeCryptoTrust {
    QString symbol;
    QString horizon;
    int decisions = 0;
    int resolved = 0;
    int wins = 0;
    int buy_resolved = 0;
    int buy_wins = 0;
    int no_buy_resolved = 0;
    int no_buy_wins = 0;
    double avg_edge = 0.0;
    double avg_confidence = 0.0;
    double trust = 0.0;
    QString status;
};

bool take_bool_flag(QStringList& args, const QString& flag);
bool take_string_option(QStringList& args, const QString& flag, QString& out);
bool require_yes(QStringList& args, const char* usage_line);
const char* edge_journal_cols();
EdgeCryptoTrust edge_crypto_trust_for_symbol(const QString& symbol, const QString& horizon,
                                             int max_age_hours);
QString edge_time_text(qint64 ts_ms);
QString edge_pct(double v);
bool init_headless_for_cli(const GlobalOpts& opts, int& exit_code);
bool notify_wait_send(notifications::INotificationProvider* provider,
                      const notifications::NotificationRequest& req, int timeout_ms, bool& ok,
                      QString& error);

int edge_journal_rare_alerts_command(const GlobalOpts& opts, QStringList args);

struct CryptoRecommendationDecision {
    QString id;
    QString symbol;
    QString venue;
    QString horizon;
    QString direction;
    QString side;
    QString call;
    QString gate;
    QString data_status;
    QString rationale;
    double market_probability = 0.5;
    double model_probability = 0.5;
    double raw_edge = 0.0;
    double edge_after_cost = 0.0;
    double gate_edge = 0.0;
    double spread_cost = 0.0;
    double fee_cost = 0.0;
    double slippage_cost = 0.0;
    double liquidity_score = 0.0;
    double confidence = 0.0;
    double reference_price = 0.0;
    int seconds_left = 0;
};

// Defined in CommandDispatch.cpp; used by the split edge-journal TU.
QString elide_text(QString s, int max = 88);
QJsonObject edge_journal_row_to_json(QSqlQuery& q);
QString edge_outcome_text(int outcome);
int edge_parse_outcome(const QString& raw);
QString edge_price_or_dash(double price);
// Venue fee economics live in the shared services-level table so the GUI and
// CLI read the same constants; these names keep existing CLI code compiling.
using CryptoFeeProfile = services::crypto::CryptoFeeProfile;
inline CryptoFeeProfile crypto_fee_profile_for_venue(const QString& venue) {
    return services::crypto::crypto_fee_profile_for_venue(venue);
}
inline QString crypto_fee_venue_key(const QString& venue) {
    return services::crypto::crypto_fee_venue_key(venue);
}
inline QString crypto_fee_key(const QString& venue_key, const QString& field) {
    return services::crypto::crypto_fee_key(venue_key, field);
}
bool edge_parse_duration_ms(const QString& raw, int& duration_ms);
bool edge_parse_bps_text(const QString& raw, double& out, const char* flag);
QStringList edge_safe_latency_sources_for_symbol(const QString& raw, const QString& symbol);
CryptoRecommendationDecision edge_score_crypto_recommendation(
    const QString& symbol,
    const QString& venue,
    const services::edge_radar::CryptoMicrostructureSnapshot& snapshot,
    int horizon_sec,
    double fee_bps,
    double slippage_bps,
    double min_edge_bps);
Result<QString> edge_journal_insert_crypto_recommendation(
    const CryptoRecommendationDecision& decision,
    const services::edge_radar::CryptoMicrostructureSnapshot& snapshot,
    int horizon_sec,
    double fee_bps,
    double slippage_bps,
    double min_edge_bps);
bool edge_append_decision_journal_to_lake(const QString& id, QString* error = nullptr);
QStringList edge_parse_crypto_universe_symbols(const QString& raw);
QString edge_base_crypto_symbol(QString symbol);
int edge_journal_evaluate_btc5m_live_command(const GlobalOpts& opts, QStringList args);
QString edge_crypto_raw_tick_symbol(const QString& symbol);
QJsonObject edge_context_decision_summary(const QString& symbol, int days);
QJsonObject edge_context_model_summary(const QString& symbol, int limit);

// Defined in EdgeJournalCommands.cpp; used by later CommandDispatch code.
QString edge_context_symbol_base(QString symbol);
QString edge_context_normalized_symbol(const QString& raw, bool crypto);
QStringList edge_context_news_keywords(const QString& symbol, bool crypto);
QJsonObject edge_context_news_summary(const QStringList& keywords, int days, int limit);
int edge_journal_command(const GlobalOpts& opts, QStringList args);

// A published Quant Lab model signal for one symbol (quant-signals.json,
// written by scripts/ai_quant_lab/signal_publisher.py). Advisory evidence
// only — `trusted` is EARNED by the publisher's trailing-IC rule and an
// untrusted direction must be treated as noise by every consumer.
struct EdgeModelSignal {
    bool present = false;
    bool fresh = false;
    bool trusted = false;
    double score = 0.0;
    int rank = 0;
    QString direction;
    QString model_id;
    qint64 age_ms = -1;
    double rank_ic = 0.0;
};

inline EdgeModelSignal edge_parse_model_signal(const QJsonObject& doc,
                                               const QString& symbol_base,
                                               qint64 now_ms,
                                               qint64 max_age_ms) {
    EdgeModelSignal out;
    const QJsonObject signal_map = doc.value(QStringLiteral("signals")).toObject();
    const QJsonValue entry = signal_map.value(symbol_base.toUpper());
    if (!entry.isObject())
        return out;
    const QJsonObject sig = entry.toObject();
    out.present = true;
    out.score = sig.value(QStringLiteral("score")).toDouble();
    out.rank = sig.value(QStringLiteral("rank")).toInt();
    out.direction = sig.value(QStringLiteral("direction")).toString();
    out.model_id = doc.value(QStringLiteral("model_id")).toString();
    const qint64 generated = static_cast<qint64>(
        doc.value(QStringLiteral("generated_at_ms")).toDouble());
    out.age_ms = generated > 0 ? now_ms - generated : -1;
    out.fresh = out.age_ms >= 0 && out.age_ms <= max_age_ms;
    out.trusted = doc.value(QStringLiteral("trusted")).toBool(false) && out.fresh;
    out.rank_ic = doc.value(QStringLiteral("trailing_ic")).toObject()
                      .value(QStringLiteral("rank_ic_mean")).toDouble();
    return out;
}

struct EdgeScalpGate {
    QString symbol;
    QString venue;
    QString horizon;
    QString verdict;
    QString action;
    QString journal_id;
    QString rationale;
    QStringList blockers;
    CryptoRecommendationDecision decision;
    services::edge_radar::CryptoMicrostructureSnapshot snapshot;
    EdgeCryptoTrust trust;
    double expected_capture_bps = 0.0;
    double observed_move_bps = 0.0;
    double round_trip_cost_bps = 0.0;
    double net_after_cost_bps = 0.0;
    double fee_bps = 0.0;
    double slippage_bps = 0.0;
    double safety_bps = 0.0;
    double capture_ratio = 0.0;
    double min_net_bps = 0.0;
    int horizon_sec = 0;
    // Ambient realized volatility (0 = unavailable; the noise blocker then
    // fails OPEN so symbols without stored tick series still gate normally).
    double realized_vol_per_min_bps = 0.0;
    int realized_vol_samples = 0;
    double observed_move_sigma = 0.0;
    double min_move_sigma = 0.0;
    EdgeModelSignal model_signal;
    double min_model_ic = 0.0;
};

// Vol/noise math is shared with the radar (services/edge_radar); these
// wrappers keep the established CLI names.
inline double edge_annual_vol_to_per_min_bps(double annual_volatility) {
    return services::edge_radar::annual_vol_to_per_min_bps(annual_volatility);
}
inline double edge_move_noise_sigma(double observed_move_bps, double vol_per_min_bps,
                                    int window_sec) {
    return services::edge_radar::move_noise_sigma(observed_move_bps, vol_per_min_bps, window_sec);
}

QString edge_crypto_regime_from_features(const QJsonObject& features);
QJsonObject edge_crypto_cost_json(const QJsonObject& features);

struct CryptoRecommendationOutcome {
    QString id;
    QString call;
    QString side;
    QString symbol;
    QString raw_tick_symbol;
    double reference_price = 0.0;
    double future_price = 0.0;
    double move = 0.0;
    double breakeven = 0.0;
    int outcome = -1;
    qint64 decision_ts = 0;
    qint64 target_ts = 0;
    qint64 future_ts = 0;
    QString tick_source;
};
Result<CryptoRecommendationOutcome> edge_score_crypto_recommendation_outcome(QSqlQuery& q);

QString edge_normalize_stats_horizon(QString horizon);
bool edge_crypto_is_buy_call(const QString& call, const QString& side);
double edge_rate(int wins, int resolved);
int edge_journal_crypto_stats_command(const GlobalOpts& opts, QStringList args);
int edge_journal_evidence_command(const GlobalOpts& opts, QStringList args);
int edge_journal_list_command(const GlobalOpts& opts, QStringList args);
int edge_journal_no_trade_command(const GlobalOpts& opts, QStringList args);
int edge_journal_paper_sim_command(const GlobalOpts& opts, QStringList args);
int edge_journal_proof_loop_command(const GlobalOpts& opts, QStringList args);
int edge_journal_regimes_command(const GlobalOpts& opts, QStringList args);
int edge_journal_replay_command(const GlobalOpts& opts, QStringList args);
int edge_journal_resolve_command(const GlobalOpts& opts, QStringList args);
int edge_journal_score_crypto_command(const GlobalOpts& opts, QStringList args);
int edge_journal_show_command(const GlobalOpts& opts, QStringList args);
int edge_journal_stats_command(const GlobalOpts& opts, QStringList args);
int edge_journal_trust_command(const GlobalOpts& opts, QStringList args);
QJsonObject edge_context_broker_summary(const QString& symbol, int days);
QJsonArray edge_context_public_sources(bool crypto, const QString& symbol);
int edge_scalp_gate_command(const GlobalOpts& opts, QStringList args);
int edge_decision_cockpit_command(const GlobalOpts& opts, QStringList args);
int edge_equity_cockpit_command(const GlobalOpts& opts, QStringList args);
int edge_context_command(const GlobalOpts& opts, QStringList args);
services::edge_radar::CryptoMicrostructureSnapshot edge_capture_microstructure(
    const QString& symbol, const QStringList& sources, int duration_ms, bool store_raw_ticks = false);

} // namespace openmarketterminal::cli
