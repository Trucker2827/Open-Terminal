#include "services/edge_radar/KalshiUniversalEdgeModel.h"

#include <QDateTime>
#include <QJsonArray>
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
    if (v > 1.0)
        v /= 100.0;
    return std::clamp(v, 0.0, 1.0);
}

QString normalized_text(const QString& value) {
    QString s = value.toUpper();
    s.replace('-', ' ');
    s.replace('_', ' ');
    return s;
}

bool has_any(const QString& text, const QStringList& words) {
    for (const auto& word : words) {
        const QRegularExpression re(QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(word)));
        if (text.contains(re))
            return true;
    }
    return false;
}

QString pct(double value) {
    return QStringLiteral("%1%").arg(value * 100.0, 0, 'f', 2);
}

double variant_double(const QVariantMap& extras, const QString& key) {
    bool ok = false;
    const double v = extras.value(key).toDouble(&ok);
    return ok ? v : 0.0;
}

} // namespace

QString KalshiUniversalEdgeModel::classify_family(const pr::PredictionMarket& market) {
    const QString text = normalized_text(market.category + QStringLiteral(" ") + market.key.market_id +
                                         QStringLiteral(" ") + market.key.event_id +
                                         QStringLiteral(" ") + market.question +
                                         QStringLiteral(" ") + market.description +
                                         QStringLiteral(" ") + market.tags.join(' ') +
                                         QStringLiteral(" ") + market.extras.value(QStringLiteral("series_ticker")).toString());
    if (has_any(text, {QStringLiteral("BTC"), QStringLiteral("BITCOIN"), QStringLiteral("ETH"),
                       QStringLiteral("ETHEREUM"), QStringLiteral("SOLANA"), QStringLiteral("SOL"),
                       QStringLiteral("DOGE"), QStringLiteral("XRP"), QStringLiteral("CRYPTO")}))
        return QStringLiteral("crypto");
    if (has_any(text, {QStringLiteral("CPI"), QStringLiteral("PCE"), QStringLiteral("INFLATION"),
                       QStringLiteral("PRICE INDEX")}))
        return QStringLiteral("inflation");
    if (has_any(text, {QStringLiteral("FED"), QStringLiteral("FOMC"), QStringLiteral("RATE CUT"),
                       QStringLiteral("RATE HIKE"), QStringLiteral("INTEREST RATE"), QStringLiteral("POWELL")}))
        return QStringLiteral("fed-rates");
    if (has_any(text, {QStringLiteral("JOBS"), QStringLiteral("PAYROLL"), QStringLiteral("UNEMPLOYMENT"),
                       QStringLiteral("NFP"), QStringLiteral("JOBLESS"), QStringLiteral("JOLTS")}))
        return QStringLiteral("jobs");
    if (has_any(text, {QStringLiteral("GDP"), QStringLiteral("RETAIL SALES"), QStringLiteral("ISM"),
                       QStringLiteral("PMI"), QStringLiteral("CONSUMER SENTIMENT"), QStringLiteral("HOUSING")}))
        return QStringLiteral("macro");
    if (has_any(text, {QStringLiteral("TEMP"), QStringLiteral("TEMPERATURE"), QStringLiteral("RAIN"),
                       QStringLiteral("SNOW"), QStringLiteral("HURRICANE"), QStringLiteral("WEATHER"),
                       QStringLiteral("WIND"), QStringLiteral("TORNADO")}))
        return QStringLiteral("weather");
    if (has_any(text, {QStringLiteral("ELECTION"), QStringLiteral("PRESIDENT"), QStringLiteral("SENATE"),
                       QStringLiteral("HOUSE"), QStringLiteral("GOVERNOR"), QStringLiteral("MAYOR"),
                       QStringLiteral("APPROVAL"), QStringLiteral("POLL")}))
        return QStringLiteral("politics");
    if (has_any(text, {QStringLiteral("EARNINGS"), QStringLiteral("REVENUE"), QStringLiteral("EPS"),
                       QStringLiteral("STOCK"), QStringLiteral("NASDAQ"), QStringLiteral("S&P"),
                       QStringLiteral("DOW"), QStringLiteral("TESLA"), QStringLiteral("NVIDIA"),
                       QStringLiteral("AAPL"), QStringLiteral("NVDA")}))
        return QStringLiteral("equity");
    if (has_any(text, {QStringLiteral("OIL"), QStringLiteral("GAS"), QStringLiteral("GOLD"),
                       QStringLiteral("SILVER"), QStringLiteral("COPPER"), QStringLiteral("COMMODITY")}))
        return QStringLiteral("commodities");
    if (has_any(text, {QStringLiteral("NFL"), QStringLiteral("NBA"), QStringLiteral("MLB"),
                       QStringLiteral("NHL"), QStringLiteral("SOCCER"), QStringLiteral("TENNIS"),
                       QStringLiteral("GAME"), QStringLiteral("CHAMPIONSHIP")}))
        return QStringLiteral("sports");
    if (has_any(text, {QStringLiteral("COURT"), QStringLiteral("SUPREME"), QStringLiteral("WAR"),
                       QStringLiteral("CEASEFIRE"), QStringLiteral("LAWSUIT"), QStringLiteral("BILL"),
                       QStringLiteral("ANNOUNCE"), QStringLiteral("LAUNCH")}))
        return QStringLiteral("news-event");
    return QStringLiteral("other");
}

