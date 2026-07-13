#include "storage/repositories/EdgePredictionModelRepository.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QUuid>

namespace openmarketterminal {
namespace {

const char* kObservationCols =
    "id, venue, symbol, horizon, market_id, question, direction, market_probability,"
    " btc_anchor_probability, move_5s_pct, move_15s_pct, move_60s_pct, liquidity_score,"
    " spread_cost, fee_cost, seconds_left, outcome, source, observed_at, resolved_at";

const char* kModelCols =
    "id, symbol, horizon, sample_count, positive_count, base_rate, brier_score, weights_json, trained_at";

const char* kOutputCols =
    "id, symbol, horizon, direction, readiness, source, probability, confidence,"
    " calibration_score, sample_count, as_of, trained_at";

const char* kRawTickCols =
    "id, symbol, source, price, exchange_ts, received_ts";

QString nn(const QString& s) { return s.isNull() ? QStringLiteral("") : s; }

QString model_id(const QString& symbol, const QString& horizon) {
    return symbol.trimmed().toUpper() + QStringLiteral(":") + horizon.trimmed().toLower();
}

} // namespace

EdgePredictionModelRepository& EdgePredictionModelRepository::instance() {
    static EdgePredictionModelRepository s;
    return s;
}

EdgePredictionRawTick EdgePredictionModelRepository::map_raw_tick(QSqlQuery& q) {
    EdgePredictionRawTick t;
    t.id = q.value(0).toString();
    t.symbol = q.value(1).toString();
    t.source = q.value(2).toString();
    t.price = q.value(3).toDouble();
    t.exchange_ts = q.value(4).toLongLong();
    t.received_ts = q.value(5).toLongLong();
    return t;
}

EdgePredictionObservation EdgePredictionModelRepository::map_observation(QSqlQuery& q) {
    EdgePredictionObservation o;
    o.id = q.value(0).toString();
    o.venue = q.value(1).toString();
    o.symbol = q.value(2).toString();
    o.horizon = q.value(3).toString();
    o.market_id = q.value(4).toString();
    o.question = q.value(5).toString();
    o.direction = q.value(6).toString();
    o.market_probability = q.value(7).toDouble();
    o.btc_anchor_probability = q.value(8).toDouble();
    o.move_5s_pct = q.value(9).toDouble();
    o.move_15s_pct = q.value(10).toDouble();
    o.move_60s_pct = q.value(11).toDouble();
    o.liquidity_score = q.value(12).toDouble();
    o.spread_cost = q.value(13).toDouble();
    o.fee_cost = q.value(14).toDouble();
    o.seconds_left = q.value(15).toInt();
    o.outcome = q.value(16).toInt();
    o.source = q.value(17).toString();
    o.observed_at = q.value(18).toLongLong();
    o.resolved_at = q.value(19).toLongLong();
    return o;
}

EdgePredictionModelRecord EdgePredictionModelRepository::map_model(QSqlQuery& q) {
    EdgePredictionModelRecord m;
    m.id = q.value(0).toString();
    m.symbol = q.value(1).toString();
    m.horizon = q.value(2).toString();
    m.sample_count = q.value(3).toInt();
    m.positive_count = q.value(4).toInt();
    m.base_rate = q.value(5).toDouble();
    m.brier_score = q.value(6).toDouble();
    const QJsonDocument weights = QJsonDocument::fromJson(q.value(7).toString().toUtf8());
    m.weights = weights.isObject() ? weights.object() : QJsonObject{};
    m.trained_at = q.value(8).toLongLong();
    return m;
}

EdgePredictionModelOutput EdgePredictionModelRepository::map_output(QSqlQuery& q) {
    EdgePredictionModelOutput o;
    o.id = q.value(0).toString();
    o.symbol = q.value(1).toString();
    o.horizon = q.value(2).toString();
    o.direction = q.value(3).toString();
    o.readiness = q.value(4).toString();
    o.source = q.value(5).toString();
    o.probability = q.value(6).toDouble();
    o.confidence = q.value(7).toDouble();
    o.calibration_score = q.value(8).toDouble();
    o.sample_count = q.value(9).toInt();
    o.as_of = q.value(10).toLongLong();
    o.trained_at = q.value(11).toLongLong();
    return o;
}

Result<void> EdgePredictionModelRepository::add_raw_tick(const EdgePredictionRawTick& in) {
    EdgePredictionRawTick t = in;
    if (t.id.isEmpty())
        t.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    t.symbol = t.symbol.trimmed().toUpper();
    t.received_ts = t.received_ts > 0 ? t.received_ts : QDateTime::currentMSecsSinceEpoch();
    return exec_write(
        "INSERT OR REPLACE INTO edge_prediction_raw_ticks (id, symbol, source, price, exchange_ts, received_ts)"
        " VALUES (?,?,?,?,?,?)",
        {nn(t.id), nn(t.symbol), nn(t.source), t.price, t.exchange_ts, t.received_ts});
}

Result<QVector<EdgePredictionRawTick>>
EdgePredictionModelRepository::list_raw_ticks(const QString& symbol, int limit) {
    QString sql = QStringLiteral("SELECT %1 FROM edge_prediction_raw_ticks WHERE 1=1").arg(kRawTickCols);
    QVariantList params;
    if (!symbol.trimmed().isEmpty()) {
        sql += QStringLiteral(" AND symbol=?");
        params << symbol.trimmed().toUpper();
    }
    sql += QStringLiteral(" ORDER BY received_ts DESC");
    if (limit > 0) {
        sql += QStringLiteral(" LIMIT ?");
        params << limit;
    }
    return query_list_as<EdgePredictionRawTick>(sql, params, map_raw_tick);
}

Result<QVector<EdgePredictionRawTick>>
EdgePredictionModelRepository::list_raw_ticks_by_exchange_time(const QString& symbol, int limit) {
    QString sql =
        QStringLiteral("SELECT %1 FROM edge_prediction_raw_ticks WHERE exchange_ts > 0").arg(kRawTickCols);
    QVariantList params;
    if (!symbol.trimmed().isEmpty()) {
        sql += QStringLiteral(" AND symbol=?");
        params << symbol.trimmed().toUpper();
    }
    sql += QStringLiteral(" ORDER BY exchange_ts DESC");
    if (limit > 0) {
        sql += QStringLiteral(" LIMIT ?");
        params << limit;
    }
    return query_list_as<EdgePredictionRawTick>(sql, params, map_raw_tick);
}

Result<QVector<EdgePredictionRawTick>>
EdgePredictionModelRepository::list_price_series_since(const QString& symbol, qint64 since_ms, int max_rows) {
    // One aggregated point per minute (AVG price), keyed on the minute bucket as
    // exchange_ts. Columns aliased to match map_raw_tick's positional order.
    QString sql = QStringLiteral(
        "SELECT '' AS id, symbol, 'aggregate' AS source, AVG(price) AS price, "
        "(exchange_ts/60000)*60000 AS exchange_ts, (exchange_ts/60000)*60000 AS received_ts "
        "FROM edge_prediction_raw_ticks "
        "WHERE symbol=? AND exchange_ts>=? AND exchange_ts>0 AND price>0 "
        "GROUP BY exchange_ts/60000 ORDER BY exchange_ts ASC");
    QVariantList params;
    params << symbol.trimmed().toUpper() << since_ms;
    if (max_rows > 0) {
        sql += QStringLiteral(" LIMIT ?");
        params << max_rows;
    }
    return query_list_as<EdgePredictionRawTick>(sql, params, map_raw_tick);
}

Result<QVector<EdgePredictionRawTick>>
EdgePredictionModelRepository::list_spot_price_series_since(const QString& symbol, qint64 since_ms,
                                                             int max_rows) {
    QString sql = QStringLiteral(
        "SELECT '' AS id, symbol, 'spot_aggregate' AS source, AVG(price) AS price, "
        "(exchange_ts/60000)*60000 AS exchange_ts, MAX(received_ts) AS received_ts "
        "FROM edge_prediction_raw_ticks "
        "WHERE symbol=? AND exchange_ts>=? AND exchange_ts>0 AND received_ts>0 AND price>0 "
        "AND (LOWER(source) LIKE '%coinbase%' OR LOWER(source) LIKE '%kraken%') "
        "GROUP BY exchange_ts/60000 ORDER BY exchange_ts ASC");
    QVariantList params;
    params << symbol.trimmed().toUpper() << since_ms;
    if (max_rows > 0) {
        sql += QStringLiteral(" LIMIT ?");
        params << max_rows;
    }
    return query_list_as<EdgePredictionRawTick>(sql, params, map_raw_tick);
}

Result<int> EdgePredictionModelRepository::count_raw_ticks(const QString& symbol, qint64 since_ms) {
    QString sql = QStringLiteral("SELECT COUNT(*) FROM edge_prediction_raw_ticks WHERE 1=1");
    QVariantList params;
    if (!symbol.trimmed().isEmpty()) {
        sql += QStringLiteral(" AND symbol=?");
        params << symbol.trimmed().toUpper();
    }
    if (since_ms > 0) {
        sql += QStringLiteral(" AND received_ts>=?");
        params << since_ms;
    }
    auto r = db().execute(sql, params);
    if (r.is_err())
        return Result<int>::err(r.error());
    auto& q = r.value();
    if (!q.next())
        return Result<int>::ok(0);
    return Result<int>::ok(q.value(0).toInt());
}

Result<void> EdgePredictionModelRepository::add_market_snapshot(const EdgePredictionMarketSnapshot& in) {
    EdgePredictionMarketSnapshot s = in;
    if (s.id.isEmpty())
        s.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    s.symbol = s.symbol.trimmed().toUpper();
    s.horizon = s.horizon.trimmed().toLower();
    s.observed_at = s.observed_at > 0 ? s.observed_at : QDateTime::currentMSecsSinceEpoch();
    return exec_write(
        "INSERT INTO edge_prediction_market_snapshots (id, venue, symbol, horizon, market_id, question,"
        " yes_price, no_price, spread_cost, liquidity_score, seconds_left, observed_at)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
        {nn(s.id), nn(s.venue), nn(s.symbol), nn(s.horizon), nn(s.market_id), nn(s.question),
         s.yes_price, s.no_price, s.spread_cost, s.liquidity_score, s.seconds_left, s.observed_at});
}

Result<EdgePredictionObservation>
EdgePredictionModelRepository::add_observation(const EdgePredictionObservation& in) {
    EdgePredictionObservation o = in;
    if (o.id.isEmpty())
        o.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    o.symbol = o.symbol.trimmed().toUpper();
    o.horizon = o.horizon.trimmed().toLower();
    o.observed_at = o.observed_at > 0 ? o.observed_at : QDateTime::currentMSecsSinceEpoch();
    if (o.outcome >= 0 && o.resolved_at <= 0)
        o.resolved_at = o.observed_at;

    auto r = exec_write(
        "INSERT OR REPLACE INTO edge_prediction_observations (id, venue, symbol, horizon, market_id, question,"
        " direction, market_probability, btc_anchor_probability, move_5s_pct, move_15s_pct,"
        " move_60s_pct, liquidity_score, spread_cost, fee_cost, seconds_left, outcome, source,"
        " observed_at, resolved_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {nn(o.id), nn(o.venue), nn(o.symbol), nn(o.horizon), nn(o.market_id), nn(o.question),
         nn(o.direction), o.market_probability, o.btc_anchor_probability, o.move_5s_pct,
         o.move_15s_pct, o.move_60s_pct, o.liquidity_score, o.spread_cost, o.fee_cost,
         o.seconds_left, o.outcome, nn(o.source), o.observed_at, o.resolved_at});
    if (r.is_err())
        return Result<EdgePredictionObservation>::err(r.error());
    return Result<EdgePredictionObservation>::ok(o);
}

