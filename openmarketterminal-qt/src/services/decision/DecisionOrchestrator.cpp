#include "services/decision/DecisionOrchestrator.h"

#include "storage/sqlite/Database.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace openmarketterminal::services::decision {
namespace {

double clamp_probability(double value) {
    return std::clamp(value, 0.0, 1.0);
}

QJsonObject signal_json(const DecisionSignal& signal) {
    return QJsonObject{{"name", signal.name}, {"source", signal.source},
                       {"model_version", signal.model_version},
                       {"as_of_ms", QString::number(signal.as_of_ms)},
                       {"probability", signal.probability}, {"confidence", signal.confidence},
                       {"calibration_score", signal.calibration_score},
                       {"sample_count", signal.sample_count},
                       {"max_age_ms", QString::number(signal.max_age_ms)},
                       {"advisory_only", signal.advisory_only}, {"features", signal.features}};
}

QJsonObject quote_json(const ExecutionQuote& quote) {
    return QJsonObject{{"venue", quote.venue}, {"market_id", quote.market_id},
                       {"side", quote.side}, {"executable", quote.executable},
                       {"observed_at_ms", QString::number(quote.observed_at_ms)},
                       {"bid", quote.bid}, {"ask", quote.ask},
                       {"implied_probability", quote.implied_probability},
                       {"normalized_spread", quote.normalized_spread},
                       {"available_size", quote.available_size},
                       {"spread_cost", quote.spread_cost},
                       {"fee_cost", quote.fee_cost}, {"slippage_cost", quote.slippage_cost},
                       {"exit_cost_reserve", quote.exit_cost_reserve}};
}

QJsonObject portfolio_json(const PortfolioState& portfolio) {
    return QJsonObject{{"total_exposure", portfolio.total_exposure},
                       {"symbol_exposure", portfolio.symbol_exposure},
                       {"venue_exposure", portfolio.venue_exposure},
                       {"daily_realized_pnl", portfolio.daily_realized_pnl},
                       {"open_positions", portfolio.open_positions}};
}

QJsonObject limits_json(const DecisionRiskLimits& limits) {
    return QJsonObject{{"requested_notional", limits.requested_notional},
                       {"max_total_exposure", limits.max_total_exposure},
                       {"max_symbol_exposure", limits.max_symbol_exposure},
                       {"max_venue_exposure", limits.max_venue_exposure},
                       {"max_daily_loss", limits.max_daily_loss},
                       {"max_open_positions", limits.max_open_positions},
                       {"minimum_edge", limits.minimum_edge},
                       {"minimum_confidence", limits.minimum_confidence},
                       {"minimum_liquidity", limits.minimum_liquidity},
                       {"maximum_spread", limits.maximum_spread},
                       {"maximum_quote_age_ms", QString::number(limits.maximum_quote_age_ms)},
                       {"minimum_seconds_left", limits.minimum_seconds_left}};
}

QJsonArray strings_json(const QStringList& strings) {
    QJsonArray out;
    for (const auto& value : strings) out.append(value);
    return out;
}

QJsonArray signal_list_json(const QVector<DecisionSignal>& model_signals) {
    QJsonArray out;
    for (const auto& signal : model_signals) out.append(signal_json(signal));
    return out;
}

double signal_weight(const DecisionSignal& signal) {
    const double confidence = std::clamp(signal.confidence, 0.0, 1.0);
    const double calibration = signal.calibration_score > 0.0
        ? std::clamp(signal.calibration_score, 0.05, 1.0) : 0.25;
    const double sample_factor = signal.sample_count > 0
        ? std::min(1.0, std::sqrt(static_cast<double>(signal.sample_count) / 30.0)) : 0.25;
    return confidence * calibration * sample_factor;
}

void add_limit_blockers(DecisionEnvelope& envelope) {
    const auto& q = envelope.quote;
    const auto& p = envelope.portfolio;
    const auto& l = envelope.limits;
    const double spread = q.normalized_spread >= 0.0
        ? q.normalized_spread : (q.ask > 0.0 && q.bid >= 0.0 ? q.ask - q.bid : 1.0);
    if (!q.executable)
        envelope.risk_blockers.append(QStringLiteral("quote is not executable"));
    if (q.ask <= 0.0)
        envelope.risk_blockers.append(QStringLiteral("no executable selected-side ask"));
    if (spread > l.maximum_spread)
        envelope.risk_blockers.append(QStringLiteral("spread exceeds limit"));
    if (q.available_size < l.minimum_liquidity)
        envelope.risk_blockers.append(QStringLiteral("insufficient executable liquidity"));
    if (l.maximum_quote_age_ms > 0 &&
        (q.observed_at_ms <= 0 || envelope.decision_ts_ms - q.observed_at_ms > l.maximum_quote_age_ms))
        envelope.risk_blockers.append(QStringLiteral("quote is stale"));
    if (q.observed_at_ms > envelope.decision_ts_ms)
        envelope.risk_blockers.append(QStringLiteral("quote is from the future"));
    if (l.max_open_positions > 0 && p.open_positions >= l.max_open_positions)
        envelope.risk_blockers.append(QStringLiteral("open-position limit reached"));
    if (l.max_daily_loss > 0.0 && p.daily_realized_pnl <= -l.max_daily_loss)
        envelope.risk_blockers.append(QStringLiteral("daily loss limit reached"));
    if (l.max_total_exposure > 0.0 && p.total_exposure + l.requested_notional > l.max_total_exposure)
        envelope.risk_blockers.append(QStringLiteral("portfolio exposure limit exceeded"));
    if (l.max_symbol_exposure > 0.0 && p.symbol_exposure + l.requested_notional > l.max_symbol_exposure)
        envelope.risk_blockers.append(QStringLiteral("symbol exposure limit exceeded"));
    if (l.max_venue_exposure > 0.0 && p.venue_exposure + l.requested_notional > l.max_venue_exposure)
        envelope.risk_blockers.append(QStringLiteral("venue exposure limit exceeded"));
    if (envelope.seconds_left >= 0 && envelope.seconds_left < l.minimum_seconds_left)
        envelope.risk_blockers.append(QStringLiteral("too late to enter"));
    if (envelope.edge_after_cost < l.minimum_edge)
        envelope.risk_blockers.append(QStringLiteral("edge does not clear cost and safety floor"));
    if (envelope.confidence < l.minimum_confidence)
        envelope.risk_blockers.append(QStringLiteral("confidence below minimum"));
}

} // namespace

