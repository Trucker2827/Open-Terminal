#include "services/edge_radar/CryptoHourlyEdgeModel.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace openmarketterminal::services::edge_radar {

namespace pr = openmarketterminal::services::prediction;

namespace {

double clamp01(double v) {
    if (!std::isfinite(v))
        return 0.0;
    return std::clamp(v, 0.0, 1.0);
}

QString normalized_text(const QString& value) {
    QString s = value.toUpper();
    s.replace('-', ' ');
    s.replace('_', ' ');
    return s;
}

bool contains_word(const QString& haystack, const QString& needle) {
    const QRegularExpression re(QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(needle)));
    return haystack.contains(re);
}

QString direction_from_text(const QString& text) {
    const QString t = normalized_text(text);
    if (contains_word(t, QStringLiteral("DOWN")) || contains_word(t, QStringLiteral("BELOW")) ||
        contains_word(t, QStringLiteral("UNDER")))
        return QStringLiteral("down");
    return QStringLiteral("up");
}

double liquidity_score(double liquidity, double minimum) {
    if (liquidity <= 0.0)
        return minimum;
    return std::clamp(liquidity / 10000.0, minimum, 1.0);
}

QString pct(double value) {
    return QStringLiteral("%1%").arg(value * 100.0, 0, 'f', 2);
}

} // namespace

QString CryptoHourlyEdgeModel::extract_symbol(const QString& text) {
    const QString t = normalized_text(text);
    struct Alias {
        const char* symbol;
        const char* alias;
    };
    static constexpr Alias aliases[] = {
        {"BTC", "BTC"},       {"BTC", "BITCOIN"},
        {"ETH", "ETH"},       {"ETH", "ETHEREUM"},
        {"SOL", "SOL"},       {"SOL", "SOLANA"},
        {"DOGE", "DOGE"},     {"DOGE", "DOGECOIN"},
        {"XRP", "XRP"},       {"XRP", "RIPPLE"},
        {"BNB", "BNB"},       {"ADA", "ADA"},
        {"AVAX", "AVAX"},     {"LINK", "LINK"},
        {"LTC", "LTC"},       {"BCH", "BCH"},
        {"HYPE", "HYPE"},     {"SUI", "SUI"},
        {"NEAR", "NEAR"},     {"ZEC", "ZEC"},
        {"TRX", "TRX"},       {"TON", "TON"},
    };
    for (const auto& a : aliases) {
        if (contains_word(t, QString::fromLatin1(a.alias)))
            return QString::fromLatin1(a.symbol);
    }
    return {};
}

QString CryptoHourlyEdgeModel::infer_direction(const QString& text) {
    return direction_from_text(text);
}

bool CryptoHourlyEdgeModel::is_hourly_market(const pr::PredictionMarket& market) {
    const QString text = normalized_text(market.question + QStringLiteral(" ") + market.key.market_id);
    const bool cadence = text.contains(QStringLiteral("1 HOUR")) ||
                         text.contains(QStringLiteral("ONE HOUR")) ||
                         text.contains(QStringLiteral("60 MIN")) ||
                         text.contains(QStringLiteral("NEXT HOUR")) ||
                         text.contains(QStringLiteral("HOURLY"));
    const bool directional = text.contains(QStringLiteral(" UP ")) ||
                             text.contains(QStringLiteral(" DOWN ")) ||
                             text.contains(QStringLiteral(" ABOVE ")) ||
                             text.contains(QStringLiteral(" BELOW ")) ||
                             text.contains(QStringLiteral(" UNDER "));
    const bool excluded = text.contains(QStringLiteral(" RANGE ")) ||
                          text.contains(QStringLiteral(" BETWEEN "));
    return cadence && directional && !excluded && !extract_symbol(text).isEmpty() && !market.closed;
}

double CryptoHourlyEdgeModel::yes_probability(const pr::PredictionMarket& market) {
    if (market.outcomes.isEmpty())
        return 0.0;
    for (const auto& outcome : market.outcomes) {
        const QString name = normalized_text(outcome.name);
        if (name == QStringLiteral("YES") || name == QStringLiteral("UP") ||
            name == QStringLiteral("ABOVE"))
            return clamp01(outcome.price);
    }
    return clamp01(market.outcomes.first().price);
}

