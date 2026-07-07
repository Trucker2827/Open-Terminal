#include "services/sandbox/PaperExecutor.h"

#include "services/crypto_latency/CryptoLatencyService.h"
#include "services/sandbox/PaperFillModel.h"
#include "services/sandbox/SandboxRegistry.h"
#include "services/sandbox/SandboxResolver.h"
#include "services/sandbox/TickTail.h"
#include "storage/sqlite/Database.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QList>
#include <QUuid>
#include <QVariant>

#include <algorithm>

namespace openmarketterminal::services::sandbox {

namespace {

QJsonObject parse_object(const QString& json) {
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &pe);
    return pe.error == QJsonParseError::NoError && doc.isObject() ? doc.object() : QJsonObject{};
}

// Copy of automation::horizon_seconds (src/cli/automation/AutomationState.cpp)
// -- parses "15s"/"60s"/"1h"/"4h"/"1d" into seconds. Duplicated for the same
// layering reason TickTail.cpp duplicates automation::read_tail: that file
// is compiled into the CLI targets, not into openterminal_core.
int horizon_seconds(const QString& horizon) {
    const QString h = horizon.trimmed().toLower();
    if (h.isEmpty())
        return 0;
    bool ok = false;
    const double n = h.left(h.size() - 1).toDouble(&ok);
    if (!ok || n <= 0)
        return 0;
    switch (h.back().toLatin1()) {
        case 's': return static_cast<int>(n);
        case 'm': return static_cast<int>(n * 60);
        case 'h': return static_cast<int>(n * 3600);
        case 'd': return static_cast<int>(n * 86400);
        default: return 0;
    }
}

// Normalizes through CryptoLatencyService::normalize_symbol -- the same
// function automation_latest_spot_candidate applies to its symbol_filter
// before matching against edge_decision_journal.symbol -- so a strategy
// registered with a non-canonical symbol spelling (e.g. "BTCUSD") still
// matches journal rows stored in canonical form ("BTC-USD"). A plain
// toUpper() would silently never match in that case.
QStringList split_symbols(const QString& csv) {
    QStringList out;
    for (const QString& s : csv.split(QLatin1Char(','), Qt::SkipEmptyParts))
        out << crypto_latency::CryptoLatencyService::normalize_symbol(s);
    return out;
}

bool is_recognized_side(const QString& side) {
    return side == QLatin1String("buy") || side == QLatin1String("sell") ||
           side == QLatin1String("long") || side == QLatin1String("short");
}

// FeeModel from params keys maker_bps/taker_bps, defaulting to 40/60 bps --
// coinbase_advanced's default (non-VIP) maker/taker tier. Recalibrating fees
// is a params change (SandboxRegistry content-addresses a new strategy_id
// for any changed param), never a code change.
FeeModel fee_model_from_params(const QJsonObject& params) {
    FeeModel fm;
    fm.maker_bps = params.contains(QStringLiteral("maker_bps"))
                       ? params.value(QStringLiteral("maker_bps")).toDouble()
                       : 40.0;
    fm.taker_bps = params.contains(QStringLiteral("taker_bps"))
                       ? params.value(QStringLiteral("taker_bps")).toDouble()
                       : 60.0;
    return fm;
}

// Field presence for the data_quality rule: a freshness field counts as
// PRESENT only when the key exists AND its value is a number or a
// numeric string (some journal producers write freshest_age_ms both ways).
// A key holding garbage (non-numeric string, null, object) is treated as
// absent -- there is no usable signal in it.
bool freshness_field(const QJsonObject& o, const QString& key, double* out) {
    if (!o.contains(key))
        return false;
    const QJsonValue v = o.value(key);
    if (v.isDouble()) {
        *out = v.toDouble();
        return true;
    }
    if (v.isString()) {
        bool ok = false;
        const double parsed = v.toString().toDouble(&ok);
        if (ok)
            *out = parsed;
        return ok;
    }
    return false;
}

// data_quality, three-way (controller decision, task-5 review fix 4):
//   - 'unknown'  when the row provides NEITHER freshest_age_ms NOR
//     live_sources -- absence of freshness telemetry is not evidence of
//     degradation (prediction journal rows, e.g. btc5m/kalshi, carry
//     neither and would otherwise be permanently 'degraded').
//   - 'degraded' iff (freshest_age_ms present AND > 5000) OR (live_sources
//     present AND < 2) -- each field is only evaluated when present, so a
//     row with live_sources:1 and no age field is correctly degraded (the
//     old unconditional-|| logic defaulted a missing age to 0 and a missing
//     live_sources to 0 as well, which happened to work for missing
//     live_sources but silently passed a missing-age/bad-sources mix).
//   - 'ok' otherwise.
// Works over a freshness_json object (spot/btc5m/kalshi/long_short journal
// rows) or a scalp decision row's top level -- both use the same key names.
QString data_quality_from_freshness(const QJsonObject& freshness) {
    double freshest_age_ms = 0;
    double live_sources = 0;
    const bool has_age = freshness_field(freshness, QStringLiteral("freshest_age_ms"), &freshest_age_ms);
    const bool has_sources = freshness_field(freshness, QStringLiteral("live_sources"), &live_sources);
    if (!has_age && !has_sources)
        return QStringLiteral("unknown");
    if ((has_age && freshest_age_ms > 5000.0) || (has_sources && live_sources < 2.0))
        return QStringLiteral("degraded");
    return QStringLiteral("ok");
}

QString new_id() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

struct OpenPlan {
    QString decision_id;
    QString symbol;
    QString side;
    bool hypothetical = false;
    double qty = 0;
    double limit_price = 0; // entry / reference price
    QVariant target_price;  // invalid QVariant == SQL NULL
    QVariant stop_price;
    qint64 expires_at = 0;
    qint64 created_at = 0;
    QString state; // 'pending_fill' | 'open'
    QVariant opened_at; // invalid QVariant == SQL NULL (pending_fill)
    double entry_fee = 0;
    QString data_quality;
    double notional_usd = 0;
};

// Inserts one new sandbox_position row plus its 'open' sandbox_fill row (the
// creation transition -- distinct from the 'fill' transition a pending_fill
// position gets later in step 2). Deliberately NOT soft-caught: a
// decision_id that reaches here despite the caller's anti-join is a
// structural bug (or a broken anti-join), and the resulting UNIQUE-
// constraint violation is meant to surface loudly as a whole-cycle error --
// see PaperExecutor.h's transactionality note.
Result<void> insert_position(const QString& strategy_id, const OpenPlan& plan) {
    auto& db = Database::instance();
    const QString position_id = new_id();
    auto ins = db.execute(
        "INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side, hypothetical,"
        " qty, limit_price, target_price, stop_price, expires_at, state, opened_at, closed_at, entry_fee,"
        " exit_fee, realized_pnl, close_reason, data_quality, notional_usd, created_at) VALUES"
        " (?,?,?,?,?,?,?,?,?,?,?,?,?,NULL,?,0,NULL,NULL,?,?,?)",
        {position_id, strategy_id, plan.decision_id, plan.symbol, plan.side, plan.hypothetical ? 1 : 0,
         plan.qty, plan.limit_price, plan.target_price, plan.stop_price, plan.expires_at, plan.state,
         plan.opened_at, plan.entry_fee, plan.data_quality, plan.notional_usd, plan.created_at});
    if (ins.is_err())
        return Result<void>::err(ins.error());

    // fee mirrors plan.entry_fee for bookkeeping symmetry: prediction/
    // hypothetical positions pay their taker entry fee at this immediate
    // synthetic open, so it belongs on this row; pending_fill positions
    // carry entry_fee == 0 here and get their maker fee on the later 'fill'
    // row instead.
    auto fill = db.execute(
        "INSERT INTO sandbox_fill (fill_id, position_id, ts, kind, price, fee, note) VALUES (?,?,?,?,?,?,?)",
        {new_id(), position_id, plan.created_at, QStringLiteral("open"), plan.limit_price, plan.entry_fee,
         QStringLiteral("")});
    if (fill.is_err())
        return Result<void>::err(fill.error());
    return Result<void>::ok();
}

// ---------------------------------------------------------------------
// Candidate selection & opening, one function per strategy shape.
// ---------------------------------------------------------------------

// scalp: reimplements automation::latest_candidate's jsonl-scan contract
// directly over <daemon_dir>/scalp_decisions.jsonl (+ ".1") -- that helper
// lives in cli/ (not linked into openterminal_core), so this duplicates its
// scan/verdict/action/freshness rules rather than including a cli/ header
// (see TickTail.h's layering note; read_tail_with_prev is reused from there
// per the same rationale).
Result<void> open_scalp_candidates(const StrategyRow& strategy, const QJsonObject& params,
                                    const QString& daemon_dir, qint64 now_ms, CycleReport& report) {
    const int max_age_sec = params.contains(QStringLiteral("max_age_sec"))
                                 ? params.value(QStringLiteral("max_age_sec")).toInt()
                                 : 15;
    const double entry_offset_bps = params.value(QStringLiteral("entry_offset_bps")).toDouble(0.0);
    const double notional_usd = params.value(QStringLiteral("notional_usd")).toDouble(0.0);
    int horizon_sec_param = params.value(QStringLiteral("horizon_sec")).toInt(0);
    if (horizon_sec_param <= 0)
        horizon_sec_param = 900;
    const double target_bps = params.value(QStringLiteral("target_bps")).toDouble(0.0);
    const double stop_bps = params.value(QStringLiteral("stop_bps")).toDouble(0.0);

    const QString decisions_path = daemon_dir + QStringLiteral("/scalp_decisions.jsonl");
    const QByteArray buffer = read_tail_with_prev(decisions_path, kTickTailBytes);
    const QList<QByteArray> lines = buffer.split('\n');

    for (const QString& symbol : split_symbols(strategy.symbols)) {
        QJsonObject found;
        for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
            const QByteArray line = it->trimmed();
            if (line.isEmpty())
                continue;
            QJsonParseError pe;
            const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
            if (pe.error != QJsonParseError::NoError || !doc.isObject())
                continue;
            const QJsonObject d = doc.object();
            if (d.value(QStringLiteral("symbol")).toString().trimmed().toUpper() != symbol)
                continue;
            if (d.value(QStringLiteral("verdict")).toString() != QLatin1String("PAPER TRADE CANDIDATE"))
                continue;
            if (d.value(QStringLiteral("action")).toString() != QLatin1String("PAPER_LIMIT_BUY_ONLY"))
                continue;
            bool ok = false;
            const qint64 ts_ms = d.value(QStringLiteral("ts_ms")).toString().toLongLong(&ok);
            if (!ok || ts_ms <= 0 || now_ms - ts_ms > static_cast<qint64>(max_age_sec) * 1000)
                continue;
            found = d;
            break;
        }
        if (found.isEmpty())
            continue;

        bool ok = false;
        const qint64 ts_ms = found.value(QStringLiteral("ts_ms")).toString().toLongLong(&ok);
        const QString decision_id = symbol + QLatin1Char('|') + QString::number(ts_ms);

        // Soft dedup pre-check: the same latest candidate is routinely
        // reselected across cycles until a newer one supersedes it (unlike
        // spot's journal anti-join, this is the common steady state, not a
        // rare race), so check-then-skip here rather than let it hit the
        // decision_id UNIQUE constraint on every subsequent cycle.
        auto existing = Database::instance().execute(
            "SELECT 1 FROM sandbox_position WHERE decision_id = ? AND strategy_id = ?",
            {decision_id, strategy.strategy_id});
        if (existing.is_err())
            return Result<void>::err(existing.error());
        if (existing.value().next()) {
            report.skipped++;
            continue;
        }

        const double ref = found.value(QStringLiteral("reference_price")).toDouble();
        if (ref <= 0.0) {
            report.skipped++;
            report.notes << QStringLiteral("scalp candidate %1 skipped: non-positive reference_price").arg(decision_id);
            continue;
        }

        OpenPlan plan;
        plan.decision_id = decision_id;
        plan.symbol = symbol;
        plan.side = QStringLiteral("buy");
        plan.hypothetical = false;
        plan.limit_price = ref * (1.0 - entry_offset_bps / 10000.0);
        if (plan.limit_price <= 0.0) {
            report.skipped++;
            report.notes << QStringLiteral("scalp candidate %1 skipped: non-positive limit_price").arg(decision_id);
            continue;
        }
        plan.qty = notional_usd / plan.limit_price;
        plan.target_price = target_bps > 0.0 ? QVariant(plan.limit_price * (1.0 + target_bps / 10000.0)) : QVariant();
        plan.stop_price = stop_bps > 0.0 ? QVariant(plan.limit_price * (1.0 - stop_bps / 10000.0)) : QVariant();
        plan.expires_at = ts_ms + static_cast<qint64>(horizon_sec_param) * 1000;
        plan.created_at = ts_ms;
        plan.state = QStringLiteral("pending_fill");
        plan.opened_at = QVariant();
        plan.entry_fee = 0.0;
        // Scalp decisions carry freshest_age_ms/live_sources at the row's
        // TOP level (no freshness_json wrapper); the shared three-way rule
        // applies directly, yielding 'unknown' when an older daemon build
        // wrote neither field.
        plan.data_quality = data_quality_from_freshness(found);
        plan.notional_usd = notional_usd;

        auto ins = insert_position(strategy.strategy_id, plan);
        if (ins.is_err())
            return ins;
        report.opened++;
    }
    return Result<void>::ok();
}