QJsonObject DecisionEnvelope::to_json(bool include_hash) const {
    QJsonObject out{{"schema_version", schema_version}, {"decision_id", decision_id},
                    {"decision_ts_ms", QString::number(decision_ts_ms)}, {"symbol", symbol},
                    {"horizon", horizon}, {"source", source},
                    {"data_snapshot_id", data_snapshot_id}, {"venue", venue},
                    {"market_id", market_id}, {"side", side},
                    {"seconds_left", seconds_left}, {"model_probability", model_probability},
                    {"market_probability", market_probability}, {"confidence", confidence},
                    {"raw_edge", raw_edge}, {"total_cost", total_cost},
                    {"edge_after_cost", edge_after_cost}, {"verdict", verdict},
                    {"reasons", strings_json(reasons)},
                    {"risk_blockers", strings_json(risk_blockers)},
                    {"accepted_signals", signal_list_json(accepted_signals)},
                    {"rejected_signals", signal_list_json(rejected_signals)},
                    {"quote", quote_json(quote)}, {"portfolio", portfolio_json(portfolio)},
                    {"limits", limits_json(limits)}, {"context", context}};
    if (include_hash) out.insert(QStringLiteral("content_hash"), content_hash);
    return out;
}

DecisionEnvelope DecisionOrchestrator::evaluate(const DecisionRequest& request) {
    DecisionEnvelope out;
    out.decision_id = request.decision_id.trimmed().isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces) : request.decision_id.trimmed();
    out.decision_ts_ms = request.decision_ts_ms;
    out.symbol = request.symbol.trimmed().toUpper();
    out.horizon = request.horizon.trimmed().toLower();
    out.source = request.source.trimmed();
    out.data_snapshot_id = request.data_snapshot_id.trimmed();
    out.venue = request.quote.venue.trimmed().toLower();
    out.market_id = request.quote.market_id.trimmed();
    out.side = request.quote.side.trimmed().toLower();
    out.seconds_left = request.seconds_left;
    out.quote = request.quote;
    out.portfolio = request.portfolio;
    out.limits = request.limits;
    out.context = request.context;

    double weighted_probability = 0.0;
    double total_weight = 0.0;
    double weighted_confidence = 0.0;
    for (const auto& signal : request.model_signals) {
        const bool future = signal.as_of_ms <= 0 || signal.as_of_ms > request.decision_ts_ms;
        const bool stale = signal.max_age_ms > 0 &&
            request.decision_ts_ms - signal.as_of_ms > signal.max_age_ms;
        const bool invalid_probability = signal.probability < 0.0 || signal.probability > 1.0;
        if (future || stale || invalid_probability) {
            out.rejected_signals.append(signal);
            continue;
        }
        out.accepted_signals.append(signal);
        if (signal.advisory_only) continue;
        const double weight = signal_weight(signal);
        if (weight <= 0.0) continue;
        weighted_probability += clamp_probability(signal.probability) * weight;
        weighted_confidence += std::clamp(signal.confidence, 0.0, 1.0) * weight;
        total_weight += weight;
    }

    out.market_probability = request.quote.implied_probability >= 0.0
        ? clamp_probability(request.quote.implied_probability)
        : (request.quote.ask <= 1.0 ? request.quote.ask : 0.5);
    out.total_cost = std::max(0.0, request.quote.spread_cost) +
                     std::max(0.0, request.quote.fee_cost) +
                     std::max(0.0, request.quote.slippage_cost) +
                     std::max(0.0, request.quote.exit_cost_reserve);
    if (total_weight > 0.0) {
        out.model_probability = weighted_probability / total_weight;
        out.confidence = weighted_confidence / total_weight;
        out.raw_edge = out.model_probability - out.market_probability;
        out.edge_after_cost = out.raw_edge - out.total_cost;
    } else {
        out.reasons.append(QStringLiteral("no fresh quantitative signal"));
    }

    add_limit_blockers(out);
    if (total_weight <= 0.0)
        out.verdict = QStringLiteral("NOT_ENOUGH_DATA");
    else if (out.risk_blockers.contains(QStringLiteral("too late to enter")))
        out.verdict = QStringLiteral("TOO_LATE");
    else if (out.risk_blockers.contains(QStringLiteral("quote is not executable")) ||
             out.risk_blockers.contains(QStringLiteral("no executable selected-side ask")) ||
             out.risk_blockers.contains(QStringLiteral("spread exceeds limit")) ||
             out.risk_blockers.contains(QStringLiteral("insufficient executable liquidity")) ||
             out.risk_blockers.contains(QStringLiteral("quote is stale")) ||
             out.risk_blockers.contains(QStringLiteral("quote is from the future")))
        out.verdict = QStringLiteral("BAD_PRICE");
    else if (!out.risk_blockers.isEmpty())
        out.verdict = QStringLiteral("NO_TRADE");
    else
        out.verdict = QStringLiteral("TRADE_CANDIDATE");

    if (out.verdict == QStringLiteral("TRADE_CANDIDATE"))
        out.reasons.append(QStringLiteral("fresh quantitative consensus clears executable costs and risk limits"));
    out.content_hash = content_hash(out);
    return out;
}

