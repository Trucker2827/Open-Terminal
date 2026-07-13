#include "services/edge_radar/KalshiAutoEngine.h"

#include <QDateTime>
#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace openmarketterminal::services::edge_radar {

namespace pr = openmarketterminal::services::prediction;

namespace {

double number(const QVariant& value) {
    bool ok = false;
    const double out = value.toString().toDouble(&ok);
    return ok ? out : value.toDouble();
}

double clamp_probability(double value) {
    return std::clamp(value, 0.001, 0.999);
}

double normal_cdf(double z) {
    return 0.5 * std::erfc(-z / std::sqrt(2.0));
}

double seconds_per_year() {
    return 365.0 * 24.0 * 60.0 * 60.0;
}

double horizon_weight(const QString& horizon) {
    const QString h = horizon.trimmed().toLower();
    if (h == QStringLiteral("5m")) return 0.10;
    if (h == QStringLiteral("15m")) return 0.25;
    if (h == QStringLiteral("1h")) return 0.45;
    if (h == QStringLiteral("1d") || h == QStringLiteral("daily")) return 0.20;
    return 0.0;
}

double context_up_probability(const KalshiAutoContext& context) {
    double weighted = 0.0;
    double weights = 0.0;
    for (const auto& signal : context.horizon_signals) {
        if (signal.observed_at_ms <= 0 || signal.observed_at_ms > context.decision_ts_ms)
            continue;
        const qint64 age = context.decision_ts_ms - signal.observed_at_ms;
        if (age > 2 * 60 * 60 * 1000)
            continue;
        const double weight = horizon_weight(signal.horizon) * std::clamp(signal.confidence, 0.0, 1.0);
        weighted += clamp_probability(signal.up_probability) * weight;
        weights += weight;
    }
    return weights > 0.0 ? weighted / weights : 0.5;
}

void attach_context_audit(KalshiSurfacePoint& point, const KalshiAutoContext& context) {
    qint64 freshest = -1;
    int samples = 0;
    double calibration = 0.0;
    double calibration_weight = 0.0;
    QStringList sources;
    for (const auto& signal : context.horizon_signals) {
        if (signal.observed_at_ms <= 0 || signal.observed_at_ms > context.decision_ts_ms)
            continue;
        const qint64 age = context.decision_ts_ms - signal.observed_at_ms;
        if (age > 2 * 60 * 60 * 1000)
            continue;
        freshest = freshest < 0 ? age : std::min(freshest, age);
        samples += std::max(0, signal.sample_count);
        if (signal.calibration_score > 0.0) {
            const double weight = std::max(1, signal.sample_count);
            calibration += signal.calibration_score * weight;
            calibration_weight += weight;
        }
        if (!signal.source.isEmpty() && !sources.contains(signal.source))
            sources.append(signal.source);
    }
    point.context_freshness_ms = freshest;
    point.context_sample_count = samples;
    point.context_calibration_score = calibration_weight > 0.0
        ? calibration / calibration_weight : 0.0;
    point.context_sources = sources.join(QStringLiteral(", "));
}

QString contract_kind(const pr::PredictionMarket& market, double* floor, double* cap) {
    *floor = number(market.extras.value(QStringLiteral("floor_strike")));
    *cap = number(market.extras.value(QStringLiteral("cap_strike")));
    if (*floor > 0.0 && *cap > *floor)
        return QStringLiteral("range");
    if (*floor > 0.0)
        return QStringLiteral("above");
    if (*cap > 0.0)
        return QStringLiteral("below");
    return {};
}

QString market_cadence(const pr::PredictionMarket& market, int seconds_left) {
    const QString description = QStringList{
        market.key.market_id,
        market.key.event_id,
        market.question,
        market.category,
        market.extras.value(QStringLiteral("series_ticker")).toString(),
        market.extras.value(QStringLiteral("frequency")).toString(),
        market.extras.value(QStringLiteral("series_frequency")).toString()
    }.join(QLatin1Char(' ')).toLower();
    if (description.contains(QStringLiteral("15m")) ||
        description.contains(QStringLiteral("15 min")) ||
        description.contains(QStringLiteral("fifteen_min")))
        return QStringLiteral("15m");
    if (description.contains(QStringLiteral("hourly")) ||
        description.contains(QStringLiteral("1 hour")) ||
        description.contains(QStringLiteral("1h")))
        return QStringLiteral("1h");
    if (description.contains(QStringLiteral("daily")) ||
        description.contains(QStringLiteral("1 day")) ||
        description.contains(QStringLiteral("1d")))
        return QStringLiteral("1d");
    return seconds_left <= 15 * 60 ? QStringLiteral("15m")
        : seconds_left <= 65 * 60 ? QStringLiteral("1h") : QStringLiteral("1d");
}

double outcome_price(const pr::PredictionMarket& market, const QString& side) {
    for (const auto& outcome : market.outcomes) {
        if (outcome.name.compare(side, Qt::CaseInsensitive) == 0)
            return std::clamp(outcome.price, 0.0, 1.0);
    }
    return 0.0;
}

double fee_per_contract(const pr::PredictionMarket& market, double price, qint64 decision_ts_ms) {
    if (price <= 0.0 || price >= 1.0)
        return 0.0;
    const QDateTime waiver = QDateTime::fromString(
        market.extras.value(QStringLiteral("fee_waiver_expiration_time")).toString(), Qt::ISODate);
    if (waiver.isValid() && waiver.toMSecsSinceEpoch() > decision_ts_ms)
        return 0.0;
    if (market.extras.value(QStringLiteral("fee_type")).toString().compare(
            QStringLiteral("none"), Qt::CaseInsensitive) == 0)
        return 0.0;
    const double multiplier = market.extras.contains(QStringLiteral("fee_multiplier"))
        ? std::max(0.0, number(market.extras.value(QStringLiteral("fee_multiplier")))) : 1.0;
    return 0.07 * multiplier * price * (1.0 - price);
}

struct Quote {
    double bid = 0.0;
    double ask = 0.0;
    double depth = 0.0;
    qint64 observed_at_ms = 0;
};

Quote quote_for(const pr::PredictionMarket& market,
                const QHash<QString, pr::PredictionOrderBook>& books,
                const QString& side) {
    Quote quote;
    quote.bid = outcome_price(market, side);
    quote.ask = number(market.extras.value(side.toLower() + QStringLiteral("_ask_dollars")));
    const QString asset = market.key.market_id + QLatin1Char(':') + side.toLower();
    const auto it = books.constFind(asset);
    if (it != books.constEnd()) {
        quote.observed_at_ms = it->last_update_ms;
        if (!it->bids.isEmpty()) quote.bid = it->bids.first().price;
        if (!it->asks.isEmpty()) {
            quote.ask = it->asks.first().price;
            quote.depth = it->asks.first().size;
        }
    }
    if (quote.ask <= 0.0) {
        const double opposite_bid = outcome_price(market,
            side.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0
                ? QStringLiteral("no") : QStringLiteral("yes"));
        if (opposite_bid > 0.0)
            quote.ask = 1.0 - opposite_bid;
    }
    return quote;
}

void enforce_monotone_above(QVector<KalshiSurfacePoint>& points) {
    QVector<int> indices;
    for (int i = 0; i < points.size(); ++i)
        if (points[i].kind == QStringLiteral("above") && points[i].valid &&
            points[i].seconds_left > 0)
            indices.append(i);
    std::sort(indices.begin(), indices.end(), [&points](int a, int b) {
        return points[a].floor < points[b].floor;
    });
    if (indices.size() < 2)
        return;

    struct Block { int begin = 0; int end = 0; double mean = 0.0; };
    QVector<Block> blocks;
    for (int i = 0; i < indices.size(); ++i) {
        blocks.append(Block{i, i, points[indices[i]].fair_yes});
        while (blocks.size() >= 2 && blocks[blocks.size() - 2].mean < blocks.last().mean) {
            const Block right = blocks.takeLast();
            Block& left = blocks.last();
            const int left_count = left.end - left.begin + 1;
            const int right_count = right.end - right.begin + 1;
            left.mean = (left.mean * left_count + right.mean * right_count) /
                        (left_count + right_count);
            left.end = right.end;
        }
    }
    for (const auto& block : blocks) {
        for (int i = block.begin; i <= block.end; ++i) {
            points[indices[i]].fair_yes = clamp_probability(block.mean);
            points[indices[i]].fair_no = 1.0 - points[indices[i]].fair_yes;
        }
    }
}

void fit_monotone_market_curve(QVector<KalshiSurfacePoint>& points) {
    QVector<int> indices;
    for (int i = 0; i < points.size(); ++i) {
        points[i].market_curve_probability = points[i].market_implied_probability;
        if (points[i].kind == QStringLiteral("above") && points[i].valid &&
            points[i].seconds_left > 0)
            indices.append(i);
    }
    std::sort(indices.begin(), indices.end(), [&points](int a, int b) {
        return points[a].floor < points[b].floor;
    });
    if (indices.size() < 2)
        return;
    struct Block { int begin = 0; int end = 0; double mean = 0.0; };
    QVector<Block> blocks;
    for (int i = 0; i < indices.size(); ++i) {
        blocks.append(Block{i, i, points[indices[i]].market_implied_probability});
        while (blocks.size() >= 2 && blocks[blocks.size() - 2].mean < blocks.last().mean) {
            const Block right = blocks.takeLast();
            Block& left = blocks.last();
            const int left_count = left.end - left.begin + 1;
            const int right_count = right.end - right.begin + 1;
            left.mean = (left.mean * left_count + right.mean * right_count) /
                        (left_count + right_count);
            left.end = right.end;
        }
    }
    for (const auto& block : blocks)
        for (int i = block.begin; i <= block.end; ++i)
            points[indices[i]].market_curve_probability = clamp_probability(block.mean);
}

bool cross_horizon_consistent(const KalshiAutoContext& context, QString* reason) {
    bool bullish = false;
    bool bearish = false;
    for (const auto& signal : context.horizon_signals) {
        if (signal.observed_at_ms <= 0 || signal.observed_at_ms > context.decision_ts_ms ||
            context.decision_ts_ms - signal.observed_at_ms > 2 * 60 * 60 * 1000 ||
            signal.confidence < 0.60)
            continue;
        bullish = bullish || signal.up_probability >= 0.58;
        bearish = bearish || signal.up_probability <= 0.42;
    }
    if (bullish && bearish) {
        if (reason) *reason = QStringLiteral("fresh high-confidence horizons disagree");
        return false;
    }
    return true;
}

bool yes_wins(const KalshiSurfacePoint& point, double settlement) {
    if (point.kind == QStringLiteral("above")) return settlement >= point.floor;
    if (point.kind == QStringLiteral("below")) return settlement < point.cap;
    if (point.kind == QStringLiteral("range")) return settlement >= point.floor && settlement < point.cap;
    return false;
}

double leg_pnl(const KalshiPortfolioLeg& leg, double settlement) {
    KalshiSurfacePoint point;
    point.kind = leg.kind;
    point.floor = leg.floor;
    point.cap = leg.cap;
    const bool outcome_yes = yes_wins(point, settlement);
    const bool selected_wins = leg.side == QStringLiteral("yes") ? outcome_yes : !outcome_yes;
    const double payout = selected_wins ? leg.contracts : 0.0;
    return payout - leg.entry_price * leg.contracts - leg.entry_fee;
}

QVector<double> settlement_scenarios(const QVector<KalshiSurfacePoint>& surface,
                                     const KalshiAutoContext& context) {
    QVector<double> scenarios;
    double minimum = context.spot * 0.95;
    double maximum = context.spot * 1.05;
    for (const auto& point : surface) {
        if (point.floor > 0.0) {
            scenarios << point.floor - 0.01 << point.floor << point.floor + 0.01;
            minimum = std::min(minimum, point.floor * 0.995);
            maximum = std::max(maximum, point.floor * 1.005);
        }
        if (point.cap > 0.0) {
            scenarios << point.cap - 0.01 << point.cap << point.cap + 0.01;
            minimum = std::min(minimum, point.cap * 0.995);
            maximum = std::max(maximum, point.cap * 1.005);
        }
    }
    for (int i = 0; i <= 20; ++i)
        scenarios << minimum + (maximum - minimum) * i / 20.0;
    std::sort(scenarios.begin(), scenarios.end());
    scenarios.erase(std::unique(scenarios.begin(), scenarios.end(), [](double a, double b) {
        return std::abs(a - b) < 1e-6;
    }), scenarios.end());
    return scenarios;
}

void recompute_plan(KalshiPortfolioPlan& plan, const QVector<double>& scenarios) {
    plan.payoff_curve.clear();
    plan.worst_case_pnl = std::numeric_limits<double>::infinity();
    plan.best_case_pnl = -std::numeric_limits<double>::infinity();
    for (double settlement : scenarios) {
        double pnl = 0.0;
        for (const auto& leg : plan.legs)
            pnl += leg_pnl(leg, settlement);
        plan.payoff_curve.append(KalshiPayoffPoint{settlement, pnl});
        plan.worst_case_pnl = std::min(plan.worst_case_pnl, pnl);
        plan.best_case_pnl = std::max(plan.best_case_pnl, pnl);
    }
    if (plan.payoff_curve.isEmpty())
        plan.worst_case_pnl = plan.best_case_pnl = 0.0;
}

QJsonObject point_json(const KalshiSurfacePoint& point) {
    return QJsonObject{{"ticker", point.ticker}, {"event_ticker", point.event_ticker},
                       {"question", point.question}, {"kind", point.kind},
                       {"cadence", point.cadence},
                       {"settlement_ts_ms", QString::number(point.settlement_ts_ms)},
                       {"model_version", QString::fromLatin1(kKalshiSettlementModelVersion)},
                       {"floor", point.floor}, {"cap", point.cap},
                       {"fair_yes", point.fair_yes}, {"fair_no", point.fair_no},
                       {"model_probability_raw", point.model_probability_raw},
                       {"calibrated_probability", point.calibrated_probability},
                       {"market_implied_probability", point.market_implied_probability},
                       {"market_curve_probability", point.market_curve_probability},
                       {"relative_value_residual", point.relative_value_residual},
                       {"model_weight", point.model_weight},
                       {"yes_bid", point.yes_bid}, {"yes_ask", point.yes_ask},
                       {"no_bid", point.no_bid}, {"no_ask", point.no_ask},
                       {"selected_side", point.selected_side},
                       {"selected_fair", point.selected_fair},
                       {"selected_ask", point.selected_ask},
                       {"selected_bid", point.selected_bid},
                       {"quote_observed_at_ms", QString::number(point.quote_observed_at_ms)},
                       {"fee_per_contract", point.fee_per_contract},
                       {"net_edge", point.net_edge}, {"time_risk_score", point.time_risk_score},
                       {"directional_delta", point.directional_delta},
                       {"confidence", point.confidence},
                       {"causal_eligible", point.causal_eligible},
                       {"causal_lag_ms", QString::number(point.causal_lag_ms)},
                       {"causal_reason", point.causal_reason},
                       {"cross_horizon_consistent", point.cross_horizon_consistent},
                       {"cross_horizon_reason", point.cross_horizon_reason},
                       {"probability_source", point.probability_source},
                       {"calibration_bucket", point.calibration_bucket},
                       {"calibration_samples", point.calibration_samples},
                       {"settlement_mean", point.settlement_mean},
                       {"settlement_stddev", point.settlement_stddev},
                       {"context_freshness_ms", QString::number(point.context_freshness_ms)},
                       {"context_sample_count", point.context_sample_count},
                       {"context_calibration_score", point.context_calibration_score},
                       {"context_up_probability", point.context_up_probability},
                       {"context_sources", point.context_sources},
                       {"seconds_left", point.seconds_left}, {"valid", point.valid},
                       {"rejection_reason", point.rejection_reason}};
}

} // namespace