// Non-scalp, non-prediction, non-hypothetical (season-1: "spot"). Same query
// family as automation_latest_spot_candidate (src/cli/CommandDispatch.cpp):
// journal_source/side/call/gate SQL filters, horizon/confidence filters
// applied in C++ (parsing "15s"-style horizon strings isn't natural SQL,
// same precedent as automation_latest_spot_candidate + automation::
// spot_row_passes), plus a SQL-level anti-join against sandbox_position.
// decision_id, SCOPED to this strategy_id (migration v058 -- two active
// books sharing a journal source must each open their own position from the
// same decision, not starve each other). That anti-join clause is the ONE
// piece of dedup logic this file's test suite neuter-verifies: do not
// "harden" this with an extra pre-check the way scalp's routine-duplicate
// path does above, or removing it stops producing an observable failure.
Result<void> open_spot_like_candidates(const StrategyRow& strategy, const QJsonObject& params, qint64 now_ms,
                                        CycleReport& report) {
    const QString journal_source = params.value(QStringLiteral("journal_source")).toString();
    const int max_age_sec = params.contains(QStringLiteral("max_age_sec"))
                                 ? params.value(QStringLiteral("max_age_sec")).toInt()
                                 : 3600;
    const int min_horizon_sec = params.value(QStringLiteral("min_horizon_sec")).toInt(0);
    const double min_confidence = params.value(QStringLiteral("min_confidence")).toDouble(0.0);
    const double entry_offset_bps = params.value(QStringLiteral("entry_offset_bps")).toDouble(0.0);
    const double notional_usd = params.value(QStringLiteral("notional_usd")).toDouble(0.0);
    int horizon_sec_param = params.value(QStringLiteral("horizon_sec")).toInt(0);
    if (horizon_sec_param <= 0)
        horizon_sec_param = 86400;

    const qint64 cutoff = now_ms - static_cast<qint64>(std::max(1, max_age_sec)) * 1000;

    for (const QString& symbol : split_symbols(strategy.symbols)) {
        auto sel = Database::instance().execute(
            "SELECT id, created_at, horizon, confidence, features_json, freshness_json"
            " FROM edge_decision_journal"
            " WHERE source = ? AND symbol = ? AND created_at >= ? AND side = 'buy'"
            " AND call = 'BUY CANDIDATE' AND gate = 'pass'"
            " AND id NOT IN (SELECT decision_id FROM sandbox_position WHERE strategy_id = ?)"
            " ORDER BY created_at DESC LIMIT 20",
            {journal_source, symbol, cutoff, strategy.strategy_id});
        if (sel.is_err())
            return Result<void>::err(sel.error());

        auto& q = sel.value();
        while (q.next()) {
            const QString horizon = q.value(2).toString();
            const double confidence = q.value(3).toDouble();
            if (horizon_seconds(horizon) < min_horizon_sec || confidence < min_confidence)
                continue;

            const QString id = q.value(0).toString();
            const qint64 created_at = q.value(1).toLongLong();
            const QJsonObject features = parse_object(q.value(4).toString());
            const QJsonObject freshness = parse_object(q.value(5).toString());
            const double ref = features.value(QStringLiteral("reference_price")).toDouble();
            if (ref <= 0.0) {
                report.skipped++;
                report.notes << QStringLiteral("%1 candidate %2 skipped: non-positive reference_price")
                                    .arg(strategy.kind, id);
                continue;
            }

            OpenPlan plan;
            plan.decision_id = id;
            plan.symbol = symbol;
            plan.side = QStringLiteral("buy");
            plan.hypothetical = false;
            plan.limit_price = ref * (1.0 - entry_offset_bps / 10000.0);
            if (plan.limit_price <= 0.0) {
                report.skipped++;
                report.notes << QStringLiteral("%1 candidate %2 skipped: non-positive limit_price")
                                    .arg(strategy.kind, id);
                continue;
            }
            plan.qty = notional_usd / plan.limit_price;

            double target_frac = 0.0;
            double stop_frac = 0.0;
            if (params.contains(QStringLiteral("target_move_pct")))
                target_frac = params.value(QStringLiteral("target_move_pct")).toDouble() / 100.0;
            else if (params.contains(QStringLiteral("target_bps")))
                target_frac = params.value(QStringLiteral("target_bps")).toDouble() / 10000.0;
            if (params.contains(QStringLiteral("stop_move_pct")))
                stop_frac = params.value(QStringLiteral("stop_move_pct")).toDouble() / 100.0;
            else if (params.contains(QStringLiteral("stop_bps")))
                stop_frac = params.value(QStringLiteral("stop_bps")).toDouble() / 10000.0;
            plan.target_price = target_frac > 0.0 ? QVariant(plan.limit_price * (1.0 + target_frac)) : QVariant();
            plan.stop_price = stop_frac > 0.0 ? QVariant(plan.limit_price * (1.0 - stop_frac)) : QVariant();

            plan.expires_at = created_at + static_cast<qint64>(horizon_sec_param) * 1000;
            plan.created_at = created_at;
            plan.state = QStringLiteral("pending_fill");
            plan.opened_at = QVariant();
            plan.entry_fee = 0.0;
            plan.data_quality = data_quality_from_freshness(freshness);
            plan.notional_usd = notional_usd;

            auto ins = insert_position(strategy.strategy_id, plan);
            if (ins.is_err())
                return ins;
            report.opened++;
            break; // one candidate per symbol per cycle, like automation_latest_spot_candidate
        }
    }
    return Result<void>::ok();
}

