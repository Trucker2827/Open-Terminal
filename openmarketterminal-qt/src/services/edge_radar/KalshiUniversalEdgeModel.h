#pragma once

#include "services/edge_radar/EdgeRadarService.h"
#include "services/prediction/PredictionTypes.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace openmarketterminal::services::edge_radar {

struct KalshiUniversalOptions {
    double spread_cost_fallback = 0.02;
    double fee_cost = 0.0175;
    double minimum_net_edge = 0.05;
    double strong_net_edge = 0.08;
    double minimum_liquidity_score = 0.30;
    double safety_buffer = 0.01;
    double exit_cost_reserve = 0.01;
    double annual_volatility = 0.70;
    int minimum_seconds_left = 20;
    int maximum_seconds_left = 2 * 24 * 60 * 60;
    bool allow_market_implied_baseline = true;
};

struct KalshiUniversalSignal {
    QString market_id;
    QString event_id;
    QString question;
    QString family;
    QString time_horizon;
    QString probability_source;
    double market_probability = 0.0;
    double model_probability = 0.0;
    double raw_edge = 0.0;
    double edge_after_cost = 0.0;
    double gate_edge = 0.0;
    double spread_cost = 0.0;
    double fee_cost = 0.0;
    double liquidity_score = 0.0;
    double confidence = 0.0;
    double reference_price = 0.0;
    double target_price = 0.0;
    double executable_price = 0.0;
    double yes_bid = 0.0;
    double yes_ask = 0.0;
    double no_bid = 0.0;
    double no_ask = 0.0;
    double exit_price = 0.0;
    int seconds_left = -1;
    QString underlying_symbol;
    QString side;
    QString recommendation;
    QString gate;
    QString rationale;
    QString risk_notes;
    QString rejection_reasons;
    QStringList research_drivers;
    QStringList data_sources;
    QStringList tags;
    bool is_valid = false;
    bool passes_gate = false;
    bool is_strong = false;
};

class KalshiUniversalEdgeModel {
  public:
    static QString classify_family(const openmarketterminal::services::prediction::PredictionMarket& market);
    static QString infer_time_horizon(const openmarketterminal::services::prediction::PredictionMarket& market);
    static double yes_probability(const openmarketterminal::services::prediction::PredictionMarket& market);
    static double spread_cost(const openmarketterminal::services::prediction::PredictionMarket& market,
                              double fallback);
    static double liquidity_score(double liquidity, double minimum);
    static QStringList research_drivers_for_family(const QString& family);
    static QStringList data_sources_for_family(const QString& family);

    static KalshiUniversalSignal score_market(
        const openmarketterminal::services::prediction::PredictionMarket& market,
        const KalshiUniversalOptions& options = {},
        double model_probability = -1.0,
        double confidence_override = -1.0,
        const QString& probability_source = {});

    /// Price a crypto threshold/range contract from a timestamped underlying
    /// reference. The returned market_probability is the executable ask for
    /// the selected YES/NO side, not a midpoint or last trade.
    static KalshiUniversalSignal score_crypto_target(
        const openmarketterminal::services::prediction::PredictionMarket& market,
        double reference_price,
        const KalshiUniversalOptions& options = {},
        qint64 decision_ts_ms = 0,
        const QString& probability_source = QStringLiteral("cross-exchange-spot-proxy"));

    static QVector<KalshiUniversalSignal> rank_markets(
        const QVector<openmarketterminal::services::prediction::PredictionMarket>& markets,
        const KalshiUniversalOptions& options = {});

    static QJsonObject to_json(const KalshiUniversalSignal& signal);
};

} // namespace openmarketterminal::services::edge_radar
