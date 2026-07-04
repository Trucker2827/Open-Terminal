#include "storage/repositories/EdgeRadarRepository.h"

#include <QDateTime>
#include <QJsonObject>
#include <QUuid>

namespace openmarketterminal {
namespace {

const char* kCols =
    "id, asset_class, venue, symbol, market_id, question, side,"
    " market_probability, model_probability, spread_cost, fee_cost,"
    " liquidity_score, confidence, raw_edge, edge_after_cost, recommendation,"
    " thesis, risk_notes, status, tags, created_at, updated_at, last_evaluated_at";

QString nn(const QString& s) { return s.isNull() ? QString::fromLatin1("") : s; }

} // namespace

EdgeRadarRepository& EdgeRadarRepository::instance() {
    static EdgeRadarRepository s;
    return s;
}

EdgeRadarIdea EdgeRadarRepository::map_row(QSqlQuery& q) {
    EdgeRadarIdea i;
    i.id = q.value(0).toString();
    i.asset_class = q.value(1).toString();
    i.venue = q.value(2).toString();
    i.symbol = q.value(3).toString();
    i.market_id = q.value(4).toString();
    i.question = q.value(5).toString();
    i.side = q.value(6).toString();
    i.market_probability = q.value(7).toDouble();
    i.model_probability = q.value(8).toDouble();
    i.spread_cost = q.value(9).toDouble();
    i.fee_cost = q.value(10).toDouble();
    i.liquidity_score = q.value(11).toDouble();
    i.confidence = q.value(12).toDouble();
    i.raw_edge = q.value(13).toDouble();
    i.edge_after_cost = q.value(14).toDouble();
    i.recommendation = q.value(15).toString();
    i.thesis = q.value(16).toString();
    i.risk_notes = q.value(17).toString();
    i.status = q.value(18).toString();
    i.tags = q.value(19).toString();
    i.created_at = q.value(20).toLongLong();
    i.updated_at = q.value(21).toLongLong();
    i.last_evaluated_at = q.value(22).toLongLong();
    return i;
}

Result<EdgeRadarIdea> EdgeRadarRepository::create(const EdgeRadarIdea& in) {
    EdgeRadarIdea idea = in;
    if (idea.id.isEmpty())
        idea.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    idea.created_at = idea.created_at > 0 ? idea.created_at : now;
    idea.updated_at = now;
    idea.last_evaluated_at = idea.last_evaluated_at > 0 ? idea.last_evaluated_at : now;
    auto r = exec_write(
        "INSERT INTO edge_radar_ideas (id, asset_class, venue, symbol, market_id, question, side,"
        " market_probability, model_probability, spread_cost, fee_cost, liquidity_score, confidence,"
        " raw_edge, edge_after_cost, recommendation, thesis, risk_notes, status, tags,"
        " created_at, updated_at, last_evaluated_at)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {nn(idea.id), nn(idea.asset_class), nn(idea.venue), nn(idea.symbol), nn(idea.market_id),
         nn(idea.question), nn(idea.side), idea.market_probability, idea.model_probability,
         idea.spread_cost, idea.fee_cost, idea.liquidity_score, idea.confidence, idea.raw_edge,
         idea.edge_after_cost, nn(idea.recommendation), nn(idea.thesis), nn(idea.risk_notes),
         nn(idea.status), nn(idea.tags), idea.created_at, idea.updated_at, idea.last_evaluated_at});
    if (r.is_err())
        return Result<EdgeRadarIdea>::err(r.error());
    return get(idea.id);
}

Result<void> EdgeRadarRepository::update(const EdgeRadarIdea& idea) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    return exec_write(
        "UPDATE edge_radar_ideas SET asset_class=?, venue=?, symbol=?, market_id=?, question=?, side=?,"
        " market_probability=?, model_probability=?, spread_cost=?, fee_cost=?, liquidity_score=?,"
        " confidence=?, raw_edge=?, edge_after_cost=?, recommendation=?, thesis=?, risk_notes=?,"
        " status=?, tags=?, updated_at=?, last_evaluated_at=? WHERE id=?",
        {nn(idea.asset_class), nn(idea.venue), nn(idea.symbol), nn(idea.market_id), nn(idea.question),
         nn(idea.side), idea.market_probability, idea.model_probability, idea.spread_cost,
         idea.fee_cost, idea.liquidity_score, idea.confidence, idea.raw_edge, idea.edge_after_cost,
         nn(idea.recommendation), nn(idea.thesis), nn(idea.risk_notes), nn(idea.status), nn(idea.tags),
         now, idea.last_evaluated_at > 0 ? idea.last_evaluated_at : now, nn(idea.id)});
}

Result<void> EdgeRadarRepository::remove(const QString& id) {
    return exec_write("DELETE FROM edge_radar_ideas WHERE id=?", {id});
}

Result<EdgeRadarIdea> EdgeRadarRepository::get(const QString& id) {
    return query_one(QString("SELECT %1 FROM edge_radar_ideas WHERE id=?").arg(kCols), {id}, map_row);
}

Result<QVector<EdgeRadarIdea>> EdgeRadarRepository::list_all() {
    return query_list(QString("SELECT %1 FROM edge_radar_ideas ORDER BY updated_at DESC").arg(kCols), {}, map_row);
}

Result<QVector<EdgeRadarIdea>> EdgeRadarRepository::list_active() {
    return query_list(QString("SELECT %1 FROM edge_radar_ideas WHERE status <> 'closed' ORDER BY updated_at DESC").arg(kCols), {}, map_row);
}

QJsonObject edge_radar_idea_to_json(const EdgeRadarIdea& i) {
    return QJsonObject{{"id", i.id},
                       {"asset_class", i.asset_class},
                       {"venue", i.venue},
                       {"symbol", i.symbol},
                       {"market_id", i.market_id},
                       {"question", i.question},
                       {"side", i.side},
                       {"market_probability", i.market_probability},
                       {"model_probability", i.model_probability},
                       {"spread_cost", i.spread_cost},
                       {"fee_cost", i.fee_cost},
                       {"liquidity_score", i.liquidity_score},
                       {"confidence", i.confidence},
                       {"raw_edge", i.raw_edge},
                       {"edge_after_cost", i.edge_after_cost},
                       {"recommendation", i.recommendation},
                       {"thesis", i.thesis},
                       {"risk_notes", i.risk_notes},
                       {"status", i.status},
                       {"tags", i.tags},
                       {"created_at", QString::number(i.created_at)},
                       {"updated_at", QString::number(i.updated_at)},
                       {"last_evaluated_at", QString::number(i.last_evaluated_at)}};
}

} // namespace openmarketterminal
