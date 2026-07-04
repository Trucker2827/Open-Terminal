#include "services/edge_radar/BtcFiveMinuteEdgeModel.h"

#include "services/edge_radar/EdgeRadarService.h"

#include <QDateTime>
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

QString normalized_text(QString value) {
    value = value.toUpper();
    value.replace('-', ' ');
    value.replace('_', ' ');
    return value;
}

bool contains_word(const QString& haystack, const QString& needle) {
    const QRegularExpression re(QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(needle)));
    return haystack.contains(re);
}

QString pct(double v) {
    return QStringLiteral("%1%").arg(v * 100.0, 0, 'f', 2);
}

double liquidity_score(double liquidity, double minimum) {
    if (liquidity <= 0.0)
        return minimum;
    return std::clamp(liquidity / 10000.0, minimum, 1.0);
}

CryptoImpulseWindow primary_window(const CryptoImpulseSignal& impulse, int preferred_seconds) {
    for (const auto& w : impulse.windows) {
        if (w.available && w.seconds == preferred_seconds)
            return w;
    }
    for (const auto& w : impulse.windows) {
        if (w.available)
            return w;
    }
    return {};
}

} // namespace

bool BtcFiveMinuteEdgeModel::is_btc_five_minute_market(const pr::PredictionMarket& market) {
    const QString text = normalized_text(
        market.question + QStringLiteral(" ") + market.description + QStringLiteral(" ") +
        market.key.market_id + QStringLiteral(" ") + market.key.event_id + QStringLiteral(" ") +
        market.tags.join(' '));
    const bool btc = text.contains(QStringLiteral("BTC")) || text.contains(QStringLiteral("BITCOIN"));
    const bool five_min = text.contains(QStringLiteral("5M")) ||
                          text.contains(QStringLiteral("5 MIN")) ||
                          text.contains(QStringLiteral("FIVE MIN")) ||
                          text.contains(QStringLiteral("UPDOWN 5M"));
    const bool directional = text.contains(QStringLiteral(" UP ")) ||
                             text.contains(QStringLiteral(" DOWN ")) ||
                             text.contains(QStringLiteral("ABOVE")) ||
                             text.contains(QStringLiteral("BELOW")) ||
                             text.contains(QStringLiteral("UP OR DOWN"));
    return btc && five_min && directional && !market.closed;
}

QString BtcFiveMinuteEdgeModel::infer_direction(const QString& text) {
    const QString t = normalized_text(text);
    if (contains_word(t, QStringLiteral("DOWN")) || contains_word(t, QStringLiteral("BELOW")) ||
        contains_word(t, QStringLiteral("UNDER")))
        return QStringLiteral("down");
    if (contains_word(t, QStringLiteral("UP")) || contains_word(t, QStringLiteral("ABOVE")) ||
        contains_word(t, QStringLiteral("OVER")))
        return QStringLiteral("up");
    return QStringLiteral("flat");
}

double BtcFiveMinuteEdgeModel::side_probability(const pr::PredictionMarket& market,
                                                const QString& direction) {
    if (market.outcomes.isEmpty())
        return 0.0;
    const QString wanted = direction.trimmed().toLower() == QStringLiteral("down")
                               ? QStringLiteral("down")
                               : QStringLiteral("up");
    for (const auto& outcome : market.outcomes) {
        const QString name = normalized_text(outcome.name);
        if (wanted == QStringLiteral("up") &&
            (contains_word(name, QStringLiteral("UP")) || contains_word(name, QStringLiteral("YES")) ||
             contains_word(name, QStringLiteral("ABOVE"))))
            return clamp01(outcome.price);
        if (wanted == QStringLiteral("down") &&
            (contains_word(name, QStringLiteral("DOWN")) || contains_word(name, QStringLiteral("NO")) ||
             contains_word(name, QStringLiteral("BELOW"))))
            return clamp01(outcome.price);
    }
    return clamp01(wanted == QStringLiteral("down") && market.outcomes.size() > 1
                       ? market.outcomes[1].price
                       : market.outcomes.first().price);
}

int BtcFiveMinuteEdgeModel::seconds_left(const pr::PredictionMarket& market) {
    if (market.end_date_iso.trimmed().isEmpty())
        return -1;
    QDateTime end = QDateTime::fromString(market.end_date_iso, Qt::ISODateWithMs);
    if (!end.isValid())
        end = QDateTime::fromString(market.end_date_iso, Qt::ISODate);
    if (!end.isValid())
        return -1;
    return static_cast<int>(QDateTime::currentDateTimeUtc().secsTo(end.toUTC()));
}