// prediction (params prediction:true, e.g. btc5m/kalshi): journal rows for
// journal_source with gate='pass', opened directly at 'open' (no resting
// order -- the market_probability price IS the immediate synthetic fill),
// side "yes", entry fee at taker (there is no maker leg). Not filtered by
// strategy.symbols: these journal sources carry their own symbol/market
// naming (e.g. "BTC", "KALSHI") independent of the registry's placeholder
// symbols column.
Result<void> open_prediction_candidates(const StrategyRow& strategy, const QJsonObject& params, qint64 now_ms,
                                         CycleReport& report) {
    const QString journal_source = params.value(QStringLiteral("journal_source")).toString();
    const double notional_usd = params.value(QStringLiteral("notional_usd")).toDouble(0.0);
    int horizon_sec_param = params.value(QStringLiteral("horizon_sec")).toInt(0);
    // No explicit horizon for these books today (season-1 seed params carry
    // none) -- the Resolver (Task 8) owns closing regardless of this value,
    // so a generous 24h fallback just keeps expires_at NOT NULL-satisfied.
    if (horizon_sec_param <= 0)
        horizon_sec_param = 86400;
    const FeeModel fees = fee_model_from_params(params);
    // Staleness cutoff (task-5 review fix 3): without it, activating a book
    // against a journal with months of gate-pass history would backfill
    // "positions" for decisions the executor never could have traded live.
    const int max_age_sec = params.contains(QStringLiteral("max_age_sec"))
                                 ? params.value(QStringLiteral("max_age_sec")).toInt()
                                 : 3600;
    const qint64 cutoff = now_ms - static_cast<qint64>(std::max(1, max_age_sec)) * 1000;

    auto sel = Database::instance().execute(
        "SELECT id, created_at, symbol, market_probability, freshness_json"
        " FROM edge_decision_journal"
        " WHERE source = ? AND gate = 'pass' AND created_at >= ?"
        " AND id NOT IN (SELECT decision_id FROM sandbox_position WHERE strategy_id = ?)"
        " ORDER BY created_at DESC LIMIT 50",
        {journal_source, cutoff, strategy.strategy_id});
    if (sel.is_err())
        return Result<void>::err(sel.error());

    auto& q = sel.value();
    while (q.next()) {
        const QString id = q.value(0).toString();
        const qint64 created_at = q.value(1).toLongLong();
        const QString symbol = q.value(2).toString();
        const double probability = q.value(3).toDouble();
        const QJsonObject freshness = parse_object(q.value(4).toString());

        if (probability <= 0.0) {
            report.skipped++;
            report.notes << QStringLiteral("%1 candidate %2 skipped: non-positive market_probability")
                                .arg(strategy.kind, id);
            continue;
        }

        OpenPlan plan;
        plan.decision_id = id;
        plan.symbol = symbol;
        plan.side = QStringLiteral("yes");
        plan.hypothetical = false;
        plan.limit_price = probability;
        plan.qty = notional_usd / probability;
        plan.target_price = QVariant();
        plan.stop_price = QVariant();
        plan.expires_at = created_at + static_cast<qint64>(horizon_sec_param) * 1000;
        plan.created_at = created_at;
        plan.state = QStringLiteral("open");
        plan.opened_at = QVariant(now_ms);
        plan.entry_fee = fee_for(notional_usd, fees.taker_bps);
        plan.data_quality = data_quality_from_freshness(freshness);
        plan.notional_usd = notional_usd;

        auto ins = insert_position(strategy.strategy_id, plan);
        if (ins.is_err())
            return ins;
        report.opened++;
    }
    return Result<void>::ok();
}