double CryptoHourlyEdgeModel::beta_for_symbol(const QString& symbol) {
    const QString s = symbol.trimmed().toUpper();
    if (s == QStringLiteral("BTC")) return 1.0;
    if (s == QStringLiteral("ETH")) return 0.95;
    if (s == QStringLiteral("BNB")) return 0.75;
    if (s == QStringLiteral("SOL")) return 1.20;
    if (s == QStringLiteral("DOGE")) return 1.35;
    if (s == QStringLiteral("XRP")) return 1.05;
    if (s == QStringLiteral("ADA")) return 1.15;
    if (s == QStringLiteral("AVAX")) return 1.25;
    if (s == QStringLiteral("LINK")) return 1.10;
    if (s == QStringLiteral("LTC")) return 0.85;
    if (s == QStringLiteral("BCH")) return 0.90;
    if (s == QStringLiteral("HYPE")) return 1.30;
    if (s == QStringLiteral("SUI")) return 1.35;
    if (s == QStringLiteral("NEAR")) return 1.25;
    if (s == QStringLiteral("ZEC")) return 1.15;
    return 1.10;
}

CryptoHourlySignal CryptoHourlyEdgeModel::score_symbol(const QString& symbol,
                                                       double market_probability,
                                                       double btc_anchor_probability,
                                                       const CryptoHourlyOptions& options,
                                                       const QString& direction,
                                                       double liquidity,
                                                       const QString& question,
                                                       const QString& market_id) {
    CryptoHourlySignal out;
    out.symbol = symbol.trimmed().toUpper();
    out.market_id = market_id;
    out.question = question;
    out.direction = direction.trimmed().toLower() == QStringLiteral("down") ? QStringLiteral("down")
                                                                            : QStringLiteral("up");
    out.market_probability = clamp01(market_probability);
    out.btc_anchor_probability = clamp01(btc_anchor_probability);
    out.beta_to_btc = beta_for_symbol(out.symbol);
    out.liquidity_score = liquidity_score(liquidity, options.minimum_liquidity_score);
    out.is_anchor = out.symbol == QStringLiteral("BTC");
    out.anchor_move = std::abs(out.btc_anchor_probability - 0.5);
    out.total_cost = options.spread_cost + options.fee_cost;
    out.safety_buffer = options.safety_buffer;
    out.minimum_net_edge = options.minimum_net_edge;
    out.strong_net_edge = options.strong_net_edge;

    if (out.symbol.isEmpty() || out.btc_anchor_probability <= 0.0) {
        out.risk_notes = QStringLiteral("missing crypto symbol or BTC anchor probability");
        out.rejection_reasons = out.risk_notes;
        out.gate = QStringLiteral("reject");
        return out;
    }

    const double anchor_up = out.btc_anchor_probability;
    double fair_up = 0.5 + out.beta_to_btc * (anchor_up - 0.5);
    fair_up = std::clamp(fair_up, 0.08, 0.92);
    double fair = out.direction == QStringLiteral("down") ? 1.0 - fair_up : fair_up;

    if (out.is_anchor) {
        fair = out.market_probability;
        out.confidence = 0.45;
    } else {
        // Small shrink toward the market avoids treating every BTC wiggle as a
        // full altcoin signal. This keeps the model conservative until we add
        // live spot/vol/funding inputs.
        fair = 0.85 * fair + 0.15 * out.market_probability;
        out.confidence = std::clamp(0.45 + std::abs(anchor_up - 0.5) * 2.2, 0.45, 0.72);
    }
    out.model_probability = clamp01(fair);

    const EdgeScore score = EdgeRadarService::evaluate(
        {out.market_probability, out.model_probability, options.spread_cost,
         options.fee_cost, out.liquidity_score, out.confidence});
    out.raw_edge = score.raw_edge;
    out.edge_after_cost = score.edge_after_cost;
    out.gate_edge = score.edge_after_cost - options.safety_buffer;
    out.side = score.side;
    out.is_valid = true;

    QStringList reasons;
    if (!out.is_anchor) {
        if (out.gate_edge < options.minimum_net_edge)
            reasons << QStringLiteral("net edge %1 below %2 hurdle after safety buffer")
                           .arg(pct(out.gate_edge), pct(options.minimum_net_edge));
        if (out.anchor_move < options.minimum_anchor_move)
            reasons << QStringLiteral("BTC anchor move %1 below %2 hurdle")
                           .arg(pct(out.anchor_move), pct(options.minimum_anchor_move));
        if (out.liquidity_score < options.minimum_liquidity_score)
            reasons << QStringLiteral("liquidity score %1 below %2 hurdle")
                           .arg(pct(out.liquidity_score), pct(options.minimum_liquidity_score));
        if (out.total_cost > options.maximum_total_cost)
            reasons << QStringLiteral("cost stack %1 above %2 cap")
                           .arg(pct(out.total_cost), pct(options.maximum_total_cost));
    }

    out.passes_gate = !out.is_anchor && reasons.isEmpty();
    out.is_strong = out.passes_gate && out.gate_edge >= options.strong_net_edge;
    if (out.is_anchor) {
        out.recommendation = QStringLiteral("anchor");
        out.gate = QStringLiteral("anchor");
    } else if (out.is_strong) {
        out.recommendation = QStringLiteral("strong");
        out.gate = QStringLiteral("pass");
    } else if (out.passes_gate) {
        out.recommendation = QStringLiteral("candidate");
        out.gate = QStringLiteral("pass");
    } else if (out.gate_edge >= 0.02) {
        out.recommendation = QStringLiteral("watch");
        out.gate = QStringLiteral("reject");
    } else {
        out.recommendation = QStringLiteral("avoid");
        out.gate = QStringLiteral("reject");
    }
    out.rejection_reasons = reasons.join(QStringLiteral("; "));
    out.risk_notes = reasons.isEmpty() ? score.risk_notes : reasons.join(QStringLiteral("; "));

    out.rationale = QStringLiteral("%1 hourly BTC anchor %2%, beta %3, fair %4%, market %5%, net %6%, gate %7%")
        .arg(out.symbol)
        .arg(out.btc_anchor_probability * 100.0, 0, 'f', 2)
        .arg(out.beta_to_btc, 0, 'f', 2)
        .arg(out.model_probability * 100.0, 0, 'f', 2)
        .arg(out.market_probability * 100.0, 0, 'f', 2)
        .arg(out.edge_after_cost * 100.0, 0, 'f', 2)
        .arg(out.gate_edge * 100.0, 0, 'f', 2);
    return out;
}