double BtcFiveMinuteEdgeModel::model_probability_from_impulse(
    const CryptoImpulseSignal& impulse,
    const BtcFiveMinuteOptions& options,
    double* move_usd,
    double* move_pct,
    double* velocity_usd_per_sec,
    double* latest_price) {
    if (move_usd) *move_usd = 0.0;
    if (move_pct) *move_pct = 0.0;
    if (velocity_usd_per_sec) *velocity_usd_per_sec = 0.0;
    if (latest_price) *latest_price = 0.0;

    const CryptoImpulseWindow w = primary_window(impulse, options.primary_window_seconds);
    if (!w.available || w.start_price <= 0.0 || w.end_price <= 0.0)
        return 0.5;

    const double usd = w.end_price - w.start_price;
    const double abs_usd = std::abs(usd);
    const double span_sec = std::max(0.001, static_cast<double>(w.end_ts_ms - w.start_ts_ms) / 1000.0);
    if (move_usd) *move_usd = usd;
    if (move_pct) *move_pct = w.move_pct;
    if (velocity_usd_per_sec) *velocity_usd_per_sec = usd / span_sec;
    if (latest_price) *latest_price = w.end_price;

    if (impulse.gate != QStringLiteral("pass") || impulse.direction == QStringLiteral("flat"))
        return 0.5;

    const double ratio = std::clamp(abs_usd / std::max(1.0, options.minimum_move_usd), 0.0, 3.0);
    const double push = std::clamp(0.08 + ratio * 0.08, 0.0, 0.32);
    return clamp01(0.5 + push);
}

BtcFiveMinuteSignal BtcFiveMinuteEdgeModel::score_market(const pr::PredictionMarket& market,
                                                         const CryptoImpulseSignal& impulse,
                                                         const BtcFiveMinuteOptions& options,
                                                         double override_market_probability,
                                                         double override_model_probability) {
    const QString direction = impulse.direction == QStringLiteral("down") ? QStringLiteral("down")
                            : impulse.direction == QStringLiteral("up") ? QStringLiteral("up")
                                                                         : QStringLiteral("flat");
    const double market_prob = direction == QStringLiteral("flat")
                                   ? std::min(side_probability(market, QStringLiteral("up")),
                                              side_probability(market, QStringLiteral("down")))
                                   : side_probability(market, direction);
    return score_manual(direction,
                        override_market_probability >= 0.0 ? override_market_probability
                                                           : market_prob,
                        impulse,
                        options,
                        seconds_left(market),
                        market.liquidity,
                        market.question,
                        market.key.market_id,
                        override_model_probability);
}