// price_forecast (season-1: chronos2 / chronos2_equity): these journal rows
// describe expected price movement, not a binary market payout. Open them as
// concrete paper positions so the ordinary tick target/stop/expiry path can
// prove whether the forecast had net edge after costs.
Result<void> open_price_forecast_candidates(const StrategyRow& strategy, const QJsonObject& params, qint64 now_ms,
                                            CycleReport& report) {
    const QString journal_source = params.value(QStringLiteral("journal_source")).toString();
    const QString horizon_filter = params.value(QStringLiteral("horizon")).toString().trimmed().toLower();
    const double notional_usd = params.value(QStringLiteral("notional_usd")).toDouble(0.0);
    int horizon_sec_param = params.value(QStringLiteral("horizon_sec")).toInt(0);
    if (horizon_sec_param <= 0)
        horizon_sec_param = 86400;
    const double target_bps = params.value(QStringLiteral("target_bps")).toDouble(0.0);
    const double stop_bps = params.value(QStringLiteral("stop_bps")).toDouble(0.0);
    const FeeModel fees = fee_model_from_params(params);
    const int max_age_sec = params.contains(QStringLiteral("max_age_sec"))
                                 ? params.value(QStringLiteral("max_age_sec")).toInt()
                                 : 3600;
    const qint64 cutoff = now_ms - static_cast<qint64>(std::max(1, max_age_sec)) * 1000;

    for (const QString& symbol : split_symbols(strategy.symbols)) {
        auto sel = Database::instance().execute(
            "SELECT id, created_at, symbol, side, horizon, features_json, freshness_json"
            " FROM edge_decision_journal"
            " WHERE source = ? AND symbol = ? AND gate = 'pass' AND created_at >= ?"
            " AND side IN ('buy','sell','long','short')"
            " AND id NOT IN (SELECT decision_id FROM sandbox_position WHERE strategy_id = ?)"
            " ORDER BY created_at DESC LIMIT 20",
            {journal_source, symbol, cutoff, strategy.strategy_id});
        if (sel.is_err())
            return Result<void>::err(sel.error());

        auto& q = sel.value();
        while (q.next()) {
            const QString id = q.value(0).toString();
            const qint64 created_at = q.value(1).toLongLong();
            const QString row_symbol = q.value(2).toString();
            const QString side = q.value(3).toString();
            const QString row_horizon = q.value(4).toString().trimmed().toLower();
            const QJsonObject features = parse_object(q.value(5).toString());
            const QJsonObject freshness = parse_object(q.value(6).toString());
            const double ref = features.value(QStringLiteral("reference_price")).toDouble();

            if (!horizon_filter.isEmpty() && row_horizon != horizon_filter) {
                report.skipped++;
                report.notes << QStringLiteral("%1 candidate %2 skipped: horizon %3 does not match book %4")
                                    .arg(strategy.kind, id, row_horizon, horizon_filter);
                continue;
            }
            if (!is_recognized_side(side)) {
                report.skipped++;
                report.notes << QStringLiteral("%1 candidate %2 skipped: unrecognized side %3")
                                    .arg(strategy.kind, id, side);
                continue;
            }
            if (ref <= 0.0) {
                report.skipped++;
                report.notes << QStringLiteral("%1 candidate %2 skipped: non-positive reference_price")
                                    .arg(strategy.kind, id);
                continue;
            }

            const bool short_side = side == QLatin1String("short") || side == QLatin1String("sell");
            OpenPlan plan;
            plan.decision_id = id;
            plan.symbol = row_symbol;
            plan.side = side;
            plan.hypothetical = false;
            plan.limit_price = ref;
            plan.qty = notional_usd / ref;
            plan.target_price = target_bps > 0.0
                                    ? QVariant(ref * (short_side ? (1.0 - target_bps / 10000.0)
                                                                  : (1.0 + target_bps / 10000.0)))
                                    : QVariant();
            plan.stop_price = stop_bps > 0.0
                                  ? QVariant(ref * (short_side ? (1.0 + stop_bps / 10000.0)
                                                               : (1.0 - stop_bps / 10000.0)))
                                  : QVariant();
            plan.expires_at = created_at + static_cast<qint64>(horizon_sec_param) * 1000;
            plan.created_at = created_at;
            plan.state = QStringLiteral("open");
            plan.opened_at = QVariant(now_ms);
            plan.entry_fee = fee_for(notional_usd, fees.taker_bps);
            plan.data_quality = data_quality_from_freshness(freshness);
            plan.notional_usd = notional_usd;

            auto ins = insert_position(strategy.strategy_id, plan);
            if (ins.is_err())
                return ins;
            report.opened++;
            break; // one latest forecast per symbol per cycle.
        }
    }
    return Result<void>::ok();
}

