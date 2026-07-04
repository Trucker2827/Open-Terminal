#include "services/edge_radar/EdgeRadarService.h"

#include <QStringList>
#include <QtGlobal>

#include <cmath>

namespace openmarketterminal::services::edge_radar {

double EdgeRadarService::clamp_probability(double v) {
    if (!std::isfinite(v))
        return 0.0;
    if (v > 1.0)
        v /= 100.0;
    return qBound(0.0, v, 1.0);
}

EdgeScore EdgeRadarService::evaluate(const EdgeInputs& in) {
    const double market = clamp_probability(in.market_probability);
    const double model = clamp_probability(in.model_probability);
    const double spread = clamp_probability(in.spread_cost);
    const double fee = clamp_probability(in.fee_cost);
    const double confidence = clamp_probability(in.confidence);
    const double liquidity = clamp_probability(in.liquidity_score);

    EdgeScore out;
    out.market_probability = market;
    out.model_probability = model;
    out.raw_edge = model - market;
    out.side = out.raw_edge >= 0.0 ? QStringLiteral("yes/long") : QStringLiteral("no/short");
    out.edge_after_cost = std::abs(out.raw_edge) - spread - fee;

    if (out.edge_after_cost >= 0.05 && confidence >= 0.60 && liquidity >= 0.30) {
        out.recommendation = QStringLiteral("candidate");
    } else if (out.edge_after_cost >= 0.02 && confidence >= 0.45) {
        out.recommendation = QStringLiteral("watch");
    } else {
        out.recommendation = QStringLiteral("avoid");
    }

    QStringList risks;
    if (confidence < 0.60)
        risks << QStringLiteral("low model confidence");
    if (liquidity < 0.30)
        risks << QStringLiteral("thin liquidity");
    if (spread + fee > 0.03)
        risks << QStringLiteral("costs consume edge");
    if (std::abs(out.raw_edge) < 0.02)
        risks << QStringLiteral("small probability gap");
    out.risk_notes = risks.isEmpty() ? QStringLiteral("edge clears basic cost/liquidity checks") : risks.join(QStringLiteral("; "));
    return out;
}

QJsonObject EdgeRadarService::to_json(const EdgeScore& score) {
    return QJsonObject{{"market_probability", score.market_probability},
                       {"model_probability", score.model_probability},
                       {"raw_edge", score.raw_edge},
                       {"edge_after_cost", score.edge_after_cost},
                       {"side", score.side},
                       {"recommendation", score.recommendation},
                       {"risk_notes", score.risk_notes}};
}

} // namespace openmarketterminal::services::edge_radar