BtcFiveMinuteSignal BtcFiveMinuteEdgeModel::score_manual(const QString& direction,
                                                         double market_probability,
                                                         const CryptoImpulseSignal& impulse,
                                                         const BtcFiveMinuteOptions& options,
                                                         int seconds_left,
                                                         double liquidity,
                                                         const QString& question,
                                                         const QString& market_id,
                                                         double override_model_probability) {
    BtcFiveMinuteSignal out;
    out.market_id = market_id;
    out.question = question;
    const QString requested_direction = direction.trimmed().toLower();
    out.direction = requested_direction == QStringLiteral("down") ? QStringLiteral("down")
                  : requested_direction == QStringLiteral("up") ? QStringLiteral("up")
                                                                : QStringLiteral("flat");
    out.side = out.direction == QStringLiteral("down") ? QStringLiteral("buy down")
             : out.direction == QStringLiteral("up") ? QStringLiteral("buy up")
                                                      : QStringLiteral("no directional side");
    out.market_probability = clamp01(market_probability);
    out.spread_cost = options.spread_cost;
    out.fee_cost = options.fee_cost;
    out.liquidity_score = liquidity_score(liquidity, options.minimum_liquidity_score);
    out.seconds_left = seconds_left;
    out.latest_tick_age_ms = impulse.latest_tick_age_ms;

    double impulse_model = model_probability_from_impulse(
        impulse, options, &out.move_usd, &out.move_pct, &out.velocity_usd_per_sec, &out.latest_price);
    if (override_model_probability >= 0.0) {
        out.model_probability = clamp01(override_model_probability);
        out.probability_source = QStringLiteral("user-model");
    } else {
        out.model_probability = impulse_model;
    }

    const double abs_move_usd = std::abs(out.move_usd);
    out.confidence = std::clamp(0.35 + (abs_move_usd / std::max(1.0, options.minimum_move_usd)) * 0.25,
                                0.35, 0.82);
    if (impulse.latest_tick_age_ms > 1500)
        out.confidence *= 0.80;
    if (override_model_probability >= 0.0)
        out.confidence = std::max(out.confidence, options.minimum_confidence);

    out.raw_edge = out.model_probability - out.market_probability;
    out.edge_after_cost = out.raw_edge - options.spread_cost - options.fee_cost;
    out.gate_edge = out.edge_after_cost - options.safety_buffer;
    out.is_valid = true;

    QStringList reasons;
    if (out.direction == QStringLiteral("flat"))
        reasons << QStringLiteral("no directional BTC impulse");
    if (impulse.gate != QStringLiteral("pass"))
        reasons << QStringLiteral("BTC impulse gate rejected: %1").arg(impulse.rejection_reasons);
    if ((impulse.direction == QStringLiteral("up") || impulse.direction == QStringLiteral("down")) &&
        impulse.direction != out.direction)
        reasons << QStringLiteral("contract side does not match BTC impulse direction");
    if (abs_move_usd < options.minimum_move_usd)
        reasons << QStringLiteral("BTC move $%1 below $%2 impulse hurdle")
                       .arg(abs_move_usd, 0, 'f', 2)
                       .arg(options.minimum_move_usd, 0, 'f', 2);
    if (seconds_left >= 0 && seconds_left < options.min_entry_seconds_left)
        reasons << QStringLiteral("%1s left is below %2s entry guard")
                       .arg(seconds_left)
                       .arg(options.min_entry_seconds_left);
    if (seconds_left >= 0 && seconds_left > options.max_entry_seconds_left)
        reasons << QStringLiteral("%1s left is above %2s late-window guard")
                       .arg(seconds_left)
                       .arg(options.max_entry_seconds_left);
    if (seconds_left >= 0 && seconds_left <= options.exit_before_seconds)
        reasons << QStringLiteral("inside exit-before-close guard");
    if (out.market_probability > options.maximum_entry_price)
        reasons << QStringLiteral("entry price %1 above %2 cap")
                       .arg(pct(out.market_probability), pct(options.maximum_entry_price));
    if (out.confidence < options.minimum_confidence)
        reasons << QStringLiteral("confidence %1 below %2 gate")
                       .arg(pct(out.confidence), pct(options.minimum_confidence));
    if (out.gate_edge < options.minimum_net_edge)
        reasons << QStringLiteral("net edge %1 below %2 hurdle after safety buffer")
                       .arg(pct(out.gate_edge), pct(options.minimum_net_edge));
    if (out.liquidity_score < options.minimum_liquidity_score)
        reasons << QStringLiteral("liquidity %1 below %2 gate")
                       .arg(pct(out.liquidity_score), pct(options.minimum_liquidity_score));

    out.passes_gate = reasons.isEmpty();
    out.is_strong = out.passes_gate && out.gate_edge >= options.strong_net_edge;
    if (out.is_strong) {
        out.recommendation = QStringLiteral("strong");
        out.gate = QStringLiteral("pass");
    } else if (out.passes_gate) {
        out.recommendation = QStringLiteral("candidate");
        out.gate = QStringLiteral("pass");
    } else if (out.gate_edge >= 0.02 && abs_move_usd >= options.minimum_move_usd * 0.75) {
        out.recommendation = QStringLiteral("watch");
        out.gate = QStringLiteral("reject");
    } else {
        out.recommendation = QStringLiteral("avoid");
        out.gate = QStringLiteral("reject");
    }

    out.rejection_reasons = reasons.join(QStringLiteral("; "));
    out.risk_notes = out.rejection_reasons.isEmpty()
                         ? QStringLiteral("directional buy-only BTC 5m signal; last-second reversal and fill risk remain")
                         : out.rejection_reasons;
    out.rationale = QStringLiteral(
        "BTC 5m %1: impulse $%2 (%3), market %4, model %5, net %6, gate %7, %8s left")
        .arg(out.direction)
        .arg(out.move_usd, 0, 'f', 2)
        .arg(pct(out.move_pct / 100.0))
        .arg(pct(out.market_probability))
        .arg(pct(out.model_probability))
        .arg(pct(out.edge_after_cost))
        .arg(pct(out.gate_edge))
        .arg(out.seconds_left);
    return out;
}

QJsonObject BtcFiveMinuteEdgeModel::to_json(const BtcFiveMinuteSignal& s) {
    return QJsonObject{{"market_id", s.market_id},
                       {"question", s.question},
                       {"direction", s.direction},
                       {"side", s.side},
                       {"recommendation", s.recommendation},
                       {"gate", s.gate},
                       {"rationale", s.rationale},
                       {"risk_notes", s.risk_notes},
                       {"rejection_reasons", s.rejection_reasons},
                       {"probability_source", s.probability_source},
                       {"market_probability", s.market_probability},
                       {"model_probability", s.model_probability},
                       {"raw_edge", s.raw_edge},
                       {"edge_after_cost", s.edge_after_cost},
                       {"gate_edge", s.gate_edge},
                       {"spread_cost", s.spread_cost},
                       {"fee_cost", s.fee_cost},
                       {"liquidity_score", s.liquidity_score},
                       {"confidence", s.confidence},
                       {"move_usd", s.move_usd},
                       {"move_pct", s.move_pct},
                       {"velocity_usd_per_sec", s.velocity_usd_per_sec},
                       {"latest_price", s.latest_price},
                       {"latest_tick_age_ms", QString::number(s.latest_tick_age_ms)},
                       {"seconds_left", s.seconds_left},
                       {"is_valid", s.is_valid},
                       {"passes_gate", s.passes_gate},
                       {"is_strong", s.is_strong}};
}

} // namespace openmarketterminal::services::edge_radar