KalshiVolatilityEstimate KalshiAutoEngine::estimate_realized_volatility(
    const QVector<KalshiTimedPrice>& input_prices,
    qint64 decision_ts_ms) {
    KalshiVolatilityEstimate out;
    if (decision_ts_ms <= 0) {
        out.reason = QStringLiteral("invalid decision timestamp");
        return out;
    }

    QVector<KalshiTimedPrice> prices;
    prices.reserve(input_prices.size());
    for (const auto& point : input_prices) {
        if (point.price > 0.0 && point.observed_at_ms > 0 &&
            point.observed_at_ms <= decision_ts_ms)
            prices.append(point);
    }
    std::sort(prices.begin(), prices.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.observed_at_ms < rhs.observed_at_ms;
    });
    if (prices.size() < 31) {
        out.reason = QStringLiteral("need at least 31 timestamped minute prices");
        return out;
    }

    out.observed_at_ms = prices.last().observed_at_ms;
    if (decision_ts_ms - out.observed_at_ms > 3 * 60 * 1000) {
        out.reason = QStringLiteral("latest volatility input is older than 3 minutes");
        return out;
    }

    struct TimedReturn {
        qint64 observed_at_ms = 0;
        double per_minute = 0.0;
        int utc_hour = 0;
    };
    QVector<TimedReturn> returns;
    returns.reserve(prices.size() - 1);
    for (int i = 1; i < prices.size(); ++i) {
        const qint64 elapsed_ms = prices[i].observed_at_ms - prices[i - 1].observed_at_ms;
        if (elapsed_ms < 30'000 || elapsed_ms > 5 * 60 * 1000)
            continue;
        const double elapsed_minutes = elapsed_ms / 60'000.0;
        const double normalized = std::log(prices[i].price / prices[i - 1].price) /
                                  std::sqrt(elapsed_minutes);
        if (!std::isfinite(normalized))
            continue;
        returns.append(TimedReturn{
            prices[i].observed_at_ms,
            normalized,
            QDateTime::fromMSecsSinceEpoch(prices[i].observed_at_ms, QTimeZone::UTC).time().hour()});
    }

    const qint64 recent_cutoff = decision_ts_ms - 6 * 60 * 60 * 1000;
    constexpr double half_life_minutes = 60.0;
    double weight_sum = 0.0;
    double weighted_return = 0.0;
    double weighted_square = 0.0;
    int recent_count = 0;
    for (const auto& sample : returns) {
        if (sample.observed_at_ms < recent_cutoff)
            continue;
        const double age_minutes = (decision_ts_ms - sample.observed_at_ms) / 60'000.0;
        const double weight = std::exp(-std::log(2.0) * age_minutes / half_life_minutes);
        weight_sum += weight;
        weighted_return += weight * sample.per_minute;
        weighted_square += weight * sample.per_minute * sample.per_minute;
        ++recent_count;
    }
    out.sample_count = recent_count;
    if (recent_count < 30 || weight_sum <= 0.0) {
        out.reason = QStringLiteral("need at least 30 recent one-minute returns");
        return out;
    }

    const double mean = weighted_return / weight_sum;
    const double minute_variance = std::max(0.0, weighted_square / weight_sum - mean * mean);
    if (minute_variance <= 0.0) {
        out.reason = QStringLiteral("recent realized variance is zero");
        return out;
    }

    const int current_hour = QDateTime::fromMSecsSinceEpoch(
        decision_ts_ms, QTimeZone::UTC).time().hour();
    double all_square = 0.0;
    double hour_square = 0.0;
    int all_count = 0;
    int hour_count = 0;
    for (const auto& sample : returns) {
        const double square = sample.per_minute * sample.per_minute;
        all_square += square;
        ++all_count;
        if (sample.utc_hour == current_hour) {
            hour_square += square;
            ++hour_count;
        }
    }
    if (all_count >= 120 && hour_count >= 8 && all_square > 0.0) {
        const double raw = std::sqrt((hour_square / hour_count) / (all_square / all_count));
        // Shrink noisy hourly seasonality halfway toward neutral.
        out.time_of_day_multiplier = std::clamp(1.0 + 0.5 * (raw - 1.0), 0.75, 1.25);
    }

    constexpr double minutes_per_year = 365.0 * 24.0 * 60.0;
    out.annual_volatility = std::clamp(
        std::sqrt(minute_variance * minutes_per_year) * out.time_of_day_multiplier,
        0.15, 2.50);
    out.ready = std::isfinite(out.annual_volatility);
    out.reason = out.ready ? QStringLiteral("ready")
                           : QStringLiteral("realized volatility was not finite");
    return out;
}