QString KalshiUniversalEdgeModel::infer_time_horizon(const pr::PredictionMarket& market) {
    const QString text = normalized_text(market.question + QStringLiteral(" ") + market.key.market_id);
    if (has_any(text, {QStringLiteral("15 MIN"), QStringLiteral("FIFTEEN MIN")}))
        return QStringLiteral("15m");
    if (has_any(text, {QStringLiteral("1 HOUR"), QStringLiteral("ONE HOUR"), QStringLiteral("HOURLY")}))
        return QStringLiteral("1h");
    if (has_any(text, {QStringLiteral("TODAY"), QStringLiteral("DAILY")}))
        return QStringLiteral("daily");
    if (has_any(text, {QStringLiteral("THIS WEEK"), QStringLiteral("WEEKLY")}))
        return QStringLiteral("weekly");
    if (has_any(text, {QStringLiteral("THIS MONTH"), QStringLiteral("MONTHLY")}))
        return QStringLiteral("monthly");
    const QDateTime close = QDateTime::fromString(market.end_date_iso, Qt::ISODate);
    if (close.isValid()) {
        const qint64 secs = QDateTime::currentDateTimeUtc().secsTo(close.toUTC());
        if (secs <= 2 * 3600)
            return QStringLiteral("intraday");
        if (secs <= 48 * 3600)
            return QStringLiteral("daily");
        if (secs <= 14 * 24 * 3600)
            return QStringLiteral("weekly");
        if (secs <= 75 * 24 * 3600)
            return QStringLiteral("monthly");
    }
    return QStringLiteral("event");
}

double KalshiUniversalEdgeModel::yes_probability(const pr::PredictionMarket& market) {
    if (market.outcomes.isEmpty())
        return 0.0;
    for (const auto& outcome : market.outcomes) {
        if (normalized_text(outcome.name) == QStringLiteral("YES"))
            return clamp01(outcome.price);
    }
    return clamp01(market.outcomes.first().price);
}

double KalshiUniversalEdgeModel::spread_cost(const pr::PredictionMarket& market, double fallback) {
    const double bid = yes_probability(market);
    const double ask = variant_double(market.extras, QStringLiteral("yes_ask_dollars"));
    if (bid > 0.0 && ask > 0.0 && ask >= bid)
        return std::clamp(ask - bid, 0.0, 0.25);
    return fallback;
}

double KalshiUniversalEdgeModel::liquidity_score(double liquidity, double minimum) {
    if (liquidity <= 0.0)
        return minimum;
    return std::clamp(liquidity / 15000.0, minimum, 1.0);
}