Result<QVector<EdgePredictionObservation>>
EdgePredictionModelRepository::list_observations(const QString& symbol,
                                                 const QString& horizon,
                                                 bool resolved_only,
                                                 int limit) {
    QString sql = QStringLiteral("SELECT %1 FROM edge_prediction_observations WHERE 1=1").arg(kObservationCols);
    QVariantList params;
    if (!symbol.trimmed().isEmpty()) {
        sql += QStringLiteral(" AND symbol=?");
        params << symbol.trimmed().toUpper();
    }
    if (!horizon.trimmed().isEmpty() && horizon != QStringLiteral("all")) {
        sql += QStringLiteral(" AND horizon=?");
        params << horizon.trimmed().toLower();
    }
    if (resolved_only)
        sql += QStringLiteral(" AND outcome IN (0,1)");
    sql += QStringLiteral(" ORDER BY observed_at DESC");
    if (limit > 0) {
        sql += QStringLiteral(" LIMIT ?");
        params << limit;
    }
    return query_list(sql, params, map_observation);
}

Result<void> EdgePredictionModelRepository::resolve_observation(const QString& id, int outcome) {
    return exec_write("UPDATE edge_prediction_observations SET outcome=?, resolved_at=? WHERE id=?",
                      {outcome, QDateTime::currentMSecsSinceEpoch(), id});
}

