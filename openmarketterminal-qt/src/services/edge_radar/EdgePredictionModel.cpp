#include "services/edge_radar/EdgePredictionModel.h"

#include <QDateTime>
#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace openmarketterminal::services::edge_radar {
namespace {

double clamp01(double v) {
    if (!std::isfinite(v))
        return 0.5;
    return std::clamp(v, 0.01, 0.99);
}

double logit(double p) {
    p = clamp01(p);
    return std::log(p / (1.0 - p));
}

double sigmoid(double x) {
    if (x < -40.0)
        return 0.0;
    if (x > 40.0)
        return 1.0;
    return 1.0 / (1.0 + std::exp(-x));
}

double weight_value(const QJsonObject& weights, const QString& key, double fallback = 0.0) {
    return weights.value(key).toDouble(fallback);
}

double normalized_time_left(const EdgePredictionFeatures& f) {
    const int horizon = std::max(1, EdgePredictionModel::horizon_seconds(f.horizon));
    if (f.seconds_left < 0)
        return 0.5;
    return std::clamp(static_cast<double>(f.seconds_left) / static_cast<double>(horizon), 0.0, 1.0);
}

double feature_value(const EdgePredictionFeatures& f, const QString& key) {
    if (key == QStringLiteral("anchor"))
        return f.btc_anchor_probability - 0.5;
    if (key == QStringLiteral("move_5s"))
        return f.move_5s_pct;
    if (key == QStringLiteral("move_15s"))
        return f.move_15s_pct;
    if (key == QStringLiteral("move_60s"))
        return f.move_60s_pct;
    if (key == QStringLiteral("liquidity"))
        return f.liquidity_score - 0.5;
    if (key == QStringLiteral("time_left"))
        return normalized_time_left(f) - 0.5;
    if (key == QStringLiteral("context_5m"))
        return (f.context_5m_probability - 0.5) * f.context_5m_confidence;
    if (key == QStringLiteral("context_15m"))
        return (f.context_15m_probability - 0.5) * f.context_15m_confidence;
    if (key == QStringLiteral("context_1h"))
        return (f.context_1h_probability - 0.5) * f.context_1h_confidence;
    if (key == QStringLiteral("context_daily"))
        return (f.context_daily_probability - 0.5) * f.context_daily_confidence;
    if (key == QStringLiteral("lower_consensus"))
        return f.lower_horizon_consensus;
    if (key == QStringLiteral("higher_consensus"))
        return f.higher_horizon_consensus;
    return 0.0;
}

double observation_feature(const EdgePredictionObservation& o, const QString& key) {
    EdgePredictionFeatures f;
    f.horizon = o.horizon;
    f.btc_anchor_probability = o.btc_anchor_probability;
    f.move_5s_pct = o.move_5s_pct;
    f.move_15s_pct = o.move_15s_pct;
    f.move_60s_pct = o.move_60s_pct;
    f.liquidity_score = o.liquidity_score;
    f.seconds_left = o.seconds_left;
    f.decision_ts = o.observed_at;
    // Offline observations do not carry timestamped cross-horizon model
    // snapshots. Looking them up here would be both an invalid substitute
    // for historical context and millions of avoidable SQLite reads during a
    // backfill. Live estimates still build their context at decision time.
    return feature_value(f, key);
}

double fit_weight(const QVector<EdgePredictionObservation>& rows, const QString& key, double mean_y) {
    double mean_x = 0.0;
    for (const auto& r : rows)
        mean_x += observation_feature(r, key);
    mean_x /= static_cast<double>(std::max<qsizetype>(1, rows.size()));

    double cov = 0.0;
    double var = 0.0;
    for (const auto& r : rows) {
        const double x = observation_feature(r, key) - mean_x;
        const double y = static_cast<double>(r.outcome) - mean_y;
        cov += x * y;
        var += x * x;
    }
    if (var <= 1e-9)
        return 0.0;
    return std::clamp((cov / var) * 3.0, -8.0, 8.0);
}

} // namespace

