#include "services/ai_decision/DecisionContext.h"

#include "services/sandbox/FreshnessGate.h"
#include "storage/sqlite/Database.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlQuery>

namespace openmarketterminal::ai_decision {

namespace {

// edge_decision_journal SELECT column ordinals (0-based), pinned to the
// exact order this query lists them — see the brief's "Global Constraints"
// ordinal table. Keep this list and the SELECT in DecisionContext.cpp's
// assess() in lockstep; a reordering here without updating the other is a
// silent field-swap bug that the compiler cannot catch (both sides are
// plain ints/strings). `enum class` (not a plain enum): a plain enum's
// unscoped enumerators (kId/kSymbol/kSide/kGate/kSource/...) would leak
// into this anonymous namespace, and in a unity build every anonymous
// namespace in a batch shares one TU-wide scope — a same-named enumerator
// in a batch-mate's own anon-namespace helper would collide. Scoping to
// EdgeCol::kFoo leaves only the single type name "EdgeCol" as a (much
// less likely) collision surface.
enum class EdgeCol {
    kId = 0,
    kCreatedAt = 1,
    kUpdatedAt = 2,
    kVenue = 3,
    kSymbol = 4,
    kHorizon = 5,
    kMarketId = 6,
    kQuestion = 7,
    kDirection = 8,
    kSide = 9,
    kCall = 10,
    kGate = 11,
    kMarketProbability = 12,
    kModelProbability = 13,
    kRawEdge = 14,
    kEdgeAfterCost = 15,
    kGateEdge = 16,
    kSpreadCost = 17,
    kFeeCost = 18,
    kLiquidityScore = 19,
    kConfidence = 20,
    kSecondsLeft = 21,
    kDataStatus = 22,
    kFreshnessJson = 23,
    kFeaturesJson = 24,
    kReasons = 25,
    kOutcome = 26,
    kResolvedAt = 27,
    kSource = 28,
};

// QSqlQuery::value() takes a plain int column index; this converts the
// scoped enum back so call sites read `col(EdgeCol::kVenue)`.
constexpr int col(EdgeCol c) {
    return static_cast<int>(c);
}

} // namespace

DecisionPacket assess(const QString& symbol, const QString& market) {
    DecisionPacket packet;
    packet.symbol = symbol;
    packet.market = market;

    auto rows = openmarketterminal::Database::instance().execute(
        "SELECT id, created_at, updated_at, venue, symbol, horizon, market_id, question, direction,"
        " side, call, gate, market_probability, model_probability, raw_edge, edge_after_cost, gate_edge,"
        " spread_cost, fee_cost, liquidity_score, confidence, seconds_left, data_status, freshness_json,"
        " features_json, reasons, outcome, resolved_at, source"
        " FROM edge_decision_journal WHERE symbol = ? ORDER BY created_at DESC LIMIT 1",
        {symbol});

    if (rows.is_err()) {
        packet.notes << QStringLiteral("edge_decision_journal query failed: ") + QString::fromStdString(rows.error());
        packet.clears_cost = QStringLiteral("unknown");
        packet.freshness = QStringLiteral("unknown");
        packet.lane_verdict = QStringLiteral("unknown");
        packet.recommendation_hint = compute_hint(packet);
        return packet;
    }

    if (!rows.value().next()) {
        packet.has_edge_signal = false;
        packet.notes << QStringLiteral("no edge_decision_journal row");
        packet.clears_cost = QStringLiteral("unknown");
        packet.freshness = QStringLiteral("unknown");
        packet.lane_verdict = QStringLiteral("unknown");
        packet.recommendation_hint = compute_hint(packet);
        return packet;
    }

    QSqlQuery& q = rows.value();
    packet.has_edge_signal = true;
    packet.venue = q.value(col(EdgeCol::kVenue)).toString();
    packet.symbol = q.value(col(EdgeCol::kSymbol)).toString();
    packet.horizon = q.value(col(EdgeCol::kHorizon)).toString();
    packet.market_id = q.value(col(EdgeCol::kMarketId)).toString();
    packet.question = q.value(col(EdgeCol::kQuestion)).toString();
    packet.direction = q.value(col(EdgeCol::kDirection)).toString();
    packet.side = q.value(col(EdgeCol::kSide)).toString();
    packet.call = q.value(col(EdgeCol::kCall)).toString();
    packet.gate = q.value(col(EdgeCol::kGate)).toString();
    packet.market_probability = q.value(col(EdgeCol::kMarketProbability)).toDouble();
    packet.model_probability = q.value(col(EdgeCol::kModelProbability)).toDouble();
    packet.raw_edge = q.value(col(EdgeCol::kRawEdge)).toDouble();
    packet.edge_after_cost = q.value(col(EdgeCol::kEdgeAfterCost)).toDouble();
    packet.gate_edge = q.value(col(EdgeCol::kGateEdge)).toDouble();
    packet.spread_cost = q.value(col(EdgeCol::kSpreadCost)).toDouble();
    packet.fee_cost = q.value(col(EdgeCol::kFeeCost)).toDouble();
    packet.liquidity_score = q.value(col(EdgeCol::kLiquidityScore)).toDouble();
    packet.confidence = q.value(col(EdgeCol::kConfidence)).toDouble();
    packet.seconds_left = q.value(col(EdgeCol::kSecondsLeft)).toLongLong();
    packet.data_status = q.value(col(EdgeCol::kDataStatus)).toString();
    packet.reasons = q.value(col(EdgeCol::kReasons)).toString();
    packet.source = q.value(col(EdgeCol::kSource)).toString();
    const QString freshness_json = q.value(col(EdgeCol::kFreshnessJson)).toString();

    // Cost: from the journal row's OWN recorded accounting, not a fresh call
    // into the spot maker cost model (see file header).
    packet.round_trip_cost_bps = packet.spread_cost + packet.fee_cost;
    packet.clears_cost = (packet.edge_after_cost > 0.0 && packet.gate != QStringLiteral("fail"))
                              ? QStringLiteral("true")
                              : QStringLiteral("false");

    // Freshness: Task 1's data_quality_from_freshness over the row's own
    // freshness_json object.
    packet.freshness = openmarketterminal::services::sandbox::data_quality_from_freshness(
        QJsonDocument::fromJson(freshness_json.toUtf8()).object());

    // Lane significance: best-effort, deliberately left "unknown" here.
    // evaluate_lane_significance (SandboxScorer.h) returns a QVector of
    // per-LANE verdicts (cohort = kind/venue/liquidity), clustered by
    // session, over the WHOLE resolved-position history that matches a lane
    // — it has no notion of "this one symbol's verdict". A symbol can span
    // several lanes (e.g. BTC-USD trades under scalp/coinbase_advanced/taker
    // AND scalp/kraken_pro/maker simultaneously), so collapsing the returned
    // QVector<LaneSignificance> down to this packet's single lane_verdict
    // field is not well-defined without inventing an aggregation rule the
    // brief does not specify (mean? best lane? first match?). Per the brief
    // ("if wiring it cleanly is non-trivial, return 'unknown' ... lane
    // verdict is best-effort"), this piece punts rather than guess: no
    // sandbox_position/sandbox_strategy query is issued for this field.
    packet.lane_verdict = QStringLiteral("unknown");
    packet.notes << QStringLiteral(
        "lane_verdict is a best-effort stub: evaluate_lane_significance is per-lane "
        "(kind/venue/liquidity), not per-symbol, so no single-verdict collapse is defined yet");

    // Position/headroom: documented best-effort stub. assess() is
    // synchronous and must not reach into a broker/event-loop connection
    // (see file header), so this is a placeholder, not a live read.
    packet.position_source = QStringLiteral("none");
    packet.position_qty = 0.0;
    packet.buying_power = -1.0;
    packet.notes << QStringLiteral("position enrichment pending AI ledger (piece 4)");

    packet.recommendation_hint = compute_hint(packet);
    return packet;
}

QString compute_hint(const DecisionPacket& packet) {
    if (!packet.has_edge_signal)
        return QStringLiteral("no edge signal");
    if (packet.freshness != QStringLiteral("ok"))
        return QStringLiteral("blocked: stale data");
    if (packet.clears_cost == QStringLiteral("false"))
        return QStringLiteral("blocked: below cost");
    return QStringLiteral("all gates pass");
}

QJsonObject to_json(const DecisionPacket& packet) {
    QJsonObject obj;
    obj[QStringLiteral("symbol")] = packet.symbol;
    obj[QStringLiteral("market")] = packet.market;
    obj[QStringLiteral("has_edge_signal")] = packet.has_edge_signal;
    obj[QStringLiteral("venue")] = packet.venue;
    obj[QStringLiteral("horizon")] = packet.horizon;
    obj[QStringLiteral("market_id")] = packet.market_id;
    obj[QStringLiteral("question")] = packet.question;
    obj[QStringLiteral("direction")] = packet.direction;
    obj[QStringLiteral("side")] = packet.side;
    obj[QStringLiteral("call")] = packet.call;
    obj[QStringLiteral("gate")] = packet.gate;
    obj[QStringLiteral("market_probability")] = packet.market_probability;
    obj[QStringLiteral("model_probability")] = packet.model_probability;
    obj[QStringLiteral("raw_edge")] = packet.raw_edge;
    obj[QStringLiteral("edge_after_cost")] = packet.edge_after_cost;
    obj[QStringLiteral("gate_edge")] = packet.gate_edge;
    obj[QStringLiteral("spread_cost")] = packet.spread_cost;
    obj[QStringLiteral("fee_cost")] = packet.fee_cost;
    obj[QStringLiteral("liquidity_score")] = packet.liquidity_score;
    obj[QStringLiteral("confidence")] = packet.confidence;
    obj[QStringLiteral("seconds_left")] = static_cast<double>(packet.seconds_left);
    obj[QStringLiteral("data_status")] = packet.data_status;
    obj[QStringLiteral("reasons")] = packet.reasons;
    obj[QStringLiteral("source")] = packet.source;
    obj[QStringLiteral("round_trip_cost_bps")] = packet.round_trip_cost_bps;
    obj[QStringLiteral("clears_cost")] = packet.clears_cost;
    obj[QStringLiteral("freshness")] = packet.freshness;
    obj[QStringLiteral("lane_verdict")] = packet.lane_verdict;
    obj[QStringLiteral("position_source")] = packet.position_source;
    obj[QStringLiteral("position_qty")] = packet.position_qty;
    obj[QStringLiteral("buying_power")] = packet.buying_power;
    obj[QStringLiteral("recommendation_hint")] = packet.recommendation_hint;
    QJsonArray notes_arr;
    for (const auto& n : packet.notes)
        notes_arr.append(n);
    obj[QStringLiteral("notes")] = notes_arr;
    return obj;
}

} // namespace openmarketterminal::ai_decision