Result<void> EdgePredictionModelRepository::upsert_model(const EdgePredictionModelRecord& model) {
    EdgePredictionModelRecord m = model;
    m.symbol = m.symbol.trimmed().toUpper();
    m.horizon = m.horizon.trimmed().toLower();
    m.id = model_id(m.symbol, m.horizon);
    m.trained_at = m.trained_at > 0 ? m.trained_at : QDateTime::currentMSecsSinceEpoch();
    const QString weights = QString::fromUtf8(QJsonDocument(m.weights).toJson(QJsonDocument::Compact));
    return exec_write(
        "INSERT INTO edge_prediction_models (id, symbol, horizon, sample_count, positive_count,"
        " base_rate, brier_score, weights_json, trained_at) VALUES (?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(id) DO UPDATE SET sample_count=excluded.sample_count,"
        " positive_count=excluded.positive_count, base_rate=excluded.base_rate,"
        " brier_score=excluded.brier_score, weights_json=excluded.weights_json,"
        " trained_at=excluded.trained_at",
        {m.id, m.symbol, m.horizon, m.sample_count, m.positive_count, m.base_rate,
         m.brier_score, weights, m.trained_at});
}

Result<EdgePredictionModelRecord>
EdgePredictionModelRepository::get_model(const QString& symbol, const QString& horizon) {
    auto rows = query_list_as<EdgePredictionModelRecord>(
        QStringLiteral("SELECT %1 FROM edge_prediction_models WHERE id=?").arg(kModelCols),
        {model_id(symbol, horizon)}, map_model);
    if (rows.is_err())
        return Result<EdgePredictionModelRecord>::err(rows.error());
    if (rows.value().isEmpty())
        return Result<EdgePredictionModelRecord>::err("Not found");
    return Result<EdgePredictionModelRecord>::ok(rows.value().first());
}