// hypothetical (params hypothetical:true, e.g. long_short): journal rows for
// journal_source with gate='pass', opened directly at 'open' at features_
// json.reference_price, side taken from the row's own `side` column
// ("long"/"short"), target/stop from params target_bps/stop_bps around that
// entry. Entry fee at taker (no resting order, same reasoning as prediction).
Result<void> open_hypothetical_candidates(const StrategyRow& strategy, const QJsonObject& params, qint64 now_ms,
                                           CycleReport& report) {
    const QString journal_source = params.value(QStringLiteral("journal_source")).toString();
    const double notional_usd = params.value(QStringLiteral("notional_usd")).toDouble(0.0);
    int horizon_sec_param = params.value(QStringLiteral("horizon_sec")).toInt(0);
    if (horizon_sec_param <= 0)
        horizon_sec_param = 86400;
    const double target_bps = params.value(QStringLiteral("target_bps")).toDouble(0.0);
    const double stop_bps = params.value(QStringLiteral("stop_bps")).toDouble(0.0);
    const FeeModel fees = fee_model_from_params(params);
    // Staleness cutoff (task-5 review fix 3) -- same rationale as the
    // prediction lane above.
    const int max_age_sec = params.contains(QStringLiteral("max_age_sec"))
                                 ? params.value(QStringLiteral("max_age_sec")).toInt()
                                 : 3600;
    const qint64 cutoff = now_ms - static_cast<qint64>(std::max(1, max_age_sec)) * 1000;

    auto sel = Database::instance().execute(
        "SELECT id, created_at, symbol, side, features_json, freshness_json"
        " FROM edge_decision_journal"
        " WHERE source = ? AND gate = 'pass' AND created_at >= ?"
        " AND id NOT IN (SELECT decision_id FROM sandbox_position WHERE strategy_id = ?)"
        " ORDER BY created_at DESC LIMIT 50",
        {journal_source, cutoff, strategy.strategy_id});
    if (sel.is_err())
        return Result<void>::err(sel.error());

    auto& q = sel.value();
    while (q.next()) {
        const QString id = q.value(0).toString();
        const qint64 created_at = q.value(1).toLongLong();
        const QString symbol = q.value(2).toString();
        const QString side = q.value(3).toString();
        const QJsonObject features = parse_object(q.value(4).toString());
        const QJsonObject freshness = parse_object(q.value(5).toString());
        const double ref = features.value(QStringLiteral("reference_price")).toDouble();

        if (!is_recognized_side(side)) {
            qWarning("PaperExecutor: unrecognized side '%s' for %s candidate %s; skipping",
                      qUtf8Printable(side), qUtf8Printable(strategy.kind), qUtf8Printable(id));
            report.skipped++;
            continue;
        }
        if (ref <= 0.0) {
            report.skipped++;
            report.notes << QStringLiteral("%1 candidate %2 skipped: non-positive reference_price")
                                .arg(strategy.kind, id);
            continue;
        }

        const bool is_short = side == QLatin1String("short") || side == QLatin1String("sell");
        OpenPlan plan;
        plan.decision_id = id;
        plan.symbol = symbol;
        plan.side = side;
        plan.hypothetical = true;
        plan.limit_price = ref;
        plan.qty = notional_usd / ref;
        plan.target_price = target_bps > 0.0
                                 ? QVariant(ref * (is_short ? (1.0 - target_bps / 10000.0) : (1.0 + target_bps / 10000.0)))
                                 : QVariant();
        plan.stop_price = stop_bps > 0.0
                               ? QVariant(ref * (is_short ? (1.0 + stop_bps / 10000.0) : (1.0 - stop_bps / 10000.0)))
                               : QVariant();
        plan.expires_at = created_at + static_cast<qint64>(horizon_sec_param) * 1000;
        plan.created_at = created_at;
        plan.state = QStringLiteral("open");
        plan.opened_at = QVariant(now_ms);
        plan.entry_fee = fee_for(notional_usd, fees.taker_bps);
        plan.data_quality = data_quality_from_freshness(freshness);
        plan.notional_usd = notional_usd;

        auto ins = insert_position(strategy.strategy_id, plan);
        if (ins.is_err())
            return ins;
        report.opened++;
    }
    return Result<void>::ok();
}

