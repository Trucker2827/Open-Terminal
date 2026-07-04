#pragma once

#include <QJsonObject>
#include <QString>

namespace openmarketterminal::services::edge_radar {

struct EdgeInputs {
    double market_probability = 0.0;
    double model_probability = 0.0;
    double spread_cost = 0.0;
    double fee_cost = 0.0;
    double liquidity_score = 0.0;
    double confidence = 0.0;
};

struct EdgeScore {
    double market_probability = 0.0;
    double model_probability = 0.0;
    double raw_edge = 0.0;
    double edge_after_cost = 0.0;
    QString side;
    QString recommendation;
    QString risk_notes;
};

class EdgeRadarService {
  public:
    static EdgeScore evaluate(const EdgeInputs& in);
    static QJsonObject to_json(const EdgeScore& score);

  private:
    static double clamp_probability(double v);
};

} // namespace openmarketterminal::services::edge_radar