Result<QVector<EdgePredictionModelRecord>>
EdgePredictionModelRepository::list_models(const QString& symbol) {
    QString sql = QStringLiteral("SELECT %1 FROM edge_prediction_models").arg(kModelCols);
    QVariantList params;
    if (!symbol.trimmed().isEmpty()) {
        sql += QStringLiteral(" WHERE symbol=?");
        params << symbol.trimmed().toUpper();
    }
    sql += QStringLiteral(" ORDER BY horizon, trained_at DESC");
    return query_list_as<EdgePredictionModelRecord>(sql, params, map_model);
}

Result<EdgePredictionModelOutput>
EdgePredictionModelRepository::publish_model_output(const EdgePredictionModelOutput& in) {
    EdgePredictionModelOutput o = in;
    if (o.id.isEmpty())
        o.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    o.symbol = o.symbol.trimmed().toUpper();
    o.horizon = o.horizon.trimmed().toLower();
    o.as_of = o.as_of > 0 ? o.as_of : QDateTime::currentMSecsSinceEpoch();
    auto r = exec_write(
        "INSERT INTO edge_prediction_model_outputs (id, symbol, horizon, direction, readiness, source,"
        " probability, confidence, calibration_score, sample_count, as_of, trained_at)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
        {nn(o.id), nn(o.symbol), nn(o.horizon), nn(o.direction), nn(o.readiness), nn(o.source),
         o.probability, o.confidence, o.calibration_score, o.sample_count, o.as_of, o.trained_at});
    if (r.is_err())
        return Result<EdgePredictionModelOutput>::err(r.error());
    return Result<EdgePredictionModelOutput>::ok(o);
}

Result<QVector<EdgePredictionModelOutput>>
EdgePredictionModelRepository::list_model_outputs(const QString& symbol, qint64 as_of, int limit) {
    QString sql = QStringLiteral("SELECT %1 FROM edge_prediction_model_outputs WHERE 1=1").arg(kOutputCols);
    QVariantList params;
    if (!symbol.trimmed().isEmpty()) {
        sql += QStringLiteral(" AND symbol=?");
        params << symbol.trimmed().toUpper();
    }
    if (as_of > 0) {
        sql += QStringLiteral(" AND as_of<=?");
        params << as_of;
    }
    sql += QStringLiteral(" ORDER BY as_of DESC");
    if (limit > 0) {
        sql += QStringLiteral(" LIMIT ?");
        params << limit;
    }
    return query_list_as<EdgePredictionModelOutput>(sql, params, map_output);
}

