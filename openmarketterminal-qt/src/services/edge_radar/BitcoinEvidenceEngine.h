#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace openmarketterminal::services::edge_radar {

struct EvidenceTimedValue {
    qint64 ts_ms = 0;
    double value = 0.0;
};

struct EvidenceHistorySample {
    qint64 observed_at_ms = 0;
    QString kind;
    QString horizon;
    QString verdict;
    QString regime;
    QString time_bucket;
    QStringList catalysts;
    double predicted_probability = 0.5;
    double news_score = 0.0;
    double novelty = 0.0;
    double reaction = 0.0;
    double cross_confirmation = 0.0;
    double actual_return_pct = 0.0;
    double net_pnl = 0.0;
    bool outcome_up = false;
    bool outcome_success = false;
    bool traded = false;
};

struct BitcoinEvidenceInput {
    qint64 decision_ts_ms = 0;
    qint64 news_ts_ms = 0;
    QString news_verdict;
    QStringList catalysts;
    QStringList current_headlines;
    QStringList historical_headlines;
    double news_score = 0.0;
    double news_confidence = 0.0;
    double reference_spot = 0.0;
    double current_spot = 0.0;
    double target_price = 0.0;
    double model_probability = 0.5;
    double market_ask = 0.0;
    double round_trip_cost = 0.0;
    double order_book_imbalance = 0.0;
    double liquidity_score = 1.0;
    double annual_volatility = 0.70;
    int seconds_left = -1;
    QVector<EvidenceTimedValue> btc_prices;
    QHash<QString, QVector<EvidenceTimedValue>> cross_market_prices;
    QVector<EvidenceTimedValue> kalshi_probabilities;
    QVector<EvidenceHistorySample> history;
};

struct BitcoinEvidenceResult {
    QJsonObject impact_memory;
    QJsonObject market_reaction;
    QJsonObject novelty;
    QJsonObject regime;
    QJsonObject cross_market;
    QJsonObject kalshi_lag;
    QJsonObject replay;
    QJsonObject adaptive_weights;
    QJsonObject abstention;
    QJsonObject calibration;
    QJsonObject gate;
    QString verdict;
    QStringList reasons;
    qint64 as_of_ms = 0;
};

class BitcoinEvidenceEngine {
  public:
    static BitcoinEvidenceResult analyze(const BitcoinEvidenceInput& input);
    static QJsonObject to_json(const BitcoinEvidenceResult& result);
};

} // namespace openmarketterminal::services::edge_radar