QString EdgePredictionModel::normalize_horizon(QString horizon) {
    horizon = horizon.trimmed().toLower();
    horizon.replace(QStringLiteral("_"), QStringLiteral("-"));
    if (horizon == QStringLiteral("5") || horizon == QStringLiteral("5min") ||
        horizon == QStringLiteral("5-min") || horizon == QStringLiteral("five-minute"))
        return QStringLiteral("5m");
    if (horizon == QStringLiteral("15") || horizon == QStringLiteral("15min") ||
        horizon == QStringLiteral("15-min") || horizon == QStringLiteral("quarter-hour"))
        return QStringLiteral("15m");
    if (horizon == QStringLiteral("60m") || horizon == QStringLiteral("1hr") ||
        horizon == QStringLiteral("hour") || horizon == QStringLiteral("hourly"))
        return QStringLiteral("1h");
    if (horizon == QStringLiteral("1d") || horizon == QStringLiteral("day") ||
        horizon == QStringLiteral("daily") || horizon == QStringLiteral("24h"))
        return QStringLiteral("daily");
    if (horizon == QStringLiteral("all"))
        return horizon;
    return horizon.isEmpty() ? QStringLiteral("5m") : horizon;
}

QStringList EdgePredictionModel::supported_horizons() {
    return {QStringLiteral("5m"), QStringLiteral("15m"), QStringLiteral("1h"), QStringLiteral("daily")};
}

int EdgePredictionModel::horizon_seconds(const QString& horizon) {
    const QString h = normalize_horizon(horizon);
    if (h == QStringLiteral("5m"))
        return 300;
    if (h == QStringLiteral("15m"))
        return 900;
    if (h == QStringLiteral("1h"))
        return 3600;
    if (h == QStringLiteral("daily"))
        return 86400;
    return 300;
}

Result<EdgePredictionTrainResult>
EdgePredictionModel::train(const QString& symbol, const QString& horizon, int minimum_samples) {
    EdgePredictionTrainResult out;
    const QString sym = symbol.trimmed().isEmpty() ? QStringLiteral("BTC") : symbol.trimmed().toUpper();
    const QString h = normalize_horizon(horizon);
    auto rows_r = EdgePredictionModelRepository::instance().list_observations(sym, h, true, 100000);
    if (rows_r.is_err())
        return Result<EdgePredictionTrainResult>::err(rows_r.error());

    const auto rows = rows_r.value();
    out.model.symbol = sym;
    out.model.horizon = h;
    out.model.sample_count = rows.size();
    for (const auto& r : rows)
        out.model.positive_count += r.outcome == 1 ? 1 : 0;
    out.model.base_rate = rows.isEmpty() ? 0.5
                                         : static_cast<double>(out.model.positive_count) /
                                               static_cast<double>(rows.size());
    out.readiness = rows.size() >= minimum_samples ? QStringLiteral("ready")
                                                   : QStringLiteral("collecting");

    QJsonObject weights;
    weights.insert(QStringLiteral("minimum_samples"), minimum_samples);
    weights.insert(QStringLiteral("intercept"), logit(out.model.base_rate));
    weights.insert(QStringLiteral("anchor"), 0.0);
    weights.insert(QStringLiteral("move_5s"), 0.0);
    weights.insert(QStringLiteral("move_15s"), 0.0);
    weights.insert(QStringLiteral("move_60s"), 0.0);
    weights.insert(QStringLiteral("liquidity"), 0.0);
    weights.insert(QStringLiteral("time_left"), 0.0);
    weights.insert(QStringLiteral("context_5m"), 0.0);
    weights.insert(QStringLiteral("context_15m"), 0.0);
    weights.insert(QStringLiteral("context_1h"), 0.0);
    weights.insert(QStringLiteral("context_daily"), 0.0);
    weights.insert(QStringLiteral("lower_consensus"), 0.0);
    weights.insert(QStringLiteral("higher_consensus"), 0.0);
    if (!rows.isEmpty()) {
        weights.insert(QStringLiteral("anchor"), fit_weight(rows, QStringLiteral("anchor"), out.model.base_rate));
        weights.insert(QStringLiteral("move_5s"), fit_weight(rows, QStringLiteral("move_5s"), out.model.base_rate));
        weights.insert(QStringLiteral("move_15s"), fit_weight(rows, QStringLiteral("move_15s"), out.model.base_rate));
        weights.insert(QStringLiteral("move_60s"), fit_weight(rows, QStringLiteral("move_60s"), out.model.base_rate));
        weights.insert(QStringLiteral("liquidity"), fit_weight(rows, QStringLiteral("liquidity"), out.model.base_rate));
        weights.insert(QStringLiteral("time_left"), fit_weight(rows, QStringLiteral("time_left"), out.model.base_rate));
        weights.insert(QStringLiteral("context_5m"), fit_weight(rows, QStringLiteral("context_5m"), out.model.base_rate));
        weights.insert(QStringLiteral("context_15m"), fit_weight(rows, QStringLiteral("context_15m"), out.model.base_rate));
        weights.insert(QStringLiteral("context_1h"), fit_weight(rows, QStringLiteral("context_1h"), out.model.base_rate));
        weights.insert(QStringLiteral("context_daily"), fit_weight(rows, QStringLiteral("context_daily"), out.model.base_rate));
        weights.insert(QStringLiteral("lower_consensus"), fit_weight(rows, QStringLiteral("lower_consensus"), out.model.base_rate));
        weights.insert(QStringLiteral("higher_consensus"), fit_weight(rows, QStringLiteral("higher_consensus"), out.model.base_rate));
    }

    double brier = 0.0;
    for (const auto& r : rows) {
        EdgePredictionFeatures f;
        f.symbol = r.symbol;
        f.horizon = r.horizon;
        f.market_probability = r.market_probability;
        f.btc_anchor_probability = r.btc_anchor_probability;
        f.move_5s_pct = r.move_5s_pct;
        f.move_15s_pct = r.move_15s_pct;
        f.move_60s_pct = r.move_60s_pct;
        f.liquidity_score = r.liquidity_score;
        f.seconds_left = r.seconds_left;
        f.decision_ts = r.observed_at;
        double score = weight_value(weights, QStringLiteral("intercept"));
        for (const QString& key : {QStringLiteral("anchor"), QStringLiteral("move_5s"),
                                   QStringLiteral("move_15s"), QStringLiteral("move_60s"),
                                   QStringLiteral("liquidity"), QStringLiteral("time_left"),
                                   QStringLiteral("context_5m"), QStringLiteral("context_15m"),
                                   QStringLiteral("context_1h"), QStringLiteral("context_daily"),
                                   QStringLiteral("lower_consensus"), QStringLiteral("higher_consensus")})
            score += weight_value(weights, key) * feature_value(f, key);
        const double p = sigmoid(score);
        const double e = p - static_cast<double>(r.outcome);
        brier += e * e;
    }
    out.model.brier_score = rows.isEmpty() ? 0.0 : brier / static_cast<double>(rows.size());
    out.model.weights = weights;
    out.message = out.readiness == QStringLiteral("ready")
                      ? QStringLiteral("local model trained")
                      : QStringLiteral("collecting observations before model is trusted");

    auto save = EdgePredictionModelRepository::instance().upsert_model(out.model);
    if (save.is_err())
        return Result<EdgePredictionTrainResult>::err(save.error());
    auto saved = EdgePredictionModelRepository::instance().get_model(sym, h);
    if (saved.is_ok())
        out.model = saved.value();
    out.trained = true;
    return Result<EdgePredictionTrainResult>::ok(out);
}

