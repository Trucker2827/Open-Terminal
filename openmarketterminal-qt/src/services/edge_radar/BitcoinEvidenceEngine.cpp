#include "services/edge_radar/BitcoinEvidenceEngine.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

namespace openmarketterminal::services::edge_radar {
namespace {

double clamp01(double value) { return std::clamp(value, 0.0, 1.0); }

QString normalized_direction(const QString& value) {
    const QString direction = value.trimmed().toUpper();
    return direction == QStringLiteral("UP") || direction == QStringLiteral("DOWN") ||
                   direction == QStringLiteral("NEUTRAL")
        ? direction : QStringLiteral("NEUTRAL");
}

int direction_sign(const QString& value) {
    const QString direction = normalized_direction(value);
    return direction == QStringLiteral("UP") ? 1 : direction == QStringLiteral("DOWN") ? -1 : 0;
}

QSet<QString> tokens(QString text) {
    text = text.toLower();
    text.remove(QRegularExpression(QStringLiteral("[^a-z0-9 ]")));
    static const QSet<QString> stop{QStringLiteral("a"), QStringLiteral("an"), QStringLiteral("and"),
        QStringLiteral("as"), QStringLiteral("at"), QStringLiteral("for"), QStringLiteral("from"),
        QStringLiteral("in"), QStringLiteral("is"), QStringLiteral("of"), QStringLiteral("on"),
        QStringLiteral("the"), QStringLiteral("to"), QStringLiteral("with")};
    QSet<QString> out;
    for (const auto& token : text.split(QLatin1Char(' '), Qt::SkipEmptyParts))
        if (!stop.contains(token) && token.size() > 2) out.insert(token);
    return out;
}

double similarity(const QString& left, const QString& right) {
    const QSet<QString> a = tokens(left);
    const QSet<QString> b = tokens(right);
    if (a.isEmpty() || b.isEmpty()) return 0.0;
    int overlap = 0;
    for (const auto& token : a)
        if (b.contains(token)) ++overlap;
    return static_cast<double>(overlap) / (a.size() + b.size() - overlap);
}

QVector<EvidenceTimedValue> past_values(const QVector<EvidenceTimedValue>& values, qint64 cutoff) {
    QVector<EvidenceTimedValue> out;
    for (const auto& value : values)
        if (value.ts_ms > 0 && value.ts_ms <= cutoff && std::isfinite(value.value)) out.append(value);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.ts_ms < b.ts_ms; });
    return out;
}

QVector<EvidenceHistorySample> past_history(const QVector<EvidenceHistorySample>& history, qint64 cutoff) {
    QVector<EvidenceHistorySample> out;
    for (const auto& sample : history)
        if (sample.observed_at_ms > 0 && sample.observed_at_ms <= cutoff) out.append(sample);
    return out;
}

double return_pct(const QVector<EvidenceTimedValue>& values) {
    if (values.size() < 2 || values.first().value <= 0.0) return 0.0;
    return (values.last().value / values.first().value - 1.0) * 100.0;
}

double realized_volatility_pct(const QVector<EvidenceTimedValue>& values) {
    if (values.size() < 3) return 0.0;
    QVector<double> returns;
    for (int i = 1; i < values.size(); ++i) {
        if (values[i - 1].value > 0.0 && values[i].value > 0.0)
            returns.append(std::log(values[i].value / values[i - 1].value) * 100.0);
    }
    if (returns.size() < 2) return 0.0;
    const double mean = std::accumulate(returns.cbegin(), returns.cend(), 0.0) / returns.size();
    double variance = 0.0;
    for (double value : returns) variance += (value - mean) * (value - mean);
    return std::sqrt(variance / (returns.size() - 1));
}

QString time_bucket(int seconds_left) {
    if (seconds_left < 0) return QStringLiteral("UNKNOWN");
    if (seconds_left <= 120) return QStringLiteral("FINAL_2M");
    if (seconds_left <= 600) return QStringLiteral("2_10M");
    if (seconds_left <= 1800) return QStringLiteral("10_30M");
    return QStringLiteral("30M_PLUS");
}