QStringList KalshiUniversalEdgeModel::research_drivers_for_family(const QString& family) {
    if (family == QStringLiteral("crypto"))
        return {QStringLiteral("spot impulse"), QStringLiteral("cross-exchange freshness"),
                QStringLiteral("alt/BTC beta"), QStringLiteral("market spread")};
    if (family == QStringLiteral("inflation"))
        return {QStringLiteral("release calendar"), QStringLiteral("consensus estimate"),
                QStringLiteral("nowcast trend"), QStringLiteral("surprise distribution")};
    if (family == QStringLiteral("fed-rates"))
        return {QStringLiteral("FedWatch/futures implied odds"), QStringLiteral("FOMC calendar"),
                QStringLiteral("latest inflation/jobs inputs"), QStringLiteral("Fed speaker tone")};
    if (family == QStringLiteral("jobs"))
        return {QStringLiteral("BLS release calendar"), QStringLiteral("consensus estimate"),
                QStringLiteral("ADP/jobless/JOLTS lead-ins"), QStringLiteral("historical error bands")};
    if (family == QStringLiteral("macro"))
        return {QStringLiteral("official release calendar"), QStringLiteral("consensus estimate"),
                QStringLiteral("revision risk"), QStringLiteral("related high-frequency series")};
    if (family == QStringLiteral("weather"))
        return {QStringLiteral("NOAA/NWS forecast"), QStringLiteral("station/location mapping"),
                QStringLiteral("forecast update time"), QStringLiteral("settlement rule check")};
    if (family == QStringLiteral("politics"))
        return {QStringLiteral("polling average"), QStringLiteral("sample quality"),
                QStringLiteral("state/district mapping"), QStringLiteral("news/legal catalyst")};
    if (family == QStringLiteral("equity"))
        return {QStringLiteral("stock/option implied probability"), QStringLiteral("earnings calendar"),
                QStringLiteral("guidance/filings"), QStringLiteral("after-hours move")};
    if (family == QStringLiteral("commodities"))
        return {QStringLiteral("futures curve"), QStringLiteral("inventory/report calendar"),
                QStringLiteral("macro dollar/rates context"), QStringLiteral("event settlement source")};
    if (family == QStringLiteral("sports"))
        return {QStringLiteral("sportsbook implied odds"), QStringLiteral("injury/news feed"),
                QStringLiteral("line movement"), QStringLiteral("settlement timing")};
    if (family == QStringLiteral("news-event"))
        return {QStringLiteral("primary source timeline"), QStringLiteral("trusted news confirmation"),
                QStringLiteral("legal/policy calendar"), QStringLiteral("resolution wording")};
    return {QStringLiteral("primary source"), QStringLiteral("consensus/reference price"),
            QStringLiteral("settlement wording"), QStringLiteral("liquidity/spread")};
}