double EdgePredictionModel::heuristic_probability(const EdgePredictionFeatures& features) {
    const double anchor = features.btc_anchor_probability - 0.5;
    const double impulse = features.horizon == QStringLiteral("daily")
                               ? features.move_60s_pct * 0.25
                           : features.horizon == QStringLiteral("1h")
                               ? features.move_60s_pct * 0.50
                           : features.horizon == QStringLiteral("15m")
                               ? features.move_15s_pct * 0.80 + features.move_60s_pct * 0.25
                               : features.move_5s_pct * 0.60 + features.move_15s_pct * 0.80;
    const double context = features.lower_horizon_consensus * 0.08 + features.higher_horizon_consensus * 0.06;
    return clamp01(0.5 + anchor * 0.70 + impulse * 0.02 + context);
}

EdgePredictionFeatures EdgePredictionModel::build_features(const EdgePredictionFeatures& base,
                                                           qint64 decision_ts) {
    EdgePredictionFeatures out = base;
    out.symbol = out.symbol.trimmed().isEmpty() ? QStringLiteral("BTC") : out.symbol.trimmed().toUpper();
    out.horizon = normalize_horizon(out.horizon);
    out.decision_ts = decision_ts > 0 ? decision_ts : QDateTime::currentMSecsSinceEpoch();
    out.feature_sources.clear();
    out.leakage_rejections.clear();

    const QMap<QString, int> rank{{QStringLiteral("5m"), 0},
                                  {QStringLiteral("15m"), 1},
                                  {QStringLiteral("1h"), 2},
                                  {QStringLiteral("daily"), 3}};
    const int current_rank = rank.value(out.horizon, 0);
    double lower_sum = 0.0;
    double lower_weight = 0.0;
    double higher_sum = 0.0;
    double higher_weight = 0.0;

    for (const auto& h : supported_horizons()) {
        if (h == out.horizon)
            continue;
        auto r = EdgePredictionModelRepository::instance().latest_model_output(out.symbol, h, out.decision_ts);
        if (r.is_err())
            continue;
        const auto model = r.value();
        if (model.as_of > out.decision_ts) {
            out.leakage_rejections << QStringLiteral("%1 output after decision timestamp rejected").arg(h);
            continue;
        }
        const double p = clamp01(model.probability);
        const double c = std::clamp(model.confidence, 0.0, 1.0);
        if (h == QStringLiteral("5m")) {
            out.context_5m_probability = p;
            out.context_5m_confidence = c;
        } else if (h == QStringLiteral("15m")) {
            out.context_15m_probability = p;
            out.context_15m_confidence = c;
        } else if (h == QStringLiteral("1h")) {
            out.context_1h_probability = p;
            out.context_1h_confidence = c;
        } else if (h == QStringLiteral("daily")) {
            out.context_daily_probability = p;
            out.context_daily_confidence = c;
        }
        out.feature_sources << QStringLiteral("%1 p=%2% confidence=%3% as_of=%4")
                                    .arg(h)
                                    .arg(p * 100.0, 0, 'f', 2)
                                    .arg(c * 100.0, 0, 'f', 1)
                                    .arg(QDateTime::fromMSecsSinceEpoch(model.as_of).toString(Qt::ISODate));
        const int rnk = rank.value(h, current_rank);
        if (rnk < current_rank) {
            lower_sum += (p - 0.5) * c;
            lower_weight += c;
        } else if (rnk > current_rank) {
            higher_sum += (p - 0.5) * c;
            higher_weight += c;
        }
    }
    out.lower_horizon_consensus = lower_weight > 0.0 ? std::clamp(lower_sum / lower_weight, -0.5, 0.5) : 0.0;
    out.higher_horizon_consensus = higher_weight > 0.0 ? std::clamp(higher_sum / higher_weight, -0.5, 0.5) : 0.0;
    return out;
}

