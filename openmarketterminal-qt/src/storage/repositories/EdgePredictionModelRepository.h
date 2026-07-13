#pragma once

#include "storage/repositories/BaseRepository.h"

#include <QJsonObject>
#include <QVector>

namespace openmarketterminal {

struct EdgePredictionObservation {
    QString id;
    QString venue = QStringLiteral("polymarket");
    QString symbol = QStringLiteral("BTC");
    QString horizon = QStringLiteral("5m");
    QString market_id;
    QString question;
    QString direction = QStringLiteral("up");
    double market_probability = 0.0;
    double btc_anchor_probability = 0.5;
    double move_5s_pct = 0.0;
    double move_15s_pct = 0.0;
    double move_60s_pct = 0.0;
    double liquidity_score = 0.0;
    double spread_cost = 0.0;
    double fee_cost = 0.0;
    int seconds_left = -1;
    int outcome = -1;
    QString source = QStringLiteral("cli");
    qint64 observed_at = 0;
    qint64 resolved_at = 0;
};

struct EdgePredictionRawTick {
    QString id;
    QString symbol = QStringLiteral("BTC");
    QString source;
    double price = 0.0;
    qint64 exchange_ts = 0;
    qint64 received_ts = 0;
};

struct EdgePredictionMarketSnapshot {
    QString id;
    QString venue = QStringLiteral("polymarket");
    QString symbol = QStringLiteral("BTC");
    QString horizon = QStringLiteral("5m");
    QString market_id;
    QString question;
    double yes_price = 0.0;
    double no_price = 0.0;
    double spread_cost = 0.0;
    double liquidity_score = 0.0;
    int seconds_left = -1;
    qint64 observed_at = 0;
};

struct EdgePredictionModelRecord {
    QString id;
    QString symbol = QStringLiteral("BTC");
    QString horizon = QStringLiteral("5m");
    int sample_count = 0;
    int positive_count = 0;
    double base_rate = 0.5;
    double brier_score = 0.0;
    QJsonObject weights;
    qint64 trained_at = 0;
};

struct EdgePredictionModelOutput {
    QString id;
    QString symbol = QStringLiteral("BTC");
    QString horizon = QStringLiteral("5m");
    QString direction = QStringLiteral("flat");
    QString readiness = QStringLiteral("collecting");
    QString source = QStringLiteral("local");
    double probability = 0.5;
    double confidence = 0.0;
    double calibration_score = 0.0;
    int sample_count = 0;
    qint64 as_of = 0;
    qint64 trained_at = 0;
};

class EdgePredictionModelRepository : public BaseRepository<EdgePredictionObservation> {
  public:
    static EdgePredictionModelRepository& instance();

    Result<void> add_raw_tick(const EdgePredictionRawTick& tick);
    Result<QVector<EdgePredictionRawTick>> list_raw_ticks(const QString& symbol = {}, int limit = 100);
    // Most-recent ticks by MARKET time (exchange_ts), for forecasters that need a
    // real historical series — backfilled ticks share one received_ts, so the
    // default received_ts ordering hides imported history.
    Result<QVector<EdgePredictionRawTick>> list_raw_ticks_by_exchange_time(const QString& symbol, int limit);
    // Per-minute downsampled price series since a market time (exchange_ts). Live
    // ticks are dense (~hundreds/min), so a raw-tick count cap can't span the days
    // a long-horizon forecast needs; aggregating to one point per minute decouples
    // context length from tick density.
    Result<QVector<EdgePredictionRawTick>> list_price_series_since(const QString& symbol, qint64 since_ms,
                                                                  int max_rows = 300000);
    // Per-minute series restricted to independent spot exchanges. Derivatives,
    // settlement indices, and synthetic feeds must not contaminate realized
    // volatility or empirical-return estimates used for Kalshi pricing.
    Result<QVector<EdgePredictionRawTick>> list_spot_price_series_since(
        const QString& symbol, qint64 since_ms, int max_rows = 300000);
    Result<int> count_raw_ticks(const QString& symbol = {}, qint64 since_ms = 0);
    Result<void> add_market_snapshot(const EdgePredictionMarketSnapshot& snapshot);

    Result<EdgePredictionObservation> add_observation(const EdgePredictionObservation& in);
    Result<QVector<EdgePredictionObservation>> list_observations(const QString& symbol,
                                                                 const QString& horizon,
                                                                 bool resolved_only,
                                                                 int limit);
    Result<void> resolve_observation(const QString& id, int outcome);

    Result<void> upsert_model(const EdgePredictionModelRecord& model);
    Result<EdgePredictionModelRecord> get_model(const QString& symbol, const QString& horizon);
    Result<QVector<EdgePredictionModelRecord>> list_models(const QString& symbol = {});

    Result<EdgePredictionModelOutput> publish_model_output(const EdgePredictionModelOutput& output);
    Result<QVector<EdgePredictionModelOutput>> list_model_outputs(const QString& symbol,
                                                                  qint64 as_of = 0,
                                                                  int limit = 100);
    Result<EdgePredictionModelOutput> latest_model_output(const QString& symbol,
                                                          const QString& horizon,
                                                          qint64 as_of);

  private:
    EdgePredictionModelRepository() = default;
    static EdgePredictionRawTick map_raw_tick(QSqlQuery& q);
    static EdgePredictionObservation map_observation(QSqlQuery& q);
    static EdgePredictionModelRecord map_model(QSqlQuery& q);
    static EdgePredictionModelOutput map_output(QSqlQuery& q);
};

QJsonObject edge_prediction_observation_to_json(const EdgePredictionObservation& o);
QJsonObject edge_prediction_raw_tick_to_json(const EdgePredictionRawTick& t);
QJsonObject edge_prediction_market_snapshot_to_json(const EdgePredictionMarketSnapshot& s);
QJsonObject edge_prediction_model_to_json(const EdgePredictionModelRecord& m);
QJsonObject edge_prediction_model_output_to_json(const EdgePredictionModelOutput& o);

} // namespace openmarketterminal