KalshiDistributionEstimate KalshiAutoEngine::estimate_distribution(
    const QVector<KalshiTimedPrice>& input_prices,
    qint64 decision_ts_ms) {
    KalshiDistributionEstimate out;
    QVector<KalshiTimedPrice> prices;
    for (const auto& point : input_prices) {
        if (point.price > 0.0 && point.observed_at_ms > 0 &&
            point.observed_at_ms <= decision_ts_ms)
            prices.append(point);
    }
    std::sort(prices.begin(), prices.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.observed_at_ms < rhs.observed_at_ms;
    });
    if (prices.size() < 32) {
        out.reason = QStringLiteral("need at least 32 timestamped prices");
        return out;
    }

    QVector<double> returns;
    const qint64 cutoff = decision_ts_ms - 6 * 60 * 60 * 1000;
    for (int i = 1; i < prices.size(); ++i) {
        if (prices[i].observed_at_ms < cutoff)
            continue;
        const qint64 elapsed = prices[i].observed_at_ms - prices[i - 1].observed_at_ms;
        if (elapsed < 30'000 || elapsed > 5 * 60 * 1000)
            continue;
        const double per_minute = std::log(prices[i].price / prices[i - 1].price) /
                                  std::sqrt(elapsed / 60'000.0);
        if (std::isfinite(per_minute))
            returns.append(per_minute);
    }
    if (returns.size() < 30) {
        out.reason = QStringLiteral("need at least 30 recent returns");
        return out;
    }

    double bipower = 0.0;
    for (int i = 1; i < returns.size(); ++i)
        bipower += std::abs(returns[i]) * std::abs(returns[i - 1]);
    bipower *= (M_PI / 2.0) /
               static_cast<double>(std::max<qsizetype>(1, returns.size() - 1));
    const double diffusion_sigma = std::sqrt(std::max(1e-16, bipower));
    const double jump_cutoff = 4.0 * diffusion_sigma;
    double jump_sum = 0.0;
    for (double value : returns) {
        if (std::abs(value) > jump_cutoff) {
            jump_sum += std::abs(value);
            ++out.jump_sample_count;
        }
    }
    constexpr double minutes_per_year = 365.0 * 24.0 * 60.0;
    out.diffusion_annual_volatility = std::clamp(
        diffusion_sigma * std::sqrt(minutes_per_year), 0.05, 3.0);
    out.diffusion_sample_count = returns.size();
    out.jump_intensity_per_hour = out.jump_sample_count * 60.0 / returns.size();
    out.mean_absolute_jump_return = out.jump_sample_count > 0
        ? jump_sum / out.jump_sample_count : 0.0;
    out.observed_at_ms = prices.last().observed_at_ms;
    out.ready = decision_ts_ms - out.observed_at_ms <= 3 * 60 * 1000;
    out.reason = out.ready ? QStringLiteral("ready")
                           : QStringLiteral("latest distribution input is stale");
    return out;
}

KalshiCalibrationModel KalshiAutoEngine::fit_isotonic_calibration(
    const QVector<KalshiCalibrationSample>& input_samples,
    int minimum_samples) {
    KalshiCalibrationModel out;
    QVector<KalshiCalibrationSample> samples;
    for (const auto& sample : input_samples) {
        if (sample.predicted_probability >= 0.0 && sample.predicted_probability <= 1.0 &&
            sample.market_probability >= 0.0 && sample.market_probability <= 1.0 &&
            (sample.outcome == 0.0 || sample.outcome == 1.0))
            samples.append(sample);
    }
    QHash<QString, QVector<int>> event_rows;
    for (int i = 0; i < samples.size(); ++i) {
        QString event_id = samples[i].independent_event_id.trimmed();
        if (event_id.isEmpty())
            event_id = QStringLiteral("observation:%1:%2")
                           .arg(samples[i].observed_at_ms).arg(i);
        event_rows[event_id].append(i);
    }
    out.sample_count = event_rows.size();
    if (event_rows.size() < minimum_samples) {
        out.reason = QStringLiteral("insufficient independent settlement events (%1 < %2)")
                         .arg(event_rows.size()).arg(minimum_samples);
        return out;
    }

    const auto fit_curve = [&samples, &event_rows](const QString& excluded_event) {
        struct WeightedSample {
            double predicted = 0.5;
            double outcome = 0.0;
            double weight = 0.0;
        };
        QVector<WeightedSample> training;
        for (auto event = event_rows.cbegin(); event != event_rows.cend(); ++event) {
            if (event.key() == excluded_event || event.value().isEmpty())
                continue;
            const double weight = 1.0 / event.value().size();
            for (int index : event.value())
                training.append({samples[index].predicted_probability,
                                 samples[index].outcome, weight});
        }
        std::sort(training.begin(), training.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.predicted < rhs.predicted;
        });
        struct Block {
            double x_sum = 0.0;
            double y_sum = 0.0;
            double weight = 0.0;
            int count = 0;
        };
        QVector<Block> blocks;
        for (const auto& sample : training) {
            blocks.append(Block{sample.predicted * sample.weight,
                                sample.outcome * sample.weight,
                                sample.weight, 1});
            while (blocks.size() > 1) {
                const auto& left = blocks[blocks.size() - 2];
                const auto& right = blocks.last();
                if (left.y_sum / left.weight <= right.y_sum / right.weight)
                    break;
                const Block merged{left.x_sum + right.x_sum, left.y_sum + right.y_sum,
                                   left.weight + right.weight, left.count + right.count};
                blocks.removeLast();
                blocks.last() = merged;
            }
        }
        QVector<KalshiCalibrationPoint> curve;
        for (const auto& block : blocks) {
            curve.append(KalshiCalibrationPoint{
                block.x_sum / block.weight,
                clamp_probability(block.y_sum / block.weight),
                block.count});
        }
        return curve;
    };

    QVector<double> clustered_advantages;
    for (auto event = event_rows.cbegin(); event != event_rows.cend(); ++event) {
        const auto curve = fit_curve(event.key());
        if (curve.isEmpty())
            continue;
        double model_loss = 0.0;
        double market_loss = 0.0;
        for (int index : event.value()) {
            const auto& sample = samples[index];
            const double calibrated = calibrated_probability(sample.predicted_probability, curve);
            model_loss += std::pow(calibrated - sample.outcome, 2.0);
            market_loss += std::pow(sample.market_probability - sample.outcome, 2.0);
        }
        model_loss /= event.value().size();
        market_loss /= event.value().size();
        out.model_brier += model_loss;
        out.market_brier += market_loss;
        clustered_advantages.append(market_loss - model_loss);
    }
    if (clustered_advantages.size() != event_rows.size()) {
        out.reason = QStringLiteral("prequential calibration could not score every event");
        return out;
    }
    out.model_brier /= clustered_advantages.size();
    out.market_brier /= clustered_advantages.size();
    const double advantage = out.market_brier - out.model_brier;
    double squared_deviation = 0.0;
    for (double value : clustered_advantages)
        squared_deviation += std::pow(value - advantage, 2.0);
    if (clustered_advantages.size() > 1) {
        const double sample_variance = squared_deviation / (clustered_advantages.size() - 1);
        out.clustered_standard_error = std::sqrt(sample_variance / clustered_advantages.size());
    }
    out.conservative_advantage = advantage - 2.0 * out.clustered_standard_error;
    const double evidence = std::clamp(event_rows.size() / 100.0, 0.0, 1.0);
    out.learned_model_weight = out.market_brier > 1e-12 && out.conservative_advantage > 0.0
        ? std::clamp((out.conservative_advantage / out.market_brier) * evidence, 0.0, 0.75)
        : 0.0;
    out.points = fit_curve(QString());
    out.ready = true;
    out.reason = out.learned_model_weight > 0.0
        ? QStringLiteral("out-of-fold calibrated model beats market by more than 2 clustered SE")
        : QStringLiteral("no out-of-fold advantage beyond 2 clustered SE");
    return out;
}

