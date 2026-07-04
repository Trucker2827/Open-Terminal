#pragma once

#include "services/edge_radar/CryptoImpulseModel.h"
#include "services/prediction/PredictionTypes.h"

#include <QJsonObject>
#include <QString>

namespace openmarketterminal::services::edge_radar {

struct BtcFiveMinuteOptions {
    double spread_cost = 0.03;
    double fee_cost = 0.0;
    double safety_buffer = 0.02;
    double minimum_net_edge = 0.05;
    double strong_net_edge = 0.10;
    double minimum_liquidity_score = 0.30;
    double minimum_confidence = 0.55;
    double minimum_move_usd = 70.0;
    double maximum_entry_price = 0.90;
    int min_entry_seconds_left = 45;
    int max_entry_seconds_left = 180;
    int exit_before_seconds = 20;
    int primary_window_seconds = 15;
};

struct BtcFiveMinuteSignal {
    QString market_id;
    QString question;
    QString direction = QStringLiteral("flat");
    QString side;
    QString recommendation = QStringLiteral("avoid");
    QString gate = QStringLiteral("reject");
    QString rationale;
    QString risk_notes;
    QString rejection_reasons;
    QString probability_source = QStringLiteral("btc-impulse");
    double market_probability = 0.0;
    double model_probability = 0.0;
    double raw_edge = 0.0;
    double edge_after_cost = 0.0;
    double gate_edge = 0.0;
    double spread_cost = 0.0;
    double fee_cost = 0.0;
    double liquidity_score = 0.0;
    double confidence = 0.0;
    double move_usd = 0.0;
    double move_pct = 0.0;
    double velocity_usd_per_sec = 0.0;
    double latest_price = 0.0;
    qint64 latest_tick_age_ms = -1;
    int seconds_left = -1;
    bool is_valid = false;
    bool passes_gate = false;
    bool is_strong = false;
};

class BtcFiveMinuteEdgeModel {
  public:
    static bool is_btc_five_minute_market(
        const openmarketterminal::services::prediction::PredictionMarket& market);
    static QString infer_direction(const QString& text);
    static double side_probability(
        const openmarketterminal::services::prediction::PredictionMarket& market,
        const QString& direction);
    static int seconds_left(
        const openmarketterminal::services::prediction::PredictionMarket& market);

    static BtcFiveMinuteSignal score_market(
        const openmarketterminal::services::prediction::PredictionMarket& market,
        const CryptoImpulseSignal& impulse,
        const BtcFiveMinuteOptions& options = {},
        double override_market_probability = -1.0,
        double override_model_probability = -1.0);

    static BtcFiveMinuteSignal score_manual(
        const QString& direction,
        double market_probability,
        const CryptoImpulseSignal& impulse,
        const BtcFiveMinuteOptions& options = {},
        int seconds_left = -1,
        double liquidity = 0.0,
        const QString& question = {},
        const QString& market_id = {},
        double override_model_probability = -1.0);

    static QJsonObject to_json(const BtcFiveMinuteSignal& signal);

  private:
    static double model_probability_from_impulse(
        const CryptoImpulseSignal& impulse,
        const BtcFiveMinuteOptions& options,
        double* move_usd,
        double* move_pct,
        double* velocity_usd_per_sec,
        double* latest_price);
};

} // namespace openmarketterminal::services::edge_radar
