#pragma once

#include "services/edge_radar/EdgeRadarService.h"
#include "services/prediction/PredictionTypes.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace openmarketterminal::services::edge_radar {

struct CryptoHourlyOptions {
    double spread_cost = 0.02;
    double fee_cost = 0.0175;
    double minimum_liquidity_score = 0.30;
    double minimum_net_edge = 0.05;
    double strong_net_edge = 0.08;
    double minimum_anchor_move = 0.04;
    double maximum_total_cost = 0.06;
    double safety_buffer = 0.01;
};

struct CryptoHourlySignal {
    QString symbol;
    QString market_id;
    QString question;
    QString direction = QStringLiteral("up");
    double beta_to_btc = 1.0;
    double btc_anchor_probability = 0.0;
    double market_probability = 0.0;
    double model_probability = 0.0;
    double raw_edge = 0.0;
    double edge_after_cost = 0.0;
    double liquidity_score = 0.0;
    double confidence = 0.0;
    double anchor_move = 0.0;
    double total_cost = 0.0;
    double safety_buffer = 0.0;
    double gate_edge = 0.0;
    double minimum_net_edge = 0.0;
    double strong_net_edge = 0.0;
    QString side;
    QString recommendation;
    QString gate;
    QString rationale;
    QString risk_notes;
    QString rejection_reasons;
    bool is_valid = false;
    bool is_anchor = false;
    bool passes_gate = false;
    bool is_strong = false;
};

class CryptoHourlyEdgeModel {
  public:
    static QString extract_symbol(const QString& text);
    static QString infer_direction(const QString& text);
    static bool is_hourly_market(const openmarketterminal::services::prediction::PredictionMarket& market);
    static double yes_probability(const openmarketterminal::services::prediction::PredictionMarket& market);
    static double beta_for_symbol(const QString& symbol);

    static CryptoHourlySignal score_symbol(const QString& symbol,
                                           double market_probability,
                                           double btc_anchor_probability,
                                           const CryptoHourlyOptions& options = {},
                                           const QString& direction = QStringLiteral("up"),
                                           double liquidity = 0.0,
                                           const QString& question = {},
                                           const QString& market_id = {});

    static CryptoHourlySignal score_market(
        const openmarketterminal::services::prediction::PredictionMarket& market,
        const QVector<openmarketterminal::services::prediction::PredictionMarket>& context,
        const CryptoHourlyOptions& options = {});

    static QVector<CryptoHourlySignal> rank_markets(
        const QVector<openmarketterminal::services::prediction::PredictionMarket>& markets,
        const CryptoHourlyOptions& options = {});

    static QJsonObject to_json(const CryptoHourlySignal& signal);
};

} // namespace openmarketterminal::services::edge_radar
