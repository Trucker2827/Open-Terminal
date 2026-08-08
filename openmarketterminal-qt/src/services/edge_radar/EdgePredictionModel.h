#pragma once

#include "storage/repositories/EdgePredictionModelRepository.h"

#include <QJsonObject>
#include <QStringList>

namespace openmarketterminal::services::edge_radar {

struct EdgePredictionFeatures {
    QString symbol = QStringLiteral("BTC");
    QString horizon = QStringLiteral("5m");
    QString direction = QStringLiteral("up");
    double market_probability = 0.5;
    double btc_anchor_probability = 0.5;
    double move_5s_pct = 0.0;
    double move_15s_pct = 0.0;
    double move_60s_pct = 0.0;
    double liquidity_score = 0.0;
    double spread_cost = 0.0;
    double fee_cost = 0.0;
    double context_5m_probability = 0.5;
    double context_15m_probability = 0.5;
    double context_1h_probability = 0.5;
    double context_daily_probability = 0.5;
    double context_5m_confidence = 0.0;
    double context_15m_confidence = 0.0;
    double context_1h_confidence = 0.0;
    double context_daily_confidence = 0.0;
    double lower_horizon_consensus = 0.0;
    double higher_horizon_consensus = 0.0;
    int seconds_left = -1;
    qint64 decision_ts = 0;
    QStringList feature_sources;
    QStringList leakage_rejections;
};

struct EdgePredictionEstimate {
    QString symbol;
    QString horizon;
    QString probability_source = QStringLiteral("heuristic");
    QString readiness = QStringLiteral("collecting");
    QString direction = QStringLiteral("flat");
    QString rationale;
    QStringList warnings;
    QStringList feature_sources;
    QStringList leakage_rejections;
    double probability = 0.5;
    double confidence = 0.0;
    double net_edge = 0.0;
    double lower_horizon_consensus = 0.0;
    double higher_horizon_consensus = 0.0;
    qint64 decision_ts = 0;
    qint64 freshest_context_at = 0;
    int freshness_ms = -1;
    int sample_count = 0;
    bool model_ready = false;
};

struct EdgeMetaGateDecision {
    QString call = QStringLiteral("NOT ENOUGH DATA");
    QString symbol;
    QString horizon;
    QString explanation;
    QStringList reasons;
    EdgePredictionEstimate primary;
    QVector<EdgePredictionModelOutput> context_outputs;
    double market_probability = 0.0;
    double net_edge = 0.0;
    qint64 decision_ts = 0;
};

struct EdgePredictionTrainResult {
    EdgePredictionModelRecord model;
    QString readiness;
    QString message;
    bool trained = false;
};

class EdgePredictionModel {
  public:
    static QString normalize_horizon(QString horizon);
    static QStringList supported_horizons();
    static int horizon_seconds(const QString& horizon);

    static Result<EdgePredictionTrainResult> train(const QString& symbol,
                                                   const QString& horizon,
                                                   int minimum_samples = 30);
    static EdgePredictionFeatures build_features(const EdgePredictionFeatures& base,
                                                 qint64 decision_ts);
    static EdgePredictionEstimate estimate(const EdgePredictionFeatures& features,
                                           int minimum_samples = 30);
    static EdgeMetaGateDecision meta_gate(const EdgePredictionFeatures& features,
                                          int minimum_samples = 30);
    static Result<void> publish_estimate(const EdgePredictionEstimate& estimate,
                                         const QString& source = QStringLiteral("local"));
    static bool no_lookahead_selftest(QString* details = nullptr);
    static QJsonObject estimate_to_json(const EdgePredictionEstimate& estimate);
    static QJsonObject meta_gate_to_json(const EdgeMetaGateDecision& decision);
    static QJsonObject train_result_to_json(const EdgePredictionTrainResult& result);

  private:
    static double heuristic_probability(const EdgePredictionFeatures& features);
};

// --- raw-tick retention -----------------------------------------------------
//
// edge_prediction_raw_ticks grows ~1.1-1.4M rows/day and was never pruned; it
// reached 43.3M rows / 10GB, which is what made an unindexed scan expensive
// enough to starve the Kalshi planner. Retention is deliberately narrow: only
// the high-volume exchange tape whose research value has already been settled
// is bounded. Everything else is kept forever.
//
// NEVER prunable, by design:
//   cfbenchmarks:BRTI  -- the Kalshi settlement feed; our capture IS the
//                         settlement reference (validated to median |err| $0.00
//                         against Kalshi's published expiration_value).
//   coinbase-1m-close  -- the long-history 1-minute series (back to 2025-12-19)
//                         at only ~300k rows.
//   coinbase, kraken   -- the spot aggregate that list_spot_price_series_since
//                         reads, and the tape prior edge tests drew on.
bool edge_tick_source_is_prunable(const QString& source);

// Sources bounded by retention, in a stable order.
QStringList edge_tick_prunable_sources();

// Cutoff timestamp for a retention window. Returns 0 for a non-positive
// keep_days so a misconfigured job deletes nothing rather than everything.
qint64 edge_tick_retention_cutoff_ms(qint64 now_ms, int keep_days);

} // namespace openmarketterminal::services::edge_radar