double normal_cdf(double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }

double correlation(const QVector<QPair<double, double>>& pairs) {
    if (pairs.size() < 3) return 0.0;
    double x_mean = 0.0;
    double y_mean = 0.0;
    for (const auto& pair : pairs) { x_mean += pair.first; y_mean += pair.second; }
    x_mean /= pairs.size();
    y_mean /= pairs.size();
    double covariance = 0.0;
    double x_variance = 0.0;
    double y_variance = 0.0;
    for (const auto& pair : pairs) {
        const double x = pair.first - x_mean;
        const double y = pair.second - y_mean;
        covariance += x * y;
        x_variance += x * x;
        y_variance += y * y;
    }
    return x_variance > 0.0 && y_variance > 0.0
        ? covariance / std::sqrt(x_variance * y_variance) : 0.0;
}

QJsonObject calibration_for(const QVector<EvidenceHistorySample>& history, const QString& bucket) {
    int samples = 0;
    double brier = 0.0;
    double predicted = 0.0;
    double observed = 0.0;
    for (const auto& sample : history) {
        if (!sample.kind.isEmpty() && sample.kind != QStringLiteral("settlement")) continue;
        if (!bucket.isEmpty() && sample.time_bucket != bucket) continue;
        const double probability = clamp01(sample.predicted_probability);
        const double outcome = sample.outcome_success ? 1.0 : 0.0;
        ++samples;
        brier += (probability - outcome) * (probability - outcome);
        predicted += probability;
        observed += outcome;
    }
    return QJsonObject{{"time_bucket", bucket.isEmpty() ? QStringLiteral("ALL") : bucket},
        {"samples", samples}, {"brier", samples > 0 ? brier / samples : 0.0},
        {"mean_predicted", samples > 0 ? predicted / samples : 0.0},
        {"observed_rate", samples > 0 ? observed / samples : 0.0},
        {"calibration_error", samples > 0 ? std::abs(predicted - observed) / samples : 0.0},
        {"status", samples >= 30 ? QStringLiteral("CALIBRATED SAMPLE") : QStringLiteral("NOT ENOUGH DATA")}};
}

} // namespace