DecisionReplayResult DecisionOrchestrator::replay(const QVector<DecisionRequest>& requests) {
    QVector<DecisionRequest> ordered = requests;
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return left.decision_ts_ms < right.decision_ts_ms;
    });
    DecisionReplayResult result;
    result.frames = ordered.size();
    result.envelopes.reserve(ordered.size());
    for (const auto& request : ordered) {
        const auto envelope = evaluate(request);
        result.envelopes.append(envelope);
        if (envelope.verdict == QStringLiteral("TRADE_CANDIDATE")) ++result.trade_candidates;
        else if (envelope.verdict == QStringLiteral("BAD_PRICE")) ++result.bad_price;
        else if (envelope.verdict == QStringLiteral("TOO_LATE")) ++result.too_late;
        else if (envelope.verdict == QStringLiteral("NOT_ENOUGH_DATA")) ++result.not_enough_data;
        else ++result.no_trade;
    }
    return result;
}

QStringList DecisionOrchestrator::validate(const DecisionEnvelope& envelope) {
    QStringList issues;
    if (envelope.schema_version != 1) issues.append(QStringLiteral("unsupported schema version"));
    if (envelope.decision_id.trimmed().isEmpty()) issues.append(QStringLiteral("missing decision id"));
    if (envelope.decision_ts_ms <= 0) issues.append(QStringLiteral("missing decision timestamp"));
    if (envelope.symbol.trimmed().isEmpty()) issues.append(QStringLiteral("missing symbol"));
    if (envelope.horizon.trimmed().isEmpty()) issues.append(QStringLiteral("missing horizon"));
    if (envelope.data_snapshot_id.trimmed().isEmpty()) issues.append(QStringLiteral("missing data snapshot id"));
    if (envelope.venue.trimmed().isEmpty()) issues.append(QStringLiteral("missing venue"));
    if (envelope.market_id.trimmed().isEmpty()) issues.append(QStringLiteral("missing market id"));
    if (envelope.side.trimmed().isEmpty()) issues.append(QStringLiteral("missing side"));
    if (envelope.model_probability < 0.0 || envelope.model_probability > 1.0)
        issues.append(QStringLiteral("model probability outside [0,1]"));
    if (envelope.market_probability < 0.0 || envelope.market_probability > 1.0)
        issues.append(QStringLiteral("market probability outside [0,1]"));
    for (const auto& signal : envelope.accepted_signals)
        if (signal.as_of_ms > envelope.decision_ts_ms)
            issues.append(QStringLiteral("accepted signal is from the future"));
        else if (signal.source.trimmed().isEmpty() || signal.model_version.trimmed().isEmpty())
            issues.append(QStringLiteral("accepted signal lacks source or model version"));
    if (envelope.quote.observed_at_ms > envelope.decision_ts_ms)
        issues.append(QStringLiteral("quote is from the future"));
    if (envelope.content_hash.isEmpty()) issues.append(QStringLiteral("missing content hash"));
    else if (content_hash(envelope) != envelope.content_hash)
        issues.append(QStringLiteral("content hash mismatch"));
    return issues;
}

QString DecisionOrchestrator::content_hash(const DecisionEnvelope& envelope) {
    const QByteArray bytes = QJsonDocument(envelope.to_json(false)).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

Result<void> DecisionOrchestrator::persist_immutable(const DecisionEnvelope& envelope,
                                                     const QString& journal_id) {
    const QStringList issues = validate(envelope);
    if (!issues.isEmpty())
        return Result<void>::err(issues.join(QStringLiteral("; ")).toStdString());
    const QString json = QString::fromUtf8(
        QJsonDocument(envelope.to_json()).toJson(QJsonDocument::Compact));
    auto result = Database::instance().execute(
        "INSERT INTO decision_envelopes (id,schema_version,decision_ts,created_at,venue,symbol,horizon,"
        "market_id,side,verdict,source,journal_id,content_hash,envelope_json) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {envelope.decision_id, envelope.schema_version, envelope.decision_ts_ms,
         envelope.decision_ts_ms, envelope.venue, envelope.symbol, envelope.horizon,
         envelope.market_id, envelope.side, envelope.verdict, envelope.source,
         journal_id, envelope.content_hash, json});
    if (result.is_err()) return Result<void>::err(result.error());
    return Result<void>::ok();
}

} // namespace openmarketterminal::services::decision