QStringList KalshiUniversalEdgeModel::data_sources_for_family(const QString& family) {
    if (family == QStringLiteral("crypto"))
        return {QStringLiteral("Coinbase/Kraken/Binance feeds"), QStringLiteral("Kalshi order book"),
                QStringLiteral("Polymarket/cross-market prices")};
    if (family == QStringLiteral("inflation"))
        return {QStringLiteral("BLS CPI"), QStringLiteral("BEA PCE"), QStringLiteral("FRED"),
                QStringLiteral("economic calendar/consensus")};
    if (family == QStringLiteral("fed-rates"))
        return {QStringLiteral("CME/Fed funds futures"), QStringLiteral("Federal Reserve calendar"),
                QStringLiteral("FRED rates"), QStringLiteral("news radar")};
    if (family == QStringLiteral("jobs"))
        return {QStringLiteral("BLS"), QStringLiteral("FRED"), QStringLiteral("ADP/jobless claims"),
                QStringLiteral("economic calendar/consensus")};
    if (family == QStringLiteral("macro"))
        return {QStringLiteral("FRED"), QStringLiteral("BEA/Census/ISM"), QStringLiteral("DB.NOMICS"),
                QStringLiteral("economic calendar/consensus")};
    if (family == QStringLiteral("weather"))
        return {QStringLiteral("NOAA/NWS"), QStringLiteral("weather.gov station data"),
                QStringLiteral("settlement source")};
    if (family == QStringLiteral("politics"))
        return {QStringLiteral("polling aggregators"), QStringLiteral("official election pages"),
                QStringLiteral("news radar")};
    if (family == QStringLiteral("equity"))
        return {QStringLiteral("market quotes"), QStringLiteral("options implied move"),
                QStringLiteral("SEC filings"), QStringLiteral("earnings calendar/news")};
    if (family == QStringLiteral("commodities"))
        return {QStringLiteral("futures quotes"), QStringLiteral("EIA/official reports"),
                QStringLiteral("macro/news radar")};
    if (family == QStringLiteral("sports"))
        return {QStringLiteral("sports schedule/results"), QStringLiteral("odds reference"),
                QStringLiteral("injury/news feed")};
    if (family == QStringLiteral("news-event"))
        return {QStringLiteral("primary source"), QStringLiteral("trusted news feeds"),
                QStringLiteral("official docket/calendar")};
    return {QStringLiteral("primary source"), QStringLiteral("reference market"),
            QStringLiteral("news radar")};
}

KalshiUniversalSignal KalshiUniversalEdgeModel::score_market(const pr::PredictionMarket& market,
                                                             const KalshiUniversalOptions& options,
                                                             double model_probability,
                                                             double confidence_override,
                                                             const QString& probability_source) {
    KalshiUniversalSignal out;
    out.market_id = market.key.market_id;
    out.event_id = market.key.event_id;
    out.question = market.question;
    out.family = classify_family(market);
    out.time_horizon = infer_time_horizon(market);
    out.market_probability = yes_probability(market);
    out.spread_cost = spread_cost(market, options.spread_cost_fallback);
    out.fee_cost = options.fee_cost;
    out.liquidity_score = liquidity_score(market.liquidity, options.minimum_liquidity_score);
    out.research_drivers = research_drivers_for_family(out.family);
    out.data_sources = data_sources_for_family(out.family);
    out.tags = {out.family, out.time_horizon};
    out.is_valid = !out.market_id.isEmpty() && out.market_probability > 0.0 && !market.closed;

    const bool external_model = model_probability >= 0.0;
    if (external_model) {
        out.model_probability = clamp01(model_probability);
        out.probability_source = probability_source.isEmpty()
                                     ? QStringLiteral("external-model")
                                     : probability_source;
    } else if (options.allow_market_implied_baseline) {
        out.model_probability = out.market_probability;
        out.probability_source = QStringLiteral("market-implied-baseline");
    } else {
        out.model_probability = 0.0;
        out.probability_source = QStringLiteral("missing-model");
    }

    if (confidence_override >= 0.0) {
        out.confidence = clamp01(confidence_override);
    } else if (external_model) {
        out.confidence = 0.60;
    } else {
        out.confidence = 0.35;
        if (out.family == QStringLiteral("crypto") || out.family == QStringLiteral("weather") ||
            out.family == QStringLiteral("macro") || out.family == QStringLiteral("inflation") ||
            out.family == QStringLiteral("fed-rates") || out.family == QStringLiteral("jobs"))
            out.confidence = 0.42;
    }

    const EdgeScore edge = EdgeRadarService::evaluate(
        {out.market_probability, out.model_probability, out.spread_cost,
         out.fee_cost, out.liquidity_score, out.confidence});
    out.raw_edge = edge.raw_edge;
    out.edge_after_cost = edge.edge_after_cost;
    out.gate_edge = edge.edge_after_cost - options.safety_buffer;
    out.side = edge.side;

    QStringList reject;
    if (!out.is_valid)
        reject << QStringLiteral("market is closed or missing probability");
    if (!external_model)
        reject << QStringLiteral("no independent probability model attached");
    if (out.gate_edge < options.minimum_net_edge)
        reject << QStringLiteral("net edge %1 below %2 hurdle after safety buffer")
                      .arg(pct(out.gate_edge), pct(options.minimum_net_edge));
    if (out.liquidity_score < options.minimum_liquidity_score)
        reject << QStringLiteral("liquidity score %1 below %2 hurdle")
                      .arg(pct(out.liquidity_score), pct(options.minimum_liquidity_score));
    if (out.spread_cost + out.fee_cost > 0.07)
        reject << QStringLiteral("spread/fee stack too expensive");
    if (out.confidence < 0.55)
        reject << QStringLiteral("research confidence below trade gate");

    out.passes_gate = reject.isEmpty();
    out.is_strong = out.passes_gate && out.gate_edge >= options.strong_net_edge;
    if (out.is_strong) {
        out.recommendation = QStringLiteral("strong");
        out.gate = QStringLiteral("pass");
    } else if (out.passes_gate) {
        out.recommendation = QStringLiteral("candidate");
        out.gate = QStringLiteral("pass");
    } else if (out.gate_edge >= 0.02 || !external_model) {
        out.recommendation = QStringLiteral("research");
        out.gate = QStringLiteral("reject");
    } else {
        out.recommendation = QStringLiteral("avoid");
        out.gate = QStringLiteral("reject");
    }
    out.rejection_reasons = reject.join(QStringLiteral("; "));
    out.risk_notes = reject.isEmpty() ? edge.risk_notes : out.rejection_reasons;
    out.rationale = QStringLiteral("%1/%2: market %3, model %4 via %5, cost %6, gate %7. Drivers: %8")
                        .arg(out.family, out.time_horizon)
                        .arg(pct(out.market_probability), pct(out.model_probability),
                             out.probability_source)
                        .arg(pct(out.spread_cost + out.fee_cost), pct(out.gate_edge))
                        .arg(out.research_drivers.join(QStringLiteral(", ")));
    return out;
}