Result<EdgePredictionModelOutput>
EdgePredictionModelRepository::latest_model_output(const QString& symbol,
                                                   const QString& horizon,
                                                   qint64 as_of) {
    QString sql = QStringLiteral("SELECT %1 FROM edge_prediction_model_outputs WHERE symbol=? AND horizon=?")
                      .arg(kOutputCols);
    QVariantList params{symbol.trimmed().toUpper(), horizon.trimmed().toLower()};
    if (as_of > 0) {
        sql += QStringLiteral(" AND as_of<=?");
        params << as_of;
    }
    sql += QStringLiteral(" ORDER BY as_of DESC LIMIT 1");
    auto rows = query_list_as<EdgePredictionModelOutput>(sql, params, map_output);
    if (rows.is_err())
        return Result<EdgePredictionModelOutput>::err(rows.error());
    if (rows.value().isEmpty())
        return Result<EdgePredictionModelOutput>::err("Not found");
    return Result<EdgePredictionModelOutput>::ok(rows.value().first());
}

QJsonObject edge_prediction_observation_to_json(const EdgePredictionObservation& o) {
    return QJsonObject{{"id", o.id},
                       {"venue", o.venue},
                       {"symbol", o.symbol},
                       {"horizon", o.horizon},
                       {"market_id", o.market_id},
                       {"question", o.question},
                       {"direction", o.direction},
                       {"market_probability", o.market_probability},
                       {"btc_anchor_probability", o.btc_anchor_probability},
                       {"move_5s_pct", o.move_5s_pct},
                       {"move_15s_pct", o.move_15s_pct},
                       {"move_60s_pct", o.move_60s_pct},
                       {"liquidity_score", o.liquidity_score},
                       {"spread_cost", o.spread_cost},
                       {"fee_cost", o.fee_cost},
                       {"seconds_left", o.seconds_left},
                       {"outcome", o.outcome},
                       {"source", o.source},
                       {"observed_at", QString::number(o.observed_at)},
                       {"resolved_at", QString::number(o.resolved_at)}};
}

QJsonObject edge_prediction_raw_tick_to_json(const EdgePredictionRawTick& t) {
    return QJsonObject{{"id", t.id},
                       {"symbol", t.symbol},
                       {"source", t.source},
                       {"price", t.price},
                       {"exchange_ts", QString::number(t.exchange_ts)},
                       {"received_ts", QString::number(t.received_ts)}};
}

QJsonObject edge_prediction_market_snapshot_to_json(const EdgePredictionMarketSnapshot& s) {
    return QJsonObject{{"id", s.id},
                       {"venue", s.venue},
                       {"symbol", s.symbol},
                       {"horizon", s.horizon},
                       {"market_id", s.market_id},
                       {"question", s.question},
                       {"yes_price", s.yes_price},
                       {"no_price", s.no_price},
                       {"spread_cost", s.spread_cost},
                       {"liquidity_score", s.liquidity_score},
                       {"seconds_left", s.seconds_left},
                       {"observed_at", QString::number(s.observed_at)}};
}

QJsonObject edge_prediction_model_to_json(const EdgePredictionModelRecord& m) {
    return QJsonObject{{"id", m.id},
                       {"symbol", m.symbol},
                       {"horizon", m.horizon},
                       {"sample_count", m.sample_count},
                       {"positive_count", m.positive_count},
                       {"base_rate", m.base_rate},
                       {"brier_score", m.brier_score},
                       {"weights", m.weights},
                       {"trained_at", QString::number(m.trained_at)}};
}

QJsonObject edge_prediction_model_output_to_json(const EdgePredictionModelOutput& o) {
    return QJsonObject{{"id", o.id},
                       {"symbol", o.symbol},
                       {"horizon", o.horizon},
                       {"direction", o.direction},
                       {"readiness", o.readiness},
                       {"source", o.source},
                       {"probability", o.probability},
                       {"confidence", o.confidence},
                       {"calibration_score", o.calibration_score},
                       {"sample_count", o.sample_count},
                       {"as_of", QString::number(o.as_of)},
                       {"trained_at", QString::number(o.trained_at)}};
}

} // namespace openmarketterminal