double KalshiAutoEngine::calibrated_probability(
    double probability,
    const QVector<KalshiCalibrationPoint>& curve) {
    const double p = clamp_probability(probability);
    if (curve.isEmpty())
        return p;
    if (p <= curve.first().predicted_probability)
        return clamp_probability(curve.first().calibrated_probability);
    for (int i = 1; i < curve.size(); ++i) {
        if (p > curve[i].predicted_probability)
            continue;
        const double width = curve[i].predicted_probability - curve[i - 1].predicted_probability;
        if (width <= 1e-12)
            return clamp_probability(curve[i].calibrated_probability);
        const double t = (p - curve[i - 1].predicted_probability) / width;
        return clamp_probability(curve[i - 1].calibrated_probability * (1.0 - t) +
                                 curve[i].calibrated_probability * t);
    }
    return clamp_probability(curve.last().calibrated_probability);
}

double KalshiAutoEngine::settlement_average_probability_above(
    const KalshiAutoContext& context,
    double strike,
    int seconds_left,
    double* projected_mean,
    double* projected_stddev,
    const QString& contract_horizon) {
    const auto& average = context.settlement_average;
    const double reference = average.available && average.latest_index > 0.0
        ? average.latest_index : context.spot - context.spot_index_basis;
    if (reference <= 0.0 || strike <= 0.0) {
        if (projected_mean) *projected_mean = 0.0;
        if (projected_stddev) *projected_stddev = 0.0;
        return 0.0;
    }
    if (seconds_left <= 0) {
        if (projected_mean) *projected_mean = reference;
        if (projected_stddev) *projected_stddev = 0.0;
        return reference >= strike ? 1.0 : 0.0;
    }

    const int window = std::max(1, average.window_seconds);
    const int elapsed_in_window = average.available && seconds_left <= window
        ? std::clamp(window - seconds_left, 0, window) : 0;
    int observed = 0;
    double observed_sum = 0.0;
    if (elapsed_in_window > 0 && !average.recent_observations.isEmpty()) {
        const qint64 window_start = context.decision_ts_ms - elapsed_in_window * 1000LL;
        QMap<qint64, QVector<double>> values_by_second;
        for (const auto& sample : average.recent_observations) {
            if (sample.observed_at_ms < window_start ||
                sample.observed_at_ms > context.decision_ts_ms || sample.price <= 0.0)
                continue;
            values_by_second[sample.observed_at_ms / 1000].append(sample.price);
        }
        // A closed interval can contain elapsed+1 second buckets. Consume only
        // the newest elapsed buckets so the numerator and denominator always
        // describe the same number of observations.
        auto it = values_by_second.constEnd();
        while (it != values_by_second.constBegin() && observed < elapsed_in_window) {
            --it;
            double second_sum = 0.0;
            for (double value : it.value()) second_sum += value;
            observed_sum += second_sum / it.value().size();
            ++observed;
        }
    } else if (elapsed_in_window > 0) {
        observed = std::min({average.observed_samples, elapsed_in_window, window});
        if (average.observed_samples > 0)
            observed_sum = average.observed_sum * observed / average.observed_samples;
    }
    const int remaining = std::max(0, window - observed);
    // Cross-venue lead/lag is deliberately excluded from pricing. The former
    // Binance-perpetual pulse was REST-polled and not clock-normalized, so it
    // could turn basis and clock skew into apparent predictive edge.
    const double directional = std::clamp(
        (context_up_probability(context) - 0.5) * 0.003, -0.01, 0.01);
    const double future_reference = reference * std::exp(directional);
    double mean = future_reference;
    double effective_seconds = std::max(1.0, seconds_left - window + window / 3.0);
    double remaining_weight = 1.0;
    if (observed > 0) {
        mean = (observed_sum + remaining * future_reference) / window;
        remaining_weight = remaining / static_cast<double>(window);
        effective_seconds = std::max(1.0, remaining / 3.0);
    }
    double annual_vol = context.distribution.ready
        ? context.distribution.diffusion_annual_volatility : context.annual_volatility;
    annual_vol *= std::max(1.0, context.event_volatility_multiplier);
    const double standard_deviation = reference * annual_vol *
        std::sqrt(effective_seconds / seconds_per_year()) * remaining_weight;
    double probability = standard_deviation > 1e-9
        ? normal_cdf((mean - strike) / standard_deviation)
        : (mean >= strike ? 1.0 : 0.0);

    if (context.distribution.ready && context.distribution.jump_intensity_per_hour > 0.0 &&
        context.distribution.mean_absolute_jump_return > 0.0) {
        const double jump_probability = std::clamp(
            1.0 - std::exp(-context.distribution.jump_intensity_per_hour * seconds_left / 3600.0),
            0.0, 0.35);
        const double shift = reference * context.distribution.mean_absolute_jump_return;
        const double up = standard_deviation > 1e-9
            ? normal_cdf((mean + shift - strike) / standard_deviation)
            : (mean + shift >= strike ? 1.0 : 0.0);
        const double down = standard_deviation > 1e-9
            ? normal_cdf((mean - shift - strike) / standard_deviation)
            : (mean - shift >= strike ? 1.0 : 0.0);
        probability = (1.0 - jump_probability) * probability +
                      jump_probability * 0.5 * (up + down);
    }
    const QString empirical_horizon = !contract_horizon.isEmpty() ? contract_horizon
        : seconds_left <= 15 * 60 ? QStringLiteral("15m")
        : seconds_left <= 65 * 60 ? QStringLiteral("1h") : QStringLiteral("1d");
    const QVector<double> empirical_returns =
        context.conditioned_returns_by_horizon.value(empirical_horizon);
    if (empirical_returns.size() >= 50) {
        int wins = 0;
        const double threshold_return = strike / std::max(1e-9, mean) - 1.0;
        for (double value : empirical_returns)
            if (value >= threshold_return) ++wins;
        const double empirical = (wins + 1.0) / (empirical_returns.size() + 2.0);
        const double nominal_horizon_seconds = empirical_horizon == QStringLiteral("15m")
            ? 15.0 * 60.0 : empirical_horizon == QStringLiteral("1h")
                ? 60.0 * 60.0 : 24.0 * 60.0 * 60.0;
        const double remaining_fraction = std::clamp(
            seconds_left / nominal_horizon_seconds, 0.0, 1.0);
        const double empirical_weight = 0.25 * remaining_fraction * remaining_fraction;
        probability = (1.0 - empirical_weight) * probability + empirical_weight * empirical;
    }
    if (projected_mean) *projected_mean = mean;
    if (projected_stddev) *projected_stddev = standard_deviation;
    return clamp_probability(probability);
}