QVector<KalshiUniversalSignal> KalshiUniversalEdgeModel::rank_markets(
    const QVector<pr::PredictionMarket>& markets,
    const KalshiUniversalOptions& options) {
    QVector<KalshiUniversalSignal> rows;
    rows.reserve(markets.size());
    for (const auto& market : markets) {
        auto s = score_market(market, options);
        if (s.is_valid)
            rows.push_back(s);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (a.passes_gate != b.passes_gate)
            return a.passes_gate;
        if (a.recommendation != b.recommendation)
            return a.recommendation < b.recommendation;
        if (a.liquidity_score != b.liquidity_score)
            return a.liquidity_score > b.liquidity_score;
        return std::abs(a.gate_edge) > std::abs(b.gate_edge);
    });
    return rows;
}

QJsonObject KalshiUniversalEdgeModel::to_json(const KalshiUniversalSignal& s) {
    QJsonArray drivers;
    for (const auto& d : s.research_drivers)
        drivers.append(d);
    QJsonArray sources;
    for (const auto& d : s.data_sources)
        sources.append(d);
    QJsonArray tags;
    for (const auto& d : s.tags)
        tags.append(d);
    return QJsonObject{{"market_id", s.market_id},
                       {"event_id", s.event_id},
                       {"question", s.question},
                       {"family", s.family},
                       {"time_horizon", s.time_horizon},
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
                       {"side", s.side},
                       {"recommendation", s.recommendation},
                       {"gate", s.gate},
                       {"rationale", s.rationale},
                       {"risk_notes", s.risk_notes},
                       {"rejection_reasons", s.rejection_reasons},
                       {"research_drivers", drivers},
                       {"data_sources", sources},
                       {"tags", tags},
                       {"is_valid", s.is_valid},
                       {"passes_gate", s.passes_gate},
                       {"is_strong", s.is_strong}};
}

} // namespace openmarketterminal::services::edge_radar