// ---------------------------------------------------------------------
// Step 2: pending_fill -> open|unfilled (spot/scalp only -- prediction and
// hypothetical positions never enter pending_fill).
// ---------------------------------------------------------------------
Result<void> advance_pending_fills(const QString& ticks_path, qint64 now_ms, CycleReport& report) {
    auto& db = Database::instance();
    auto sel = db.execute(
        "SELECT p.position_id, p.symbol, p.side, p.limit_price, p.expires_at, p.created_at, p.notional_usd,"
        " s.params_json"
        " FROM sandbox_position p JOIN sandbox_strategy s ON s.strategy_id = p.strategy_id"
        " WHERE p.state = 'pending_fill'");
    if (sel.is_err())
        return Result<void>::err(sel.error());

    struct Row {
        QString position_id, symbol, side;
        double limit_price = 0, notional_usd = 0;
        qint64 expires_at = 0, created_at = 0;
        QJsonObject params;
    };
    QList<Row> rows;
    {
        auto& q = sel.value();
        while (q.next()) {
            Row row;
            row.position_id = q.value(0).toString();
            row.symbol = q.value(1).toString();
            row.side = q.value(2).toString();
            row.limit_price = q.value(3).toDouble();
            row.expires_at = q.value(4).toLongLong();
            row.created_at = q.value(5).toLongLong();
            row.notional_usd = q.value(6).toDouble();
            row.params = parse_object(q.value(7).toString());
            rows.append(row);
        }
    }

    for (const auto& row : rows) {
        if (!is_recognized_side(row.side)) {
            qWarning("PaperExecutor: unrecognized side '%s' for pending position %s; skipping",
                      qUtf8Printable(row.side), qUtf8Printable(row.position_id));
            report.skipped++;
            continue;
        }

        const auto ticks = ticks_since(ticks_path, row.symbol, row.created_at);
        const FillResult fill = try_fill(row.side, row.limit_price, ticks, row.expires_at);
        const FeeModel fees = fee_model_from_params(row.params);

        if (fill.filled) {
            const double entry_fee = fee_for(row.notional_usd, fees.maker_bps);
            auto upd = db.execute(
                "UPDATE sandbox_position SET state='open', opened_at=?, entry_fee=?"
                " WHERE position_id=? AND state='pending_fill'",
                {fill.ts_ms, entry_fee, row.position_id});
            if (upd.is_err())
                return Result<void>::err(upd.error());
            if (upd.value().numRowsAffected() == 0) {
                report.skipped++;
                continue;
            }
            auto f = db.execute(
                "INSERT INTO sandbox_fill (fill_id, position_id, ts, kind, price, fee, note) VALUES (?,?,?,?,?,?,?)",
                {new_id(), row.position_id, fill.ts_ms, QStringLiteral("fill"), fill.price, entry_fee,
                 QStringLiteral("")});
            if (f.is_err())
                return Result<void>::err(f.error());
            report.filled++;
        } else if (now_ms >= row.expires_at) {
            auto upd = db.execute(
                "UPDATE sandbox_position SET state='unfilled', closed_at=?, close_reason='unfilled'"
                " WHERE position_id=? AND state='pending_fill'",
                {now_ms, row.position_id});
            if (upd.is_err())
                return Result<void>::err(upd.error());
            if (upd.value().numRowsAffected() == 0) {
                report.skipped++;
                continue;
            }
            auto f = db.execute(
                "INSERT INTO sandbox_fill (fill_id, position_id, ts, kind, price, fee, note) VALUES (?,?,?,?,?,?,?)",
                {new_id(), row.position_id, now_ms, QStringLiteral("unfilled"), 0.0, 0.0, QStringLiteral("")});
            if (f.is_err())
                return Result<void>::err(f.error());
            report.unfilled++;
        }
        // else: entry window still open, nothing to do this cycle.
    }
    return Result<void>::ok();
}