QVector<KalshiSurfacePoint> KalshiAutoEngine::build_surface(
    const QVector<pr::PredictionMarket>& markets,
    const QHash<QString, pr::PredictionOrderBook>& books,
    const KalshiAutoContext& input_context,
    const QString& event_ticker) {
    KalshiAutoContext context = input_context;
    if (context.decision_ts_ms <= 0)
        context.decision_ts_ms = QDateTime::currentMSecsSinceEpoch();
    const double cross_horizon_up = context_up_probability(context);
    QVector<KalshiSurfacePoint> surface;
    for (const auto& market : markets) {
        if (!event_ticker.isEmpty() && market.key.event_id != event_ticker)
            continue;
        KalshiSurfacePoint point;
        point.ticker = market.key.market_id;
        point.event_ticker = market.key.event_id;
        point.question = market.question;
        attach_context_audit(point, context);
        point.context_up_probability = cross_horizon_up;
        point.kind = contract_kind(market, &point.floor, &point.cap);
        const QDateTime close = QDateTime::fromString(market.end_date_iso, Qt::ISODate);
        point.settlement_ts_ms = close.isValid() ? close.toMSecsSinceEpoch() : 0;
        point.seconds_left = close.isValid()
            ? static_cast<int>(std::max<qint64>(0, close.toSecsSinceEpoch() - context.decision_ts_ms / 1000))
            : -1;
        if (context.spot <= 0.0 || point.kind.isEmpty() || point.seconds_left < 0 || market.closed) {
            point.rejection_reason = QStringLiteral("missing spot, strike, close time, or active market");
            surface.append(point);
            continue;
        }
        point.cadence = market_cadence(market, point.seconds_left);
        if (!context.volatility_ready || context.annual_volatility <= 0.0) {
            point.rejection_reason = context.volatility_reason.isEmpty()
                ? QStringLiteral("live realized volatility is unavailable")
                : QStringLiteral("live realized volatility unavailable: %1")
                      .arg(context.volatility_reason);
            surface.append(point);
            continue;
        }
        if (context.spot_observed_at_ms <= 0 || context.spot_observed_at_ms > context.decision_ts_ms ||
            context.decision_ts_ms - context.spot_observed_at_ms > context.max_spot_age_ms) {
            point.rejection_reason = QStringLiteral("spot reference is stale or from the future");
            surface.append(point);
            continue;
        }
        if (point.seconds_left <= context.settlement_average.window_seconds &&
            (!context.settlement_average.available ||
             context.settlement_average.latest_index_observed_at_ms <= 0 ||
             context.settlement_average.latest_index_observed_at_ms > context.decision_ts_ms ||
             context.decision_ts_ms -
                     context.settlement_average.latest_index_observed_at_ms > 5'000)) {
            point.rejection_reason = QStringLiteral(
                "official settlement reference is unavailable in the final window");
            surface.append(point);
            continue;
        }

        double floor_mean = 0.0;
        double floor_stddev = 0.0;
        const double floor_above = point.floor > 0.0
            ? settlement_average_probability_above(context, point.floor, point.seconds_left,
                                                   &floor_mean, &floor_stddev, point.cadence) : 0.0;
        double cap_mean = 0.0;
        double cap_stddev = 0.0;
        const double cap_above = point.cap > 0.0
            ? settlement_average_probability_above(context, point.cap, point.seconds_left,
                                                   &cap_mean, &cap_stddev, point.cadence) : 0.0;
        if (point.kind == QStringLiteral("above")) point.model_probability_raw = floor_above;
        else if (point.kind == QStringLiteral("below")) point.model_probability_raw = 1.0 - cap_above;
        else point.model_probability_raw = std::max(0.001, floor_above - cap_above);
        point.model_probability_raw = clamp_probability(point.model_probability_raw);
        point.settlement_mean = floor_mean > 0.0 ? floor_mean : cap_mean;
        point.settlement_stddev = std::max(floor_stddev, cap_stddev);

        const Quote yes = quote_for(market, books, QStringLiteral("yes"));
        const Quote no = quote_for(market, books, QStringLiteral("no"));
        point.yes_bid = yes.bid;
        point.yes_ask = yes.ask;
        point.no_bid = no.bid;
        point.no_ask = no.ask;
        point.yes_depth = yes.depth;
        point.no_depth = no.depth;
        point.quote_observed_at_ms = std::max(yes.observed_at_ms, no.observed_at_ms);
        point.market_implied_probability = yes.bid > 0.0 && yes.ask > yes.bid
            ? clamp_probability(0.5 * (yes.bid + yes.ask))
            : clamp_probability(outcome_price(market, QStringLiteral("yes")));
        point.calibrated_probability = point.model_probability_raw;
        point.calibration_samples = 0;
        // Authority is bucket-local. A successful bucket must never lend its
        // weight to a cadence/moneyness/time bucket that has no evidence.
        point.model_weight = context.calibration_gate_enabled ? 0.0 : 1.0;
        point.fair_yes = clamp_probability(
            point.market_implied_probability + point.model_weight *
                (point.calibrated_probability - point.market_implied_probability));
        point.fair_no = 1.0 - point.fair_yes;
        point.probability_source = context.calibration_gate_enabled
            ? (point.model_weight > 0.0 ? QStringLiteral("market_shrunk_calibrated_model")
                                       : QStringLiteral("market_only_unproven_model"))
            : QStringLiteral("settlement_average_model");
        const double reference = context.settlement_average.available
            ? context.settlement_average.latest_index : context.spot;
        const double strike = point.floor > 0.0 ? point.floor : point.cap;
        const double moneyness = strike > 0.0 && reference > 0.0
            ? std::log(reference / strike) * 10'000.0 : 0.0;
        const QString cadence = point.cadence == QStringLiteral("1d")
            ? QStringLiteral("daily") : point.cadence;
        QString money_bucket;
        if (std::abs(moneyness) < 25.0) {
            money_bucket = QStringLiteral("atm");
        } else if (point.kind == QStringLiteral("below")) {
            money_bucket = moneyness < 0.0 ? QStringLiteral("itm") : QStringLiteral("otm");
        } else if (point.kind == QStringLiteral("range")) {
            money_bucket = reference >= point.floor && reference < point.cap
                ? QStringLiteral("itm") : QStringLiteral("otm");
        } else {
            money_bucket = moneyness > 0.0 ? QStringLiteral("itm") : QStringLiteral("otm");
        }
        const QString time_bucket = point.seconds_left <= 60 ? QStringLiteral("final_60s")
            : point.seconds_left <= 5 * 60 ? QStringLiteral("final_5m")
            : point.seconds_left <= 30 * 60 ? QStringLiteral("mid") : QStringLiteral("early");
        point.calibration_bucket = cadence + QLatin1Char('/') + money_bucket +
                                   QLatin1Char('/') + time_bucket;
        const auto bucket_it = context.calibration_models.constFind(point.calibration_bucket);
        if (bucket_it != context.calibration_models.constEnd()) {
            point.calibration_samples = bucket_it->sample_count;
            point.calibrated_probability = calibrated_probability(
                point.model_probability_raw, bucket_it->points);
            point.model_weight = context.calibration_gate_enabled && bucket_it->ready
                ? bucket_it->learned_model_weight : (context.calibration_gate_enabled ? 0.0 : 1.0);
            point.fair_yes = clamp_probability(
                point.market_implied_probability + point.model_weight *
                    (point.calibrated_probability - point.market_implied_probability));
            point.fair_no = 1.0 - point.fair_yes;
            point.probability_source = point.model_weight > 0.0
                ? QStringLiteral("bucket_calibrated_market_shrink")
                : QStringLiteral("market_only_unproven_bucket");
        }

        const qint64 informative_ts = std::max(
            context.spot_observed_at_ms,
            context.settlement_average.latest_index_observed_at_ms);
        point.causal_lag_ms = informative_ts > 0 && point.quote_observed_at_ms > 0
            ? informative_ts - point.quote_observed_at_ms : -1;
        point.causal_eligible = !context.causal_gate_enabled ||
            (point.causal_lag_ms >= context.minimum_causal_lag_ms &&
             context.decision_ts_ms >= informative_ts &&
             context.decision_ts_ms - informative_ts <= context.max_spot_age_ms);
        if (!point.causal_eligible)
            point.causal_reason = QStringLiteral("quote did not precede a fresh independent price move");
        point.cross_horizon_consistent = cross_horizon_consistent(
            context, &point.cross_horizon_reason);
        const double yes_fee = fee_per_contract(market, point.yes_ask, context.decision_ts_ms);
        const double no_fee = fee_per_contract(market, point.no_ask, context.decision_ts_ms);
        const double yes_edge = point.fair_yes - point.yes_ask - yes_fee;
        const double no_edge = point.fair_no - point.no_ask - no_fee;
        const bool choose_yes = yes_edge >= no_edge;
        point.selected_side = choose_yes ? QStringLiteral("yes") : QStringLiteral("no");
        point.selected_fair = choose_yes ? point.fair_yes : point.fair_no;
        point.selected_ask = choose_yes ? point.yes_ask : point.no_ask;
        point.selected_bid = choose_yes ? point.yes_bid : point.no_bid;
        point.fee_per_contract = choose_yes ? yes_fee : no_fee;
        point.net_edge = choose_yes ? yes_edge : no_edge;
        const double sigma = std::max(1e-8, context.annual_volatility) *
                             std::sqrt(std::max(1, point.seconds_left) / seconds_per_year());
        const double distance = strike > 0.0 ? std::abs(std::log(context.spot / strike)) / sigma : 0.0;
        point.confidence = std::clamp(0.50 + point.model_weight *
            std::min(0.35, distance * 0.08), 0.0, 0.85);
        const double selected_spread = point.selected_ask - point.selected_bid;
        point.valid = point.selected_bid > 0.0 &&
                      point.selected_ask > point.selected_bid &&
                      point.selected_ask < 1.0 &&
                      selected_spread <= 0.10 + 1e-9;
        if (!point.valid) {
            if (point.selected_ask <= 0.0 || point.selected_ask >= 1.0)
                point.rejection_reason = QStringLiteral("no executable selected-side ask");
            else if (point.selected_bid <= 0.0)
                point.rejection_reason = QStringLiteral("one-sided selected-side book");
            else if (point.selected_ask <= point.selected_bid)
                point.rejection_reason = QStringLiteral("crossed or invalid selected-side book");
            else
                point.rejection_reason = QStringLiteral("selected-side spread exceeds 10c");
        }
        surface.append(point);
    }

    fit_monotone_market_curve(surface);
    for (auto& point : surface) {
        if (!point.valid)
            continue;
        point.relative_value_residual =
            point.calibrated_probability - point.market_curve_probability;
        point.fair_yes = clamp_probability(
            point.market_curve_probability + point.model_weight * point.relative_value_residual);
        point.fair_no = 1.0 - point.fair_yes;
    }
    enforce_monotone_above(surface);
    for (auto& point : surface) {
        if (!point.valid)
            continue;
        point.fair_no = 1.0 - point.fair_yes;
        const auto market_it = std::find_if(markets.cbegin(), markets.cend(), [&point](const auto& market) {
            return market.key.market_id == point.ticker;
        });
        if (market_it == markets.cend()) {
            point.valid = false;
            point.rejection_reason = QStringLiteral("market metadata disappeared during surface repair");
            continue;
        }
        const double yes_fee = fee_per_contract(*market_it, point.yes_ask, context.decision_ts_ms);
        const double no_fee = fee_per_contract(*market_it, point.no_ask, context.decision_ts_ms);
        const double yes_edge = point.fair_yes - point.yes_ask - yes_fee;
        const double no_edge = point.fair_no - point.no_ask - no_fee;
        const bool yes = yes_edge >= no_edge;
        point.selected_side = yes ? QStringLiteral("yes") : QStringLiteral("no");
        point.selected_fair = yes ? point.fair_yes : point.fair_no;
        point.selected_ask = yes ? point.yes_ask : point.no_ask;
        point.selected_bid = yes ? point.yes_bid : point.no_bid;
        point.fee_per_contract = yes ? yes_fee : no_fee;
        point.net_edge = yes ? yes_edge : no_edge;
        point.time_risk_score = point.net_edge *
            std::sqrt(3600.0 / std::max(30, point.seconds_left));
        point.directional_delta = (yes ? 1.0 : -1.0) * point.selected_ask;
    }
    std::sort(surface.begin(), surface.end(), [](const auto& a, const auto& b) {
        if (a.valid != b.valid) return a.valid;
        if (a.time_risk_score != b.time_risk_score) return a.time_risk_score > b.time_risk_score;
        if (a.net_edge != b.net_edge) return a.net_edge > b.net_edge;
        return a.ticker < b.ticker;
    });
    return surface;
}

KalshiPortfolioPlan KalshiAutoEngine::optimize(const QVector<KalshiSurfacePoint>& surface,
                                                const KalshiAutoContext& context,
                                                const KalshiPortfolioConstraints& constraints) {
    KalshiPortfolioPlan plan;
    if (context.event_trading_blocked) {
        plan.verdict = QStringLiteral("NO TRADE");
        plan.blockers.append(context.event_risk_reason.isEmpty()
            ? QStringLiteral("scheduled event risk block is active")
            : QStringLiteral("scheduled event risk: %1").arg(context.event_risk_reason));
        return plan;
    }
    if (!context.exposure_snapshot_ready) {
        plan.verdict = QStringLiteral("NO TRADE");
        plan.blockers.append(context.exposure_snapshot_reason.isEmpty()
            ? QStringLiteral("existing exposure snapshot is unavailable")
            : context.exposure_snapshot_reason);
        return plan;
    }
    const QVector<double> scenarios = settlement_scenarios(surface, context);
    int yes_count = context.existing_yes_positions;
    int no_count = context.existing_no_positions;
    double net_directional_cost = context.existing_net_directional_cost;
    QSet<QString> tickers;
    for (const auto& ticker : context.existing_market_ids)
        tickers.insert(ticker);

    QVector<KalshiSurfacePoint> ordered_surface = surface;
    const auto clears_entry_gate = [&constraints, &context](const KalshiSurfacePoint& point) {
        return point.valid && point.cross_horizon_consistent &&
            (!context.causal_gate_enabled || point.causal_eligible) &&
            point.net_edge - constraints.exit_cost_reserve >= constraints.minimum_net_edge &&
            point.selected_ask > 0.0 && point.selected_ask <= constraints.maximum_entry_price;
    };
    // Threshold contracts are monotone and easier to audit than narrow ranges.
    // Reserve the first eligible slot for the best "or above" contract, while
    // preserving net-edge order within threshold and non-threshold groups.
    std::stable_sort(ordered_surface.begin(), ordered_surface.end(),
                     [&clears_entry_gate](const auto& left, const auto& right) {
        const bool left_priority = left.kind == QStringLiteral("above") && clears_entry_gate(left);
        const bool right_priority = right.kind == QStringLiteral("above") && clears_entry_gate(right);
        return left_priority != right_priority ? left_priority : false;
    });

    for (const auto& point : ordered_surface) {
        if (!point.valid || !point.cross_horizon_consistent ||
            (context.causal_gate_enabled && !point.causal_eligible) ||
            point.net_edge - constraints.exit_cost_reserve < constraints.minimum_net_edge)
            continue;
        if (point.selected_ask <= 0.0 || point.selected_ask > constraints.maximum_entry_price)
            continue;
        if (tickers.contains(point.ticker))
            continue;
        if (context.existing_open_positions + plan.legs.size() >= constraints.max_positions)
            break;
        if (point.selected_side == QStringLiteral("yes") && yes_count >= constraints.max_same_side)
            continue;
        if (point.selected_side == QStringLiteral("no") && no_count >= constraints.max_same_side)
            continue;

        int contracts = std::max(1, static_cast<int>(std::floor(
            constraints.unit_notional / std::max(0.01, point.selected_ask))));
        const double displayed_depth = point.selected_side == QStringLiteral("yes")
            ? point.yes_depth : point.no_depth;
        if (displayed_depth > 0.0)
            contracts = std::min(contracts, std::max(1, static_cast<int>(std::floor(displayed_depth))));
        const double fee = point.fee_per_contract * contracts;
        const double cost = point.selected_ask * contracts + fee;
        if (context.existing_total_cost + plan.total_cost + cost > constraints.max_total_cost)
            continue;
        const double signed_cost = (point.selected_side == QStringLiteral("yes") ? 1.0 : -1.0) *
                                   point.selected_ask * contracts;
        if (std::abs(net_directional_cost + signed_cost) >
            constraints.max_net_directional_cost + 1e-9)
            continue;
        KalshiPortfolioLeg leg;
        leg.ticker = point.ticker;
        leg.side = point.selected_side;
        leg.kind = point.kind;
        leg.floor = point.floor;
        leg.cap = point.cap;
        leg.contracts = contracts;
        leg.entry_price = point.selected_ask;
        leg.fair_probability = point.selected_fair;
        leg.entry_fee = fee;
        leg.expected_profit = (point.selected_fair - point.selected_ask) * contracts - fee;

        KalshiPortfolioPlan trial = plan;
        trial.legs.append(leg);
        trial.total_cost += cost;
        trial.expected_profit += leg.expected_profit;
        recompute_plan(trial, scenarios);
        if (trial.worst_case_pnl - context.existing_total_cost <
            -constraints.max_worst_case_loss)
            continue;
        plan = trial;
        net_directional_cost += signed_cost;
        tickers.insert(point.ticker);
        point.selected_side == QStringLiteral("yes") ? ++yes_count : ++no_count;
    }

    recompute_plan(plan, scenarios);
    plan.expected_pnl = plan.expected_profit;
    if (plan.legs.isEmpty()) {
        plan.verdict = QStringLiteral("NO TRADE");
        plan.blockers << QStringLiteral("no distinct executable contracts clear edge and risk constraints");
    } else {
        const bool calibrated_context = context.calibration_gate_enabled
            ? std::any_of(surface.cbegin(), surface.cend(), [&context](const auto& point) {
                  return point.calibration_samples >= context.minimum_calibration_samples &&
                         point.model_weight > 0.0;
              })
            : std::any_of(surface.cbegin(), surface.cend(), [](const auto& point) {
                  return point.context_sample_count >= 30 && point.context_calibration_score > 0.0;
              });
        plan.verdict = calibrated_context ? QStringLiteral("PAPER PORTFOLIO CANDIDATE")
                                          : QStringLiteral("UNCALIBRATED PAPER EXPERIMENT");
        if (!calibrated_context)
            plan.blockers << QStringLiteral("no calibrated cross-horizon model with at least 30 samples");
    }
    return plan;
}

KalshiMicroEvidenceResult KalshiAutoEngine::evaluate_micro_evidence(
    const KalshiMicroEvidenceInput& input,
    double minimum_edge,
    qint64 maximum_quote_age_ms) {
    KalshiMicroEvidenceResult result;
    result.required_edge = input.seconds_left > 30 * 60 ? std::max(minimum_edge, 0.08)
        : input.seconds_left > 5 * 60 ? std::max(minimum_edge, 0.05)
                                     : minimum_edge;
    const QString side = input.side.trimmed().toLower();
    if (side != QStringLiteral("yes") && side != QStringLiteral("no"))
        result.blockers << QStringLiteral("side is not YES or NO");
    if (input.ask < 0.05 || input.ask > 0.95)
        result.blockers << QStringLiteral("entry price is outside the 5c-95c evidence band");
    if (input.seconds_left < 20)
        result.blockers << QStringLiteral("fewer than 20 seconds remain");
    if (input.depth < 1.0)
        result.blockers << QStringLiteral("less than one contract of executable depth");
    if (input.confidence < 0.50)
        result.blockers << QStringLiteral("model confidence below 50%");
    if (input.quote_age_ms < 0 ||
        (maximum_quote_age_ms > 0 && input.quote_age_ms > maximum_quote_age_ms))
        result.blockers << QStringLiteral("quote older than %1 seconds")
                               .arg(maximum_quote_age_ms / 1000.0, 0, 'f', 1);
    if (input.edge_after_cost + 1e-12 < result.required_edge)
        result.blockers << QStringLiteral("%1% edge is below the %2% time-conditioned floor")
                               .arg(input.edge_after_cost * 100.0, 0, 'f', 1)
                               .arg(result.required_edge * 100.0, 0, 'f', 1);
    result.eligible = result.blockers.isEmpty();
    return result;
}

KalshiReplayResult KalshiAutoEngine::replay(
    const QVector<KalshiReplayFrame>& frames,
    const QHash<QString, double>& final_settlement_by_event,
    const KalshiPortfolioConstraints& constraints) {
    KalshiReplayResult result;
    QSet<QString> traded_events;
    double equity = 0.0;
    double peak = 0.0;
    for (const auto& frame : frames) {
        ++result.frames;
        KalshiAutoContext context = frame.context;
        context.decision_ts_ms = frame.ts_ms;
        const auto surface = build_surface(frame.markets, frame.books, context);
        const auto plan = optimize(surface, context, constraints);
        result.plans.append(plan);
        ++result.decisions;
        if (plan.legs.isEmpty())
            continue;
        const QString event = surface.isEmpty() ? QString() : surface.first().event_ticker;
        if (event.isEmpty() || traded_events.contains(event) || !final_settlement_by_event.contains(event))
            continue;
        traded_events.insert(event);
        const double settlement = final_settlement_by_event.value(event);
        double pnl = 0.0;
        for (const auto& leg : plan.legs)
            pnl += leg_pnl(leg, settlement);
        result.net_pnl += pnl;
        equity += pnl;
        peak = std::max(peak, equity);
        result.max_drawdown = std::max(result.max_drawdown, peak - equity);
        ++result.trades;
        pnl > 0.0 ? ++result.wins : ++result.losses;
    }
    return result;
}

QStringList KalshiAutoEngine::compatibility_issues(const pr::PredictionMarket& market) {
    QStringList issues;
    if (market.key.market_id.isEmpty()) issues << QStringLiteral("missing market ticker");
    if (market.extras.value(QStringLiteral("price_level_structure")).toString().isEmpty())
        issues << QStringLiteral("missing price_level_structure");
    if (!market.extras.contains(QStringLiteral("fractional_trading_enabled")))
        issues << QStringLiteral("missing fractional_trading_enabled");
    if (!market.extras.contains(QStringLiteral("yes_ask_dollars")) ||
        !market.extras.contains(QStringLiteral("no_ask_dollars")))
        issues << QStringLiteral("missing fixed-point dollar asks");
    const QString status = market.extras.value(QStringLiteral("status")).toString().toLower();
    if (status.isEmpty()) issues << QStringLiteral("missing lifecycle status");
    return issues;
}

QJsonObject KalshiAutoEngine::surface_to_json(const QVector<KalshiSurfacePoint>& surface) {
    QJsonArray rows;
    for (const auto& point : surface) rows.append(point_json(point));
    return QJsonObject{{"surface", rows}, {"contracts", rows.size()}};
}

QJsonObject KalshiAutoEngine::plan_to_json(const KalshiPortfolioPlan& plan) {
    QJsonArray legs;
    for (const auto& leg : plan.legs) {
        legs.append(QJsonObject{{"ticker", leg.ticker}, {"side", leg.side}, {"kind", leg.kind},
                                {"floor", leg.floor}, {"cap", leg.cap}, {"contracts", leg.contracts},
                                {"entry_price", leg.entry_price}, {"fair_probability", leg.fair_probability},
                                {"entry_fee", leg.entry_fee}, {"expected_profit", leg.expected_profit}});
    }
    QJsonArray curve;
    for (const auto& point : plan.payoff_curve)
        curve.append(QJsonObject{{"settlement_price", point.settlement_price}, {"pnl", point.pnl}});
    QJsonArray blockers;
    for (const auto& blocker : plan.blockers) blockers.append(blocker);
    return QJsonObject{{"verdict", plan.verdict}, {"legs", legs}, {"total_cost", plan.total_cost},
                       {"expected_profit", plan.expected_profit}, {"expected_pnl", plan.expected_pnl},
                       {"worst_case_pnl", plan.worst_case_pnl}, {"best_case_pnl", plan.best_case_pnl},
                       {"payoff_curve", curve}, {"blockers", blockers}, {"paper_only", true}};
}

QJsonObject KalshiAutoEngine::replay_to_json(const KalshiReplayResult& replay) {
    return QJsonObject{{"frames", replay.frames}, {"decisions", replay.decisions},
                       {"trades", replay.trades}, {"wins", replay.wins}, {"losses", replay.losses},
                       {"net_pnl", replay.net_pnl}, {"max_drawdown", replay.max_drawdown},
                       {"paper_only", true}};
}

} // namespace openmarketterminal::services::edge_radar