EdgePredictionEstimate EdgePredictionModel::estimate(const EdgePredictionFeatures& input,
                                                     int minimum_samples) {
    EdgePredictionFeatures f = build_features(input, input.decision_ts);
    f.symbol = f.symbol.trimmed().isEmpty() ? QStringLiteral("BTC") : f.symbol.trimmed().toUpper();
    f.horizon = normalize_horizon(f.horizon);
    EdgePredictionEstimate out;
    out.symbol = f.symbol;
    out.horizon = f.horizon;
    out.decision_ts = f.decision_ts;
    out.feature_sources = f.feature_sources;
    out.leakage_rejections = f.leakage_rejections;
    out.lower_horizon_consensus = f.lower_horizon_consensus;
    out.higher_horizon_consensus = f.higher_horizon_consensus;
    if (!f.feature_sources.isEmpty()) {
        out.freshest_context_at = 0;
        for (const auto& src : f.feature_sources) {
            const int idx = src.indexOf(QStringLiteral("as_of="));
            if (idx >= 0) {
                const QDateTime dt = QDateTime::fromString(src.mid(idx + 6), Qt::ISODate);
                if (dt.isValid())
                    out.freshest_context_at = std::max(out.freshest_context_at, dt.toMSecsSinceEpoch());
            }
        }
        out.freshness_ms = out.freshest_context_at > 0 ? static_cast<int>(out.decision_ts - out.freshest_context_at) : -1;
    }

    auto model_r = EdgePredictionModelRepository::instance().get_model(f.symbol, f.horizon);
    if (model_r.is_ok()) {
        const auto model = model_r.value();
        out.sample_count = model.sample_count;
        out.model_ready = model.sample_count >= minimum_samples;
        out.readiness = out.model_ready ? QStringLiteral("ready") : QStringLiteral("collecting");
        double score = weight_value(model.weights, QStringLiteral("intercept"), logit(model.base_rate));
        for (const QString& key : {QStringLiteral("anchor"), QStringLiteral("move_5s"),
                                   QStringLiteral("move_15s"), QStringLiteral("move_60s"),
                                   QStringLiteral("liquidity"), QStringLiteral("time_left"),
                                   QStringLiteral("context_5m"), QStringLiteral("context_15m"),
                                   QStringLiteral("context_1h"), QStringLiteral("context_daily"),
                                   QStringLiteral("lower_consensus"), QStringLiteral("higher_consensus")})
            score += weight_value(model.weights, key) * feature_value(f, key);
        out.probability = sigmoid(score);
        out.confidence = std::clamp(std::sqrt(static_cast<double>(model.sample_count) /
                                              static_cast<double>(std::max(1, minimum_samples * 6))),
                                    0.10, 0.90);
        out.probability_source = out.model_ready ? QStringLiteral("local-trained")
                                                 : QStringLiteral("local-warming");
        if (!out.model_ready)
            out.warnings << QStringLiteral("not enough settled observations for trusted model");
        out.rationale = QStringLiteral("%1 %2 local model: p=%3%, samples=%4, base=%5%, brier=%6")
                            .arg(f.symbol)
                            .arg(f.horizon)
                            .arg(out.probability * 100.0, 0, 'f', 2)
                            .arg(model.sample_count)
                            .arg(model.base_rate * 100.0, 0, 'f', 2)
                            .arg(model.brier_score, 0, 'f', 4);
    } else {
        out.probability = heuristic_probability(f);
        out.confidence = 0.10;
        out.probability_source = QStringLiteral("heuristic-fallback");
        out.readiness = QStringLiteral("collecting");
        out.warnings << QStringLiteral("no local trained model for this symbol/horizon yet");
        out.rationale = QStringLiteral("%1 %2 heuristic fallback: p=%3%")
                            .arg(f.symbol)
                            .arg(f.horizon)
                            .arg(out.probability * 100.0, 0, 'f', 2);
    }
    out.direction = out.probability > 0.515 ? QStringLiteral("bullish")
                  : out.probability < 0.485 ? QStringLiteral("bearish")
                                            : QStringLiteral("neutral");
    out.net_edge = out.probability - f.market_probability - f.spread_cost - f.fee_cost;
    return out;
}