// ---------------------------------------------------------------------
// Step 3: open -> closed via check_exit. Restricted to concrete positions
// (side buy/sell/long/short, hypothetical=0) -- prediction (side 'yes'/'no')
// and hypothetical (long_short) positions are opened by run_cycle but closed
// only by the Outcome Resolver (Task 8).
// ---------------------------------------------------------------------
Result<void> advance_open_positions(const QString& ticks_path, qint64 now_ms, CycleReport& report) {
    auto& db = Database::instance();
    auto sel = db.execute(
        "SELECT p.position_id, p.symbol, p.side, p.limit_price, p.target_price, p.stop_price, p.expires_at,"
        " p.opened_at, p.entry_fee, p.notional_usd, p.qty, s.params_json"
        " FROM sandbox_position p JOIN sandbox_strategy s ON s.strategy_id = p.strategy_id"
        " WHERE p.state = 'open' AND p.hypothetical = 0 AND p.side IN ('buy','sell','long','short')");
    if (sel.is_err())
        return Result<void>::err(sel.error());

    struct Row {
        QString position_id, symbol, side;
        double limit_price = 0, target_price = 0, stop_price = 0, entry_fee = 0, notional_usd = 0, qty = 0;
        qint64 expires_at = 0, opened_at = 0;
        bool has_target = false, has_stop = false;
        QJsonObject params;
    };
    QList<Row> rows;
    {
        auto& q = sel.value();
        while (q.next()) {
            Row row;
            row.position_id = q.value(0).toString();
            row.symbol = q.value(1).toString();
            row.side = q.value(2).toString();
            row.limit_price = q.value(3).toDouble();
            row.has_target = !q.value(4).isNull();
            row.target_price = q.value(4).toDouble();
            row.has_stop = !q.value(5).isNull();
            row.stop_price = q.value(5).toDouble();
            row.expires_at = q.value(6).toLongLong();
            row.opened_at = q.value(7).toLongLong();
            row.entry_fee = q.value(8).toDouble();
            row.notional_usd = q.value(9).toDouble();
            row.qty = q.value(10).toDouble();
            row.params = parse_object(q.value(11).toString());
            rows.append(row);
        }
    }

    for (const auto& row : rows) {
        if (!is_recognized_side(row.side)) {
            qWarning("PaperExecutor: unrecognized side '%s' for open position %s; skipping",
                      qUtf8Printable(row.side), qUtf8Printable(row.position_id));
            report.skipped++;
            continue;
        }
        // NULL target/stop (task-5 review fix 2): a concrete position with a
        // missing bound must still EXPIRE, or it lives forever as a zombie
        // (the old skip-guard here never re-evaluated it). Map each absent
        // bound to a per-side never-trigger sentinel and let check_exit's
        // expiry path do its normal job: prices are always > 0, so for a
        // long, target +1e308 / stop 0 can never be crossed (and mirrored
        // for a short: target 0 / stop +1e308) -- only expiry can fire.
        const bool short_side = row.side == QLatin1String("short") || row.side == QLatin1String("sell");
        constexpr double kNever = 1e308;
        const double target_price = row.has_target ? row.target_price : (short_side ? 0.0 : kNever);
        const double stop_price = row.has_stop ? row.stop_price : (short_side ? kNever : 0.0);

        const auto ticks = ticks_since(ticks_path, row.symbol, row.opened_at);
        const ExitResult exit = check_exit(row.side, target_price, stop_price, row.expires_at, ticks, now_ms);
        if (!exit.exited)
            continue;

        // Data-gap expiry (task-5 review fix 1): check_exit's documented
        // sentinel for "expired with NO pre-expiry tick" is reason 'expiry'
        // with price 0. Booking realized_pnl against price 0 would fabricate
        // a full -notional loss (long) or +notional gain (short) out of a
        // MISSING PRINT. Instead: close the position with realized_pnl NULL
        // (excluded from resolved stats -- see run_cycle's header contract),
        // no exit fee (nothing traded), close_reason 'expiry', and
        // data_quality forced to 'degraded'.
        if (exit.reason == QLatin1String("expiry") && exit.price <= 0.0) {
            auto upd = db.execute(
                "UPDATE sandbox_position SET state='closed', closed_at=?, exit_fee=0, realized_pnl=NULL,"
                " close_reason='expiry', data_quality='degraded'"
                " WHERE position_id=? AND state='open'",
                {exit.ts_ms, row.position_id});
            if (upd.is_err())
                return Result<void>::err(upd.error());
            if (upd.value().numRowsAffected() == 0) {
                report.skipped++;
                continue;
            }
            auto f = db.execute(
                "INSERT INTO sandbox_fill (fill_id, position_id, ts, kind, price, fee, note) VALUES (?,?,?,?,?,?,?)",
                {new_id(), row.position_id, exit.ts_ms, QStringLiteral("expiry"), 0.0, 0.0,
                 QStringLiteral("data gap: no pre-expiry tick")});
            if (f.is_err())
                return Result<void>::err(f.error());
            report.closed++;
            continue;
        }

        const FeeModel fees = fee_model_from_params(row.params);
        // Deliberate modeling approximation: the exit fee is charged on the
        // position's ENTRY notional (notional_usd), not the actual exit
        // notional (exit.price * qty). For the small per-position moves this
        // sandbox trades (bps-scale targets/stops) the difference is second-
        // order, and using the fixed entry notional keeps entry and exit
        // fees directly comparable across a book.
        const double exit_fee = fee_for(row.notional_usd, fees.taker_bps);
        const double pnl = realized_pnl(row.side, row.limit_price, exit.price, row.qty, row.entry_fee, exit_fee);

        auto upd = db.execute(
            "UPDATE sandbox_position SET state='closed', closed_at=?, exit_fee=?, realized_pnl=?, close_reason=?"
            " WHERE position_id=? AND state='open'",
            {exit.ts_ms, exit_fee, pnl, exit.reason, row.position_id});
        if (upd.is_err())
            return Result<void>::err(upd.error());
        if (upd.value().numRowsAffected() == 0) {
            report.skipped++;
            continue;
        }
        auto f = db.execute(
            "INSERT INTO sandbox_fill (fill_id, position_id, ts, kind, price, fee, note) VALUES (?,?,?,?,?,?,?)",
            {new_id(), row.position_id, exit.ts_ms, exit.reason, exit.price, exit_fee, QStringLiteral("")});
        if (f.is_err())
            return Result<void>::err(f.error());
        report.closed++;
    }
    return Result<void>::ok();
}

} // namespace