BitcoinEvidenceResult BitcoinEvidenceEngine::analyze(const BitcoinEvidenceInput& input) {
    BitcoinEvidenceResult result;
    result.as_of_ms = input.decision_ts_ms;
    const auto history = past_history(input.history, input.decision_ts_ms);
    const auto btc = past_values(input.btc_prices, input.decision_ts_ms);
    const int news_sign = direction_sign(input.news_verdict);

    // 1. Narrative impact memory: only timestamp-safe analogs sharing a catalyst and direction.
    int analogs = 0;
    double analog_return = 0.0;
    QHash<QString, int> analog_count_by_horizon;
    QHash<QString, double> analog_return_by_horizon;
    for (const auto& sample : history) {
        if (!sample.kind.isEmpty() && sample.kind != QStringLiteral("directional")) continue;
        bool catalyst_match = input.catalysts.isEmpty();
        for (const auto& catalyst : input.catalysts)
            if (sample.catalysts.contains(catalyst)) catalyst_match = true;
        if (!catalyst_match || direction_sign(sample.verdict) != news_sign) continue;
        ++analogs;
        analog_return += sample.actual_return_pct;
        ++analog_count_by_horizon[sample.horizon];
        analog_return_by_horizon[sample.horizon] += sample.actual_return_pct;
    }
    QJsonArray impact_horizons;
    for (const auto& horizon : {QStringLiteral("5M"), QStringLiteral("15M"), QStringLiteral("1H"),
                                QStringLiteral("6H"), QStringLiteral("24H")}) {
        const int count = analog_count_by_horizon.value(horizon);
        impact_horizons.append(QJsonObject{{"horizon", horizon}, {"samples", count},
            {"average_return_pct", count > 0 ? analog_return_by_horizon.value(horizon) / count : 0.0},
            {"status", count >= 10 ? QStringLiteral("USABLE") : QStringLiteral("NOT ENOUGH DATA")}});
    }
    result.impact_memory = QJsonObject{{"analog_samples", analogs},
        {"average_return_pct", analogs > 0 ? analog_return / analogs : 0.0},
        {"horizons", impact_horizons},
        {"status", analogs >= 10 ? QStringLiteral("USABLE") : QStringLiteral("NOT ENOUGH DATA")},
        {"cutoff_ms", QString::number(input.decision_ts_ms)}};

    // 2. Market reaction: the response to a story is evidence distinct from its language.
    const double reaction_return = input.reference_spot > 0.0 && input.current_spot > 0.0
        ? (input.current_spot / input.reference_spot - 1.0) * 100.0 : return_pct(btc);
    const int reaction_sign = reaction_return > 0.05 ? 1 : reaction_return < -0.05 ? -1 : 0;
    QString reaction_label = QStringLiteral("NO CLEAR REACTION");
    if (news_sign != 0 && reaction_sign == news_sign) reaction_label = QStringLiteral("CONFIRMED");
    else if (news_sign != 0 && reaction_sign == -news_sign) reaction_label = QStringLiteral("REJECTED");
    else if (news_sign < 0 && reaction_sign == 0) reaction_label = QStringLiteral("RESILIENT");
    else if (news_sign > 0 && reaction_sign == 0) reaction_label = QStringLiteral("ABSORBED");
    else if (news_sign == 0 && reaction_sign != 0) reaction_label = QStringLiteral("UNEXPLAINED MOVE");
    const double reaction_score = std::clamp(reaction_return / 0.25, -1.0, 1.0);
    result.market_reaction = QJsonObject{{"label", reaction_label},
        {"return_pct", reaction_return}, {"score", reaction_score},
        {"reference_spot", input.reference_spot}, {"current_spot", input.current_spot}};

    // 3. Surprise/novelty: new language receives weight; recycled headlines decay.
    double maximum_similarity = 0.0;
    for (const auto& current : input.current_headlines)
        for (const auto& prior : input.historical_headlines)
            maximum_similarity = std::max(maximum_similarity, similarity(current, prior));
    const double novelty_score = input.current_headlines.isEmpty() ? 0.0 : 1.0 - maximum_similarity;
    result.novelty = QJsonObject{{"score", novelty_score},
        {"maximum_historical_similarity", maximum_similarity},
        {"label", novelty_score >= 0.75 ? QStringLiteral("NEW")
            : novelty_score >= 0.40 ? QStringLiteral("DEVELOPING") : QStringLiteral("REPEATED")}};

    // 4. Regime engine: trend/mean-reversion, volatility, and liquidity are orthogonal labels.
    const double period_return = return_pct(btc);
    const double realized_vol = realized_volatility_pct(btc);
    QString regime = QStringLiteral("NOT ENOUGH DATA");
    if (btc.size() >= 6) {
        if (input.liquidity_score < 0.30) regime = QStringLiteral("LOW LIQUIDITY");
        else if (realized_vol >= 0.18) regime = QStringLiteral("HIGH VOLATILITY");
        else if (std::abs(period_return) >= std::max(0.15, realized_vol * 2.0))
            regime = period_return > 0.0 ? QStringLiteral("TRENDING UP") : QStringLiteral("TRENDING DOWN");
        else regime = QStringLiteral("MEAN REVERTING");
    }
    result.regime = QJsonObject{{"label", regime}, {"period_return_pct", period_return},
        {"realized_step_volatility_pct", realized_vol}, {"liquidity_score", input.liquidity_score},
        {"samples", btc.size()}};

    // 5. Cross-market confirmation: BTC, related assets, book pressure, and venue breadth.
    int confirming = 0;
    int opposing = 0;
    QJsonArray assets;
    const int btc_sign = period_return > 0.03 ? 1 : period_return < -0.03 ? -1 : 0;
    for (auto it = input.cross_market_prices.cbegin(); it != input.cross_market_prices.cend(); ++it) {
        const auto values = past_values(it.value(), input.decision_ts_ms);
        const double asset_return = return_pct(values);
        const int sign = asset_return > 0.03 ? 1 : asset_return < -0.03 ? -1 : 0;
        if (btc_sign != 0 && sign == btc_sign) ++confirming;
        else if (btc_sign != 0 && sign == -btc_sign) ++opposing;
        assets.append(QJsonObject{{"symbol", it.key()}, {"return_pct", asset_return}, {"samples", values.size()}});
    }
    const int book_sign = input.order_book_imbalance > 0.08 ? 1 : input.order_book_imbalance < -0.08 ? -1 : 0;
    if (btc_sign != 0 && book_sign == btc_sign) ++confirming;
    else if (btc_sign != 0 && book_sign == -btc_sign) ++opposing;
    const double cross_score = confirming + opposing > 0
        ? static_cast<double>(confirming - opposing) / (confirming + opposing) : 0.0;
    result.cross_market = QJsonObject{{"score", cross_score}, {"confirming", confirming},
        {"opposing", opposing}, {"order_book_imbalance", input.order_book_imbalance},
        {"assets", assets}, {"status", confirming + opposing > 0 ? QStringLiteral("OBSERVED") : QStringLiteral("NOT ENOUGH DATA")}};

    // 6. Kalshi reaction lag: first meaningful spot move versus first contract repricing.
    const auto probabilities = past_values(input.kalshi_probabilities, input.decision_ts_ms);
    qint64 spot_move_ts = 0;
    qint64 contract_move_ts = 0;
    if (btc.size() >= 2 && btc.first().value > 0.0) {
        for (const auto& point : btc)
            if (std::abs(point.value / btc.first().value - 1.0) >= 0.0005) { spot_move_ts = point.ts_ms; break; }
    }
    if (probabilities.size() >= 2) {
        for (const auto& point : probabilities)
            if (std::abs(point.value - probabilities.first().value) >= 0.03) { contract_move_ts = point.ts_ms; break; }
    }
    const bool lag_valid = spot_move_ts > 0 && contract_move_ts > 0;
    result.kalshi_lag = QJsonObject{{"spot_move_ts_ms", QString::number(spot_move_ts)},
        {"contract_move_ts_ms", QString::number(contract_move_ts)},
        {"lag_ms", lag_valid ? contract_move_ts - spot_move_ts : 0},
        {"label", !lag_valid ? QStringLiteral("NOT ENOUGH DATA")
            : contract_move_ts > spot_move_ts ? QStringLiteral("KALSHI LAGGED")
            : contract_move_ts < spot_move_ts ? QStringLiteral("KALSHI LED") : QStringLiteral("SIMULTANEOUS")}};

    // 7. Decision replay/counterfactuals over settled historical rows.
    double chosen_pnl = 0.0;
    double opposite_pnl = 0.0;
    double early_exit_proxy = 0.0;
    int settlement_samples = 0;
    QHash<QString, int> replay_count_by_horizon;
    QHash<QString, double> replay_return_by_horizon;
    for (const auto& sample : history) {
        if (sample.kind == QStringLiteral("directional")) {
            ++replay_count_by_horizon[sample.horizon];
            replay_return_by_horizon[sample.horizon] += sample.net_pnl;
            continue;
        }
        ++settlement_samples;
        chosen_pnl += sample.net_pnl;
        opposite_pnl -= sample.net_pnl;
        early_exit_proxy += sample.net_pnl > 0.0 ? sample.net_pnl * 0.70 : sample.net_pnl * 0.50;
    }
    QJsonArray replay_horizons;
    for (const auto& horizon : {QStringLiteral("5M"), QStringLiteral("15M"), QStringLiteral("1H"),
                                QStringLiteral("6H"), QStringLiteral("24H")}) {
        const int count = replay_count_by_horizon.value(horizon);
        replay_horizons.append(QJsonObject{{"horizon", horizon}, {"samples", count},
            {"average_counterfactual_return_pct", count > 0 ? replay_return_by_horizon.value(horizon) / count : 0.0}});
    }
    result.replay = QJsonObject{{"samples", history.size()}, {"settlement_samples", settlement_samples},
        {"horizons", replay_horizons}, {"chosen_net_pnl", chosen_pnl},
        {"opposite_side_net_pnl", opposite_pnl}, {"early_exit_proxy_pnl", early_exit_proxy},
        {"status", history.size() >= 10 ? QStringLiteral("USABLE") : QStringLiteral("NOT ENOUGH DATA")}};

    // 8. Evidence-adaptive weighting: correlations are learned from settled history only.
    const QList<QPair<QString, std::function<double(const EvidenceHistorySample&)>>> features{
        {QStringLiteral("news"), [](const auto& s) { return s.news_score / 100.0; }},
        {QStringLiteral("novelty"), [](const auto& s) { return s.novelty; }},
        {QStringLiteral("reaction"), [](const auto& s) { return s.reaction; }},
        {QStringLiteral("cross_market"), [](const auto& s) { return s.cross_confirmation; }}};
    QJsonObject weights;
    double positive_total = 0.0;
    QHash<QString, double> correlations;
    int directional_samples = 0;
    for (const auto& sample : history)
        if (sample.kind.isEmpty() || sample.kind == QStringLiteral("directional")) ++directional_samples;
    for (const auto& feature : features) {
        QVector<QPair<double, double>> pairs;
        for (const auto& sample : history) {
            if (!sample.kind.isEmpty() && sample.kind != QStringLiteral("directional")) continue;
            pairs.append({feature.second(sample), sample.outcome_up ? 1.0 : -1.0});
        }
        const double value = correlation(pairs);
        correlations.insert(feature.first, value);
        positive_total += std::max(0.0, value);
    }
    for (const auto& feature : features) {
        const double learned = directional_samples >= 20 && positive_total > 0.0
            ? std::max(0.0, correlations.value(feature.first)) / positive_total : 0.25;
        weights.insert(feature.first, learned);
    }
    result.adaptive_weights = QJsonObject{{"samples", directional_samples}, {"weights", weights},
        {"status", directional_samples >= 20 ? QStringLiteral("LEARNED") : QStringLiteral("PRIOR WEIGHTS")}};

    // 9. Abstention intelligence: refusing a losing candidate is a scored success.
    int abstained = 0;
    int avoided_losses = 0;
    double abstained_net = 0.0;
    for (const auto& sample : history) {
        if (!sample.kind.isEmpty() && sample.kind != QStringLiteral("settlement")) continue;
        if (sample.traded) continue;
        ++abstained;
        abstained_net += sample.net_pnl;
        if (sample.net_pnl < 0.0) ++avoided_losses;
    }
    result.abstention = QJsonObject{{"samples", abstained}, {"avoided_losses", avoided_losses},
        {"avoided_loss_rate", abstained > 0 ? static_cast<double>(avoided_losses) / abstained : 0.0},
        {"counterfactual_net_pnl", abstained_net},
        {"value", -std::min(0.0, abstained_net)},
        {"status", abstained >= 10 ? QStringLiteral("MEASURED") : QStringLiteral("NOT ENOUGH DATA")}};

    // 10. Confidence calibration, explicitly separated by time remaining.
    const QString bucket = time_bucket(input.seconds_left);
    QJsonObject current_calibration = calibration_for(history, bucket);
    QJsonArray bins;
    for (const auto& label : {QStringLiteral("FINAL_2M"), QStringLiteral("2_10M"),
                              QStringLiteral("10_30M"), QStringLiteral("30M_PLUS")})
        bins.append(calibration_for(history, label));
    current_calibration.insert(QStringLiteral("bins"), bins);
    current_calibration.insert(QStringLiteral("seconds_left"), input.seconds_left);

    const double seconds = std::max(1, input.seconds_left);
    const double sigma = std::max(1e-8, input.annual_volatility) *
                         std::sqrt(seconds / (365.0 * 24.0 * 60.0 * 60.0));
    const double distance_z = input.current_spot > 0.0 && input.target_price > 0.0
        ? std::abs(std::log(input.current_spot / input.target_price)) / sigma : 0.0;
    const double late_stability = input.seconds_left >= 0 ? clamp01(normal_cdf(distance_z) * 2.0 - 1.0) : 0.0;
    const double time_conditioned_confidence = clamp01(input.news_confidence * (0.55 + 0.45 * late_stability));
    current_calibration.insert(QStringLiteral("distance_volatility_units"), distance_z);
    current_calibration.insert(QStringLiteral("late_market_stability"), late_stability);
    current_calibration.insert(QStringLiteral("time_conditioned_confidence"), time_conditioned_confidence);
    result.calibration = current_calibration;

    const double learned_news = weights.value(QStringLiteral("news")).toDouble(0.25);
    const double learned_reaction = weights.value(QStringLiteral("reaction")).toDouble(0.25);
    const double learned_cross = weights.value(QStringLiteral("cross_market")).toDouble(0.25);
    const double learned_novelty = weights.value(QStringLiteral("novelty")).toDouble(0.25);
    const double composite = learned_news * (input.news_score / 100.0) +
                             learned_reaction * reaction_score + learned_cross * cross_score +
                             learned_novelty * ((novelty_score - 0.5) * 2.0);
    const double expected_edge = input.model_probability - input.market_ask - input.round_trip_cost;
    const bool enough_probability_evidence = current_calibration.value(QStringLiteral("samples")).toInt() >= 30;
    const bool enough_impact_evidence = analogs >= 10;
    const bool positive_edge = input.market_ask > 0.0 && expected_edge > 0.02;
    const bool stable_late = input.seconds_left > 120 || late_stability >= 0.70;
    result.verdict = positive_edge && enough_probability_evidence && enough_impact_evidence && stable_late
        ? QStringLiteral("TRADE CANDIDATE")
        : positive_edge && stable_late ? QStringLiteral("WATCH") : QStringLiteral("NO TRADE");
    if (!positive_edge) result.reasons.append(QStringLiteral("executable edge does not clear costs and buffer"));
    if (!enough_probability_evidence) result.reasons.append(QStringLiteral("time-bucket calibration has fewer than 30 settled samples"));
    if (!enough_impact_evidence) result.reasons.append(QStringLiteral("fewer than 10 comparable catalyst outcomes"));
    if (!stable_late) result.reasons.append(QStringLiteral("final-minutes price is too close to target for stable confidence"));
    if (reaction_label == QStringLiteral("REJECTED")) result.reasons.append(QStringLiteral("market rejected the news direction"));
    result.gate = QJsonObject{{"verdict", result.verdict}, {"composite_evidence", composite},
        {"model_probability", input.model_probability}, {"market_ask", input.market_ask},
        {"round_trip_cost", input.round_trip_cost}, {"executable_edge", expected_edge},
        {"time_bucket", bucket}, {"seconds_left", input.seconds_left},
        {"time_conditioned_confidence", time_conditioned_confidence},
        {"reasons", QJsonArray::fromStringList(result.reasons)},
        {"model_role", QStringLiteral("advisory_only")}, {"can_trigger_order", false}};
    return result;
}

QJsonObject BitcoinEvidenceEngine::to_json(const BitcoinEvidenceResult& result) {
    return QJsonObject{{"as_of_ms", QString::number(result.as_of_ms)}, {"verdict", result.verdict},
        {"reasons", QJsonArray::fromStringList(result.reasons)}, {"impact_memory", result.impact_memory},
        {"market_reaction", result.market_reaction}, {"novelty", result.novelty},
        {"regime", result.regime}, {"cross_market", result.cross_market},
        {"kalshi_lag", result.kalshi_lag}, {"decision_replay", result.replay},
        {"adaptive_weights", result.adaptive_weights}, {"abstention", result.abstention},
        {"calibration", result.calibration}, {"gate", result.gate},
        {"model_role", QStringLiteral("advisory_only")}, {"can_trigger_order", false},
        {"no_lookahead_cutoff_ms", QString::number(result.as_of_ms)}};
}

} // namespace openmarketterminal::services::edge_radar