CryptoHourlySignal CryptoHourlyEdgeModel::score_market(const pr::PredictionMarket& market,
                                                       const QVector<pr::PredictionMarket>& context,
                                                       const CryptoHourlyOptions& options) {
    double btc_anchor = 0.0;
    double best_liquidity = -1.0;
    for (const auto& candidate : context) {
        if (!is_hourly_market(candidate))
            continue;
        if (extract_symbol(candidate.question + QStringLiteral(" ") + candidate.key.market_id) != QStringLiteral("BTC"))
            continue;
        if (direction_from_text(candidate.question) != QStringLiteral("up"))
            continue;
        if (candidate.liquidity >= best_liquidity) {
            best_liquidity = candidate.liquidity;
            btc_anchor = yes_probability(candidate);
        }
    }

    const QString symbol = extract_symbol(market.question + QStringLiteral(" ") + market.key.market_id);
    return score_symbol(symbol, yes_probability(market), btc_anchor, options,
                        direction_from_text(market.question), market.liquidity,
                        market.question, market.key.market_id);
}

QVector<CryptoHourlySignal> CryptoHourlyEdgeModel::rank_markets(const QVector<pr::PredictionMarket>& markets,
                                                               const CryptoHourlyOptions& options) {
    QVector<CryptoHourlySignal> rows;
    rows.reserve(markets.size());
    for (const auto& market : markets) {
        if (!is_hourly_market(market))
            continue;
        CryptoHourlySignal s = score_market(market, markets, options);
        if (s.is_valid)
            rows.push_back(s);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const CryptoHourlySignal& a, const CryptoHourlySignal& b) {
        if (a.is_anchor != b.is_anchor)
            return !a.is_anchor;
        if (a.passes_gate != b.passes_gate)
            return a.passes_gate;
        if (a.is_strong != b.is_strong)
            return a.is_strong;
        return a.gate_edge > b.gate_edge;
    });
    return rows;
}

QJsonObject CryptoHourlyEdgeModel::to_json(const CryptoHourlySignal& s) {
    return QJsonObject{{"symbol", s.symbol},
                       {"market_id", s.market_id},
                       {"question", s.question},
                       {"direction", s.direction},
                       {"beta_to_btc", s.beta_to_btc},
                       {"btc_anchor_probability", s.btc_anchor_probability},
                       {"market_probability", s.market_probability},
                       {"model_probability", s.model_probability},
                       {"raw_edge", s.raw_edge},
                       {"edge_after_cost", s.edge_after_cost},
                       {"liquidity_score", s.liquidity_score},
                       {"confidence", s.confidence},
                       {"anchor_move", s.anchor_move},
                       {"total_cost", s.total_cost},
                       {"safety_buffer", s.safety_buffer},
                       {"gate_edge", s.gate_edge},
                       {"minimum_net_edge", s.minimum_net_edge},
                       {"strong_net_edge", s.strong_net_edge},
                       {"side", s.side},
                       {"recommendation", s.recommendation},
                       {"gate", s.gate},
                       {"rationale", s.rationale},
                       {"risk_notes", s.risk_notes},
                       {"rejection_reasons", s.rejection_reasons},
                       {"is_anchor", s.is_anchor},
                       {"passes_gate", s.passes_gate},
                       {"is_strong", s.is_strong},
                       {"is_valid", s.is_valid}};
}

} // namespace openmarketterminal::services::edge_radar