Result<CycleReport> run_cycle(const QString& profile, const QString& daemon_dir, qint64 now_ms) {
    // daemon_dir is resolved by the caller (Task 6 CLI); the DB is a
    // process-wide singleton. profile is threaded through to
    // resolve_pending below (Task 8) purely for symmetry with the rest of
    // this function's signature -- it does not scope any query either.
    CycleReport report;

    auto strategies = list_strategies(QStringLiteral("active"));
    if (strategies.is_err())
        return Result<CycleReport>::err(strategies.error());

    const QString ticks_path = daemon_dir + QStringLiteral("/scalp_ticks.jsonl");

    for (const auto& strategy : strategies.value()) {
        const QJsonObject params = parse_object(strategy.params_json);
        const bool is_prediction = params.value(QStringLiteral("prediction")).toBool(false);
        const bool is_hypothetical = params.value(QStringLiteral("hypothetical")).toBool(false);
        const bool is_price_forecast = params.value(QStringLiteral("price_forecast")).toBool(false);

        Result<void> r = Result<void>::ok();
        if (strategy.kind == QLatin1String("scalp"))
            r = open_scalp_candidates(strategy, params, daemon_dir, now_ms, report);
        else if (is_prediction)
            r = open_prediction_candidates(strategy, params, now_ms, report);
        else if (is_price_forecast)
            r = open_price_forecast_candidates(strategy, params, now_ms, report);
        else if (is_hypothetical)
            r = open_hypothetical_candidates(strategy, params, now_ms, report);
        else
            r = open_spot_like_candidates(strategy, params, now_ms, report);

        if (r.is_err())
            return Result<CycleReport>::err(r.error());
    }

    auto step2 = advance_pending_fills(ticks_path, now_ms, report);
    if (step2.is_err())
        return Result<CycleReport>::err(step2.error());

    auto step3 = advance_open_positions(ticks_path, now_ms, report);
    if (step3.is_err())
        return Result<CycleReport>::err(step3.error());

    // Step 4 (Task 8): settle prediction/hypothetical books steps 1-3 above
    // deliberately never advance to 'closed' -- see this file's header.
    auto resolve = resolve_pending(profile, ticks_path, now_ms);
    if (resolve.is_err())
        return Result<CycleReport>::err(resolve.error());
    report.resolved += resolve.value().resolved;
    report.resolve_pending_count += resolve.value().pending;

    return Result<CycleReport>::ok(report);
}

} // namespace openmarketterminal::services::sandbox