EdgeMetaGateDecision EdgePredictionModel::meta_gate(const EdgePredictionFeatures& features,
                                                    int minimum_samples) {
    EdgeMetaGateDecision d;
    d.primary = estimate(features, minimum_samples);
    d.symbol = d.primary.symbol;
    d.horizon = d.primary.horizon;
    d.market_probability = features.market_probability;
    d.net_edge = d.primary.net_edge;
    d.decision_ts = d.primary.decision_ts;

    auto outputs = EdgePredictionModelRepository::instance().list_model_outputs(d.symbol, d.decision_ts, 32);
    if (outputs.is_ok()) {
        for (const auto& o : outputs.value()) {
            if (o.as_of <= d.decision_ts)
                d.context_outputs.push_back(o);
        }
    }

    if (!d.primary.model_ready) {
        d.call = QStringLiteral("NOT ENOUGH DATA");
        d.reasons << QStringLiteral("primary horizon model is not trusted yet");
    } else if (features.seconds_left >= 0 && features.seconds_left < 15) {
        d.call = QStringLiteral("TOO LATE");
        d.reasons << QStringLiteral("inside final entry guard");
    } else if (features.market_probability >= 0.90) {
        d.call = QStringLiteral("BAD PRICE");
        d.reasons << QStringLiteral("market price above max practical entry");
    } else if (d.net_edge >= 0.05 && d.primary.higher_horizon_consensus >= -0.03) {
        d.call = QStringLiteral("TRADE CANDIDATE");
        d.reasons << QStringLiteral("positive net edge and no strong higher-horizon conflict");
    } else if (d.net_edge >= 0.01) {
        d.call = QStringLiteral("WATCH");
        d.reasons << QStringLiteral("some edge remains but conflicts or margin are not clean");
    } else {
        d.call = QStringLiteral("NO TRADE");
        d.reasons << QStringLiteral("net edge does not clear hurdle");
    }
    if (!d.primary.leakage_rejections.isEmpty())
        d.reasons << d.primary.leakage_rejections;
    d.explanation = QStringLiteral(
        "%1 %2: model=%3%, market=%4c, net=%5%, source=%6, samples=%7, decision_ts=%8")
                        .arg(d.symbol)
                        .arg(d.horizon)
                        .arg(d.primary.probability * 100.0, 0, 'f', 2)
                        .arg(d.market_probability * 100.0, 0, 'f', 1)
                        .arg(d.net_edge * 100.0, 0, 'f', 2)
                        .arg(d.primary.probability_source)
                        .arg(d.primary.sample_count)
                        .arg(QDateTime::fromMSecsSinceEpoch(d.decision_ts).toString(Qt::ISODate));
    return d;
}

