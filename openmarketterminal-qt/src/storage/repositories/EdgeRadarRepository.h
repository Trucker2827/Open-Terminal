#pragma once

#include "storage/repositories/BaseRepository.h"

#include <QJsonObject>

namespace openmarketterminal {

struct EdgeRadarIdea {
    QString id;
    QString asset_class = QStringLiteral("prediction");
    QString venue = QStringLiteral("kalshi");
    QString symbol;
    QString market_id;
    QString question;
    QString side;
    double market_probability = 0.0;
    double model_probability = 0.0;
    double spread_cost = 0.0;
    double fee_cost = 0.0;
    double liquidity_score = 0.0;
    double confidence = 0.0;
    double raw_edge = 0.0;
    double edge_after_cost = 0.0;
    QString recommendation;
    QString thesis;
    QString risk_notes;
    QString status = QStringLiteral("watching");
    QString tags;
    qint64 created_at = 0;
    qint64 updated_at = 0;
    qint64 last_evaluated_at = 0;
};

class EdgeRadarRepository : public BaseRepository<EdgeRadarIdea> {
  public:
    static EdgeRadarRepository& instance();

    Result<EdgeRadarIdea> create(const EdgeRadarIdea& in);
    Result<void> update(const EdgeRadarIdea& idea);
    Result<void> remove(const QString& id);
    Result<EdgeRadarIdea> get(const QString& id);
    Result<QVector<EdgeRadarIdea>> list_all();
    Result<QVector<EdgeRadarIdea>> list_active();

  private:
    EdgeRadarRepository() = default;
    static EdgeRadarIdea map_row(QSqlQuery& q);
};

QJsonObject edge_radar_idea_to_json(const EdgeRadarIdea& idea);

} // namespace openmarketterminal
