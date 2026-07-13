#pragma once

#include "core/result/Result.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace openmarketterminal::services::decision {

struct DecisionSignal {
    QString name;
    QString source;
    QString model_version;
    qint64 as_of_ms = 0;
    double probability = 0.5;
    double confidence = 0.0;
    double calibration_score = 0.0;
    int sample_count = 0;
    qint64 max_age_ms = 0;
    bool advisory_only = false;
    QJsonObject features;
};

struct ExecutionQuote {
    QString venue;
    QString market_id;
    QString side;
    bool executable = false;
    qint64 observed_at_ms = 0;
    double bid = 0.0;
    double ask = 0.0;
    // Binary contracts use ask as implied probability. Spot/derivative venues
    // carry dollar prices in bid/ask and set this comparison baseline directly.
    double implied_probability = -1.0;
    // Venue-normalized spread used by the risk gate. Prediction markets use
    // absolute probability points; spot venues use spread as a price fraction.
    double normalized_spread = -1.0;
    double available_size = 0.0;
    double spread_cost = 0.0;
    double fee_cost = 0.0;
    double slippage_cost = 0.0;
    double exit_cost_reserve = 0.0;
};

struct PortfolioState {
    double total_exposure = 0.0;
    double symbol_exposure = 0.0;
    double venue_exposure = 0.0;
    double daily_realized_pnl = 0.0;
    int open_positions = 0;
};

struct DecisionRiskLimits {
    double requested_notional = 0.0;
    double max_total_exposure = 0.0;
    double max_symbol_exposure = 0.0;
    double max_venue_exposure = 0.0;
    double max_daily_loss = 0.0;
    int max_open_positions = 0;
    double minimum_edge = 0.0;
    double minimum_confidence = 0.0;
    double minimum_liquidity = 0.0;
    double maximum_spread = 1.0;
    qint64 maximum_quote_age_ms = 0;
    int minimum_seconds_left = 0;
};

struct DecisionRequest {
    QString decision_id;
    qint64 decision_ts_ms = 0;
    QString symbol;
    QString horizon;
    QString source;
    QString data_snapshot_id;
    int seconds_left = -1;
    QVector<DecisionSignal> model_signals;
    ExecutionQuote quote;
    PortfolioState portfolio;
    DecisionRiskLimits limits;
    QJsonObject context;
};

struct DecisionEnvelope {
    int schema_version = 1;
    QString decision_id;
    qint64 decision_ts_ms = 0;
    QString symbol;
    QString horizon;
    QString source;
    QString data_snapshot_id;
    QString venue;
    QString market_id;
    QString side;
    int seconds_left = -1;
    double model_probability = 0.5;
    double market_probability = 0.0;
    double confidence = 0.0;
    double raw_edge = 0.0;
    double total_cost = 0.0;
    double edge_after_cost = 0.0;
    QString verdict;
    QStringList reasons;
    QStringList risk_blockers;
    QVector<DecisionSignal> accepted_signals;
    QVector<DecisionSignal> rejected_signals;
    ExecutionQuote quote;
    PortfolioState portfolio;
    DecisionRiskLimits limits;
    QJsonObject context;
    QString content_hash;

    QJsonObject to_json(bool include_hash = true) const;
};

struct DecisionReplayResult {
    int frames = 0;
    int trade_candidates = 0;
    int no_trade = 0;
    int bad_price = 0;
    int too_late = 0;
    int not_enough_data = 0;
    QVector<DecisionEnvelope> envelopes;
};

class DecisionOrchestrator {
  public:
    static DecisionEnvelope evaluate(const DecisionRequest& request);
    static DecisionReplayResult replay(const QVector<DecisionRequest>& requests);
    static QStringList validate(const DecisionEnvelope& envelope);
    static QString content_hash(const DecisionEnvelope& envelope);
    static Result<void> persist_immutable(const DecisionEnvelope& envelope,
                                          const QString& journal_id = {});
};

} // namespace openmarketterminal::services::decision