Result<void> EdgePredictionModel::publish_estimate(const EdgePredictionEstimate& estimate,
                                                   const QString& source) {
    EdgePredictionModelOutput out;
    out.symbol = estimate.symbol;
    out.horizon = estimate.horizon;
    out.direction = estimate.direction;
    out.readiness = estimate.readiness;
    out.source = source;
    out.probability = estimate.probability;
    out.confidence = estimate.confidence;
    out.calibration_score = 0.0;
    out.sample_count = estimate.sample_count;
    out.as_of = estimate.decision_ts > 0 ? estimate.decision_ts : QDateTime::currentMSecsSinceEpoch();
    out.trained_at = out.as_of;
    auto saved = EdgePredictionModelRepository::instance().publish_model_output(out);
    if (saved.is_err())
        return Result<void>::err(saved.error());
    return Result<void>::ok();
}

bool EdgePredictionModel::no_lookahead_selftest(QString* details) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString symbol = QStringLiteral("LEAKTEST%1").arg(now);
    EdgePredictionModelOutput past;
    past.symbol = symbol;
    past.horizon = QStringLiteral("15m");
    past.probability = 0.61;
    past.confidence = 0.8;
    past.direction = QStringLiteral("bullish");
    past.as_of = now - 1000;
    past.sample_count = 100;
    EdgePredictionModelOutput future = past;
    future.probability = 0.99;
    future.as_of = now + 1000;
    auto a = EdgePredictionModelRepository::instance().publish_model_output(past);
    auto b = EdgePredictionModelRepository::instance().publish_model_output(future);
    if (a.is_err() || b.is_err()) {
        if (details)
            *details = QStringLiteral("failed to create test outputs");
        return false;
    }

    EdgePredictionFeatures f;
    f.symbol = symbol;
    f.horizon = QStringLiteral("5m");
    f.market_probability = 0.50;
    const auto built = build_features(f, now);
    const bool ok = built.context_15m_probability < 0.70 && built.context_15m_probability > 0.55;
    if (details) {
        *details = ok
                       ? QStringLiteral("past output used and future output excluded")
                       : QStringLiteral("future output leaked into feature builder");
    }
    return ok;
}

QJsonObject EdgePredictionModel::estimate_to_json(const EdgePredictionEstimate& e) {
    QJsonArray warnings;
    for (const auto& w : e.warnings)
        warnings.append(w);
    return QJsonObject{{"symbol", e.symbol},
                       {"horizon", e.horizon},
                       {"probability_source", e.probability_source},
                       {"readiness", e.readiness},
                       {"direction", e.direction},
                       {"rationale", e.rationale},
                       {"warnings", warnings},
                       {"feature_sources", QJsonArray::fromStringList(e.feature_sources)},
                       {"leakage_rejections", QJsonArray::fromStringList(e.leakage_rejections)},
                       {"probability", e.probability},
                       {"confidence", e.confidence},
                       {"net_edge", e.net_edge},
                       {"lower_horizon_consensus", e.lower_horizon_consensus},
                       {"higher_horizon_consensus", e.higher_horizon_consensus},
                       {"decision_ts", QString::number(e.decision_ts)},
                       {"freshness_ms", e.freshness_ms},
                       {"sample_count", e.sample_count},
                       {"model_ready", e.model_ready}};
}

QJsonObject EdgePredictionModel::meta_gate_to_json(const EdgeMetaGateDecision& d) {
    QJsonArray reasons;
    for (const auto& r : d.reasons)
        reasons.append(r);
    QJsonArray outputs;
    for (const auto& o : d.context_outputs)
        outputs.append(edge_prediction_model_output_to_json(o));
    return QJsonObject{{"call", d.call},
                       {"symbol", d.symbol},
                       {"horizon", d.horizon},
                       {"explanation", d.explanation},
                       {"reasons", reasons},
                       {"primary", estimate_to_json(d.primary)},
                       {"context_outputs", outputs},
                       {"market_probability", d.market_probability},
                       {"net_edge", d.net_edge},
                       {"decision_ts", QString::number(d.decision_ts)}};
}

QJsonObject EdgePredictionModel::train_result_to_json(const EdgePredictionTrainResult& r) {
    return QJsonObject{{"trained", r.trained},
                       {"readiness", r.readiness},
                       {"message", r.message},
                       {"model", edge_prediction_model_to_json(r.model)}};
}

} // namespace openmarketterminal::services::edge_radar
