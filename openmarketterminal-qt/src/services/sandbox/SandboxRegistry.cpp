#include "services/sandbox/SandboxRegistry.h"

#include "services/crypto/CryptoFees.h"
#include "services/ai_strategy/StrategyRegistry.h"
#include "services/sandbox/PaperFillModel.h"
#include "services/sandbox/SandboxEligibility.h"
#include "services/sandbox/SandboxScorer.h"
#include "storage/sqlite/Database.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSet>
#include <QStandardPaths>
#include <QVariantList>

namespace openmarketterminal::services::sandbox {

namespace {

QString params_json_compact(const QJsonObject& params) {
    return QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact));
}

QString research_evidence_path(const QString& filename) {
    const QString override_dir = qEnvironmentVariable("OPENTERMINAL_KALSHI_EVIDENCE_DIR").trimmed();
    const QString dir = override_dir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
              QStringLiteral("/Open Terminal/Open Terminal")
        : override_dir;
    return QDir(dir).filePath(filename);
}

struct ShadowLedgerDefinition {
    QString family;
    QString symbols;
    QString filename;
    QString producer;
    QString raw_symbol;
};

void append_shadow_ledgers(QJsonArray& entries, qint64 now_ms,
                           int& stale, int& errors, int& waiting, int& active) {
    const QVector<ShadowLedgerDefinition> definitions{
        {QStringLiteral("BTC1H"), QStringLiteral("BTC-USD"),
         QStringLiteral("btc1h-chronos-shadow.json"), QStringLiteral("btc1h_chronos_shadow.py"),
         QStringLiteral("BTC")},
        {QStringLiteral("KXGOLDH"), QStringLiteral("XAU-USD"),
         QStringLiteral("chronos-kxgoldh-shadow.json"), QStringLiteral("commodity_chronos_shadow.py"),
         QStringLiteral("XAU")},
        {QStringLiteral("KXSILVERH"), QStringLiteral("XAG-USD"),
         QStringLiteral("chronos-kxsilverh-shadow.json"), QStringLiteral("commodity_chronos_shadow.py"),
         QStringLiteral("XAG")},
        {QStringLiteral("KXWTIH"), QStringLiteral("XTI-USD"),
         QStringLiteral("chronos-kxwtih-shadow.json"), QStringLiteral("commodity_chronos_shadow.py"),
         QStringLiteral("XTI")},
    };
    const QStringList cohorts{QStringLiteral("chronos_alone"), QStringLiteral("control_alone"),
                              QStringLiteral("agreement"), QStringLiteral("conflict")};
    auto& db = Database::instance();
    for (const auto& definition : definitions) {
        const QString path = research_evidence_path(definition.filename);
        QFile file(path);
        QJsonObject state;
        QString parse_error;
        if (file.exists()) {
            if (!file.open(QIODevice::ReadOnly)) {
                parse_error = QStringLiteral("ledger unreadable");
            } else {
                QJsonParseError error;
                const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
                if (!document.isObject()) parse_error = QStringLiteral("invalid ledger JSON: %1").arg(error.errorString());
                else state = document.object();
            }
        }
        const QJsonObject records = state.value(QStringLiteral("records")).toObject();
        const QJsonObject diagnostics = state.value(QStringLiteral("diagnostics")).toObject();
        const QJsonObject skip_reasons = diagnostics.value(QStringLiteral("skip_reasons")).toObject();
        const QJsonObject last_run = diagnostics.value(QStringLiteral("last_run")).toObject();
        int source_ticks = 1;
        if (definition.family != QLatin1String("BTC1H")) {
            auto ticks = db.execute("SELECT COUNT(*) FROM edge_prediction_raw_ticks WHERE symbol=? AND source LIKE 'pyth:%'",
                                    {definition.raw_symbol});
            source_ticks = ticks.is_ok() && ticks.value().next() ? ticks.value().value(0).toInt() : 0;
        }
        const qint64 modified_ms = QFileInfo(path).exists() ? QFileInfo(path).lastModified().toMSecsSinceEpoch() : 0;
        const qint64 age_ms = modified_ms > 0 ? qMax<qint64>(0, now_ms - modified_ms) : -1;
        for (const QString& cohort : cohorts) {
            QVector<QJsonObject> completed;
            int open = 0;
            for (const QJsonValue& value : records) {
                const QJsonObject record = value.toObject();
                const bool selected = (cohort != QLatin1String("agreement") && cohort != QLatin1String("conflict")) ||
                                      record.value(QStringLiteral("relationship")).toString() == cohort;
                if (!selected) continue;
                if (record.value(QStringLiteral("status")).toString() == QLatin1String("open")) ++open;
                else if (record.value(QStringLiteral("status")).toString() == QLatin1String("completed")) completed.append(record);
            }
            std::sort(completed.begin(), completed.end(), [](const QJsonObject& a, const QJsonObject& b) {
                return a.value(QStringLiteral("settled_at_ms")).toVariant().toLongLong() <
                       b.value(QStringLiteral("settled_at_ms")).toVariant().toLongLong();
            });
            const bool chronos = cohort == QLatin1String("chronos_alone");
            const QString pnl_key = chronos ? QStringLiteral("chronos_pnl") : QStringLiteral("control_pnl");
            const QString win_key = chronos ? QStringLiteral("chronos_won") : QStringLiteral("control_won");
            double pnl = 0.0, peak = 0.0, drawdown = 0.0; int wins = 0;
            for (const QJsonObject& record : completed) {
                pnl += record.value(pnl_key).toDouble(); peak = qMax(peak, pnl); drawdown = qMax(drawdown, peak - pnl);
                wins += record.value(win_key).toBool() ? 1 : 0;
            }
            QString producer_status = QStringLiteral("RUNNING");
            QString last_error = parse_error;
            if (!parse_error.isEmpty()) producer_status = QStringLiteral("ERROR");
            else if (state.isEmpty()) producer_status = QStringLiteral("WAITING");
            else if (source_ticks <= 0) {
                producer_status = QStringLiteral("WAITING");
                last_error = QStringLiteral("settlement-aligned Pyth feed unavailable");
            } else if (records.isEmpty()) producer_status = QStringLiteral("WAITING");
            else if (age_ms > 2 * 60 * 60 * 1000LL) producer_status = QStringLiteral("STALE");
            ++active;
            if (producer_status == QLatin1String("STALE")) ++stale;
            if (producer_status == QLatin1String("ERROR")) ++errors;
            if (producer_status == QLatin1String("WAITING")) ++waiting;
            entries.append(QJsonObject{
                {"strategy_id", QStringLiteral("shadow:chronos:%1:%2").arg(definition.family.toLower(), cohort)},
                {"kind", QStringLiteral("chronos_%1_%2").arg(definition.family.toLower(), cohort)},
                {"symbols", definition.symbols}, {"market", QStringLiteral("prediction")},
                {"horizon", QStringLiteral("1h")}, {"authority", QStringLiteral("paper_research_only_no_order_api")},
                {"book_status", QStringLiteral("active")}, {"producer", definition.producer},
                {"producer_status", producer_status}, {"data_age_ms", age_ms},
                {"freshness_source", QStringLiteral("ledger file mtime")}, {"ledger", path},
                {"last_error", last_error}, {"open", open}, {"resolved", completed.size()},
                {"skip_reasons", skip_reasons}, {"last_decision", last_run},
                {"diagnostic_runs", diagnostics.value(QStringLiteral("runs")).toInt()},
                {"net_pnl", pnl}, {"max_drawdown", drawdown},
                {"hit_rate", completed.isEmpty() ? 0.0 : static_cast<double>(wins) / completed.size()},
                {"eligible", false}, {"hypothetical", true}});
        }
    }
}

// Retires removed legacy books. Venue-specific scalp books remain active so
// lower-cost venues and future fee changes can be tested independently. Only
// old scalp rows without a venue are retired because their P&L cannot be
// attributed to an executable fee schedule.
// Kalshi books are intentionally retained: the crypto producer now prices
// executable YES/NO asks from target, close time, spot, fees, and exit reserve.
// This runs at the end of every seed_default_strategies() call so a
// reseed durably kills a pre-existing row of a removed kind even though
// register_strategy() itself only ever inserts, never mutates.
Result<void> retire_removed_kinds() {
    auto r = Database::instance().execute(
        "UPDATE sandbox_strategy SET status = 'retired' "
        "WHERE status = 'active' AND (kind IN ('btc5m', 'chronos2_5m') "
        "OR (kind='scalp' AND json_extract(params_json, '$.venue') IS NULL))",
        {});
    if (r.is_err())
        return Result<void>::err(r.error());
    return Result<void>::ok();
}

} // namespace

QString strategy_id_for(const QString& kind, const QString& symbols_csv, const QJsonObject& params) {
    const QString canonical = kind + QStringLiteral("|") + symbols_csv + QStringLiteral("|") +
                               params_json_compact(params);
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

Result<QString> register_strategy(const QString& kind, const QString& symbols_csv, const QJsonObject& params,
                                   const QString& notes) {
    const QString id = strategy_id_for(kind, symbols_csv, params);
    const QString params_json = params_json_compact(params);
    // A default-constructed QString() is *null* (not just empty), and Qt's
    // SQL driver binds a null QString as SQL NULL — which would violate
    // notes' NOT NULL constraint and, under INSERT OR IGNORE, be silently
    // swallowed (no row inserted, no error). Normalize to a non-null empty
    // string so the column's DEFAULT ''-shaped contract holds.
    const QString notes_value = notes.isNull() ? QString(QLatin1String("")) : notes;
    auto& db = Database::instance();

    // INSERT OR IGNORE — never REPLACE/UPDATE. If a row with this id already
    // exists (i.e. this exact kind/symbols/params triple was registered
    // before), the insert is a silent no-op: the original row's params_json,
    // notes, and created_at are left exactly as they were.
    auto ins = db.execute(
        "INSERT OR IGNORE INTO sandbox_strategy (strategy_id, kind, symbols, params_json, created_at, notes) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        {id, kind, symbols_csv, params_json, QDateTime::currentMSecsSinceEpoch(), notes_value});
    if (ins.is_err())
        return Result<QString>::err(ins.error());

    auto sel = db.execute("SELECT strategy_id FROM sandbox_strategy WHERE strategy_id = ?", {id});
    if (sel.is_err())
        return Result<QString>::err(sel.error());
    if (!sel.value().next())
        return Result<QString>::err(("strategy_id not found after insert: " + id).toStdString());
    return Result<QString>::ok(sel.value().value(0).toString());
}

Result<QList<StrategyRow>> list_strategies(const QString& status_filter) {
    QString sql = QStringLiteral(
        "SELECT strategy_id, kind, symbols, params_json, status, notes, created_at FROM sandbox_strategy");
    QVariantList args;
    if (!status_filter.isEmpty()) {
        sql += QStringLiteral(" WHERE status = ?");
        args << status_filter;
    }
    sql += QStringLiteral(" ORDER BY created_at");

    auto r = Database::instance().execute(sql, args);
    if (r.is_err())
        return Result<QList<StrategyRow>>::err(r.error());

    QList<StrategyRow> rows;
    auto& q = r.value();
    while (q.next()) {
        StrategyRow row;
        row.strategy_id = q.value(0).toString();
        row.kind = q.value(1).toString();
        row.symbols = q.value(2).toString();
        row.params_json = q.value(3).toString();
        row.status = q.value(4).toString();
        row.notes = q.value(5).toString();
        row.created_at = q.value(6).toLongLong();
        rows.append(row);
    }
    return Result<QList<StrategyRow>>::ok(rows);
}

Result<QJsonObject> strategy_registry_snapshot(const QString& profile, qint64 now_ms) {
    auto strategies = list_strategies();
    if (strategies.is_err()) return Result<QJsonObject>::err(strategies.error());
    auto scored = leaderboard(profile);
    if (scored.is_err()) return Result<QJsonObject>::err(scored.error());

    QHash<QString, LeaderboardRow> board;
    for (const auto& row : scored.value()) board.insert(row.strategy_id, row);
    QJsonArray entries;
    int stale = 0, errors = 0, waiting = 0, active = 0;
    auto& db = Database::instance();
    for (const StrategyRow& row : strategies.value()) {
        const QJsonDocument params_doc = QJsonDocument::fromJson(row.params_json.toUtf8());
        const QJsonObject params = params_doc.isObject() ? params_doc.object() : QJsonObject{};
        const QString source = params.value(QStringLiteral("journal_source")).toString(
            params.value(QStringLiteral("source")).toString());
        QString horizon = params.value(QStringLiteral("horizon")).toString();
        if (horizon.isEmpty() && params.contains(QStringLiteral("horizon_sec")))
            horizon = QStringLiteral("%1s").arg(params.value(QStringLiteral("horizon_sec")).toInt());
        QString market = QStringLiteral("crypto");
        if (row.kind == QLatin1String("kalshi") || row.kind.contains(QStringLiteral("weather")))
            market = QStringLiteral("prediction");
        else if (row.kind.contains(QStringLiteral("equity")) ||
                 row.symbols.contains(QStringLiteral("AAPL")) || row.symbols.contains(QStringLiteral("SPY")))
            market = QStringLiteral("equity");

        qint64 newest_ms = 0;
        QString last_error;
        int open = 0;
        int total_positions = 0;
        auto positions = db.execute(
            "SELECT MAX(created_at),"
            " SUM(CASE WHEN state IN ('open','pending_fill') THEN 1 ELSE 0 END), COUNT(*)"
            " FROM sandbox_position WHERE strategy_id=?", {row.strategy_id});
        if (positions.is_ok() && positions.value().next()) {
            newest_ms = positions.value().value(0).toLongLong();
            open = positions.value().value(1).toInt();
            total_positions = positions.value().value(2).toInt();
        }
        auto latest_quality = db.execute(
            "SELECT data_quality FROM sandbox_position WHERE strategy_id=? "
            "ORDER BY created_at DESC LIMIT 1", {row.strategy_id});
        if (latest_quality.is_ok() && latest_quality.value().next()) {
            const QString quality = latest_quality.value().value(0).toString();
            if (quality != QLatin1String("ok") && !quality.isEmpty()) last_error = quality;
        }
        const qint64 age_ms = newest_ms > 0 ? qMax<qint64>(0, now_ms - newest_ms) : -1;
        const qint64 configured_age = qMax<qint64>(params.value(QStringLiteral("max_age_sec")).toInt() * 1000LL,
                                                   15 * 60 * 1000LL);
        QString producer = QStringLiteral("RUNNING");
        if (row.status == QLatin1String("paused")) producer = QStringLiteral("PAUSED");
        else if (row.status == QLatin1String("retired")) producer = QStringLiteral("RETIRED");
        else if (source.isEmpty()) producer = QStringLiteral("NO PRODUCER");
        else if (!last_error.isEmpty()) producer = QStringLiteral("ERROR");
        else if (newest_ms <= 0) producer = QStringLiteral("WAITING");
        else if (age_ms > configured_age) producer = QStringLiteral("STALE");
        if (row.status == QLatin1String("active")) ++active;
        if (producer == QLatin1String("STALE")) ++stale;
        if (producer == QLatin1String("ERROR")) ++errors;
        if (producer == QLatin1String("WAITING") || producer == QLatin1String("NO PRODUCER")) ++waiting;

        int resolved = 0; double pnl = 0.0, drawdown = 0.0, hit_rate = 0.0;
        bool degraded = false, hypothetical = params.value(QStringLiteral("hypothetical")).toBool();
        const auto score = board.constFind(row.strategy_id);
        if (score != board.constEnd()) {
            resolved = score->resolved; pnl = score->net_pnl; drawdown = score->max_drawdown;
            hit_rate = score->hit_rate; degraded = score->degraded > 0; hypothetical = score->hypothetical;
        }
        EligibilityInput eligibility;
        eligibility.resolved = resolved;
        eligibility.total_positions = total_positions;
        eligibility.net_pnl = pnl;
        eligibility.max_drawdown = drawdown;
        eligibility.hypothetical = hypothetical;
        eligibility.degraded = degraded;
        const bool eligible = evaluate_eligibility(eligibility).eligible;
        entries.append(QJsonObject{{"strategy_id", row.strategy_id}, {"kind", row.kind},
            {"symbols", row.symbols}, {"market", market}, {"horizon", horizon},
            {"authority", params.value(QStringLiteral("paper_only")).toBool(true)
                              ? QStringLiteral("paper_only") : QStringLiteral("sandbox_only")},
            {"book_status", row.status}, {"producer", source}, {"producer_status", producer},
            {"data_age_ms", age_ms}, {"freshness_source", QStringLiteral("sandbox_position.created_at")},
            {"ledger", QStringLiteral("sandbox_position+sandbox_score")}, {"last_error", last_error},
            {"open", open}, {"resolved", resolved}, {"net_pnl", pnl}, {"max_drawdown", drawdown},
            {"hit_rate", hit_rate}, {"eligible", eligible}, {"hypothetical", hypothetical}});
    }
    // On-demand AI strategy factories are real implemented strategies even
    // before a sandbox book is registered. Keep them visible in the same
    // inventory instead of silently limiting "Strategies" to proof books.
    ai_strategy::StrategyRegistry ai_registry;
    ai_strategy::register_builtin_strategies(ai_registry);
    for (const auto& info : ai_registry.list()) {
        entries.append(QJsonObject{{"strategy_id", QStringLiteral("ai:") + info.name},
            {"kind", info.name}, {"symbols", QString()}, {"market", QStringLiteral("multi_asset")},
            {"horizon", QStringLiteral("on_demand")}, {"authority", QStringLiteral("advisory_only")},
            {"book_status", QStringLiteral("available")}, {"producer", QStringLiteral("ai_strategy_factory")},
            {"producer_status", QStringLiteral("ON DEMAND")}, {"data_age_ms", -1},
            {"freshness_source", QStringLiteral("request_context")}, {"ledger", QStringLiteral("ai run artifacts")},
            {"last_error", QString()}, {"open", 0}, {"resolved", 0}, {"net_pnl", 0.0},
            {"max_drawdown", 0.0}, {"hit_rate", 0.0}, {"eligible", false}, {"hypothetical", true}});
    }
    // JSON-backed forward trials are intentionally outside sandbox_position,
    // but they are still real strategies. Adapt their immutable ledgers into
    // the same read-only registry so GUI and CLI cannot silently omit them.
    append_shadow_ledgers(entries, now_ms, stale, errors, waiting, active);
    return Result<QJsonObject>::ok(QJsonObject{{"schema", "strategy-registry/v1"},
        {"generated_at_ms", now_ms}, {"profile", profile}, {"strategies", entries},
        {"summary", QJsonObject{{"total", entries.size()}, {"active", active}, {"stale", stale},
                                 {"errors", errors}, {"waiting", waiting}}}});
}

Result<void> set_status(const QString& strategy_id, const QString& status) {
    static const QSet<QString> kValidStatuses = {QStringLiteral("active"), QStringLiteral("paused"),
                                                  QStringLiteral("retired")};
    if (!kValidStatuses.contains(status))
        return Result<void>::err(("invalid status (must be active|paused|retired): " + status).toStdString());

    auto r = Database::instance().execute("UPDATE sandbox_strategy SET status = ? WHERE strategy_id = ?",
                                           {status, strategy_id});
    if (r.is_err())
        return Result<void>::err(r.error());
    return Result<void>::ok();
}

QVector<SpotLaneSeed> spot_lane_grid() {
    struct Venue {
        const char* venue;
        const char* symbols;
        bool is_crypto;
        double maker_bps;
        double taker_bps;
        double half_spread_bps;
        double slippage_bps;
        const char* fee_source;
    };
    // Honest per-venue cost profiles. Crypto maker/taker mirror the existing
    // scalp seeds; Alpaca equities are commission-free, so their cost is the
    // spread + slippage, not a fee.
    // Venue identifiers MUST match what the producers emit (the scalp producer
    // writes coinbase_advanced / kraken_pro; candidate selection is exact-match).
    // Coinbase tier-1 maker/taker come from the shared venue fee table
    // (services/crypto/CryptoFees.h) — the bps constants live only there.
    const auto coinbase_tier1 =
        services::crypto::coinbase_fee_tier_by_key(QStringLiteral("coinbase_advanced")).value();
    const Venue venues[] = {
        {"coinbase_advanced", "BTC-USD,ETH-USD,SOL-USD", true, coinbase_tier1.maker_bps,
         coinbase_tier1.taker_bps, 2.0, 1.0,
         "Coinbase Advanced account tier; verify before live"},
        {"kraken_pro", "BTC-USD,ETH-USD,SOL-USD", true, 40.0, 80.0, 2.0, 1.0,
         "Kraken Pro account tier; verify before live"},
        {"alpaca", "AAPL,NVDA,MSFT,SPY,QQQ", false, 0.0, 0.0, 3.0, 2.0,
         "Alpaca commission-free equities; spread/slippage is the cost"},
    };
    constexpr double kMakerThroughBps = 5.0;

    // edge_margin_bps is the edge a lane is TESTING FOR: its target is set at
    // the honest round-trip cost PLUS this margin, so hitting the target always
    // clears cost (a win never loses). Stop risks half the target.
    const auto lane = [&](const Venue& v, const QString& liquidity, double edge_margin_bps,
                          int horizon_sec, int max_age_sec) {
        const bool maker = liquidity == QLatin1String("maker");
        const double cost_bps = honest_round_trip_cost_bps(maker, v.half_spread_bps, v.slippage_bps,
                                                           v.maker_bps, v.taker_bps);
        const double target_bps = cost_bps + edge_margin_bps;
        const double stop_bps = target_bps / 2.0;
        return QJsonObject{
            {"notional_usd", 50.0},
            {"venue", QString::fromLatin1(v.venue)},
            {"liquidity", liquidity},
            {"maker_bps", v.maker_bps},
            {"taker_bps", v.taker_bps},
            {"half_spread_bps", v.half_spread_bps},
            // Only a resting maker pays the queue/adverse-selection cost; a
            // taker crosses immediately and has no through requirement.
            {"maker_fill_through_bps", maker ? kMakerThroughBps : 0.0},
            {"slippage_bps", v.slippage_bps},
            {"entry_offset_bps", 1.0},
            {"round_trip_cost_bps", cost_bps},
            {"edge_margin_bps", edge_margin_bps},
            {"target_bps", target_bps},
            {"stop_bps", stop_bps},
            {"horizon_sec", horizon_sec},
            {"max_age_sec", max_age_sec},
            {"paper_only", true},
            {"fee_profile_source", QString::fromLatin1(v.fee_source)}};
    };

    // A lane reuses an existing signal producer (hold the signal constant,
    // vary execution). Lanes with a source are activatable; those without
    // (maker spread-capture -- market-making, no directional signal; equity
    // scalp -- no sub-minute equity producer) are documented but not seeded.
    const auto with_source = [](QJsonObject params, const QString& source,
                                const QString& journal = QString()) {
        params.insert(QStringLiteral("source"), source);
        if (!journal.isEmpty())
            params.insert(QStringLiteral("journal_source"), journal);
        return params;
    };

    QVector<SpotLaneSeed> grid;
    for (const Venue& v : venues) {
        const QString symbols = QString::fromLatin1(v.symbols);

        // Edge margins we test for, by style. scalp seeks a small fast edge;
        // swing seeks a larger move; maker spread-capture a thin edge.
        constexpr double kScalpMarginBps = 40.0;
        constexpr double kSwingMarginBps = 120.0;
        constexpr double kMakerMarginBps = 20.0;

        // scalp maker/taker: reuse scalp_decisions on crypto venues. Equity
        // scalp has no producer yet, so those lanes stay deferred (no source).
        QJsonObject scalp_maker = lane(v, QStringLiteral("maker"), kScalpMarginBps, 900, 15);
        QJsonObject scalp_taker = lane(v, QStringLiteral("taker"), kScalpMarginBps, 900, 15);
        if (v.is_crypto) {
            scalp_maker = with_source(scalp_maker, QStringLiteral("scalp_decisions"));
            scalp_taker = with_source(scalp_taker, QStringLiteral("scalp_decisions"));
        }
        grid.append({QStringLiteral("scalp"), symbols, scalp_maker});
        grid.append({QStringLiteral("scalp"), symbols, scalp_taker});

        // swing: reuse the venue's directional feed (crypto recommend / equity
        // forecast), taker entry, longer hold, wider bracket.
        const QString swing_journal = v.is_crypto ? QStringLiteral("edge crypto-recommend")
                                                  : QStringLiteral("chronos2-equity-forecast");
        grid.append({QStringLiteral("swing"), symbols,
                     with_source(lane(v, QStringLiteral("taker"), kSwingMarginBps, 14400, 900),
                                 QStringLiteral("edge_journal"), swing_journal)});

        // maker spread-capture: fed by the maker_decisions producer on crypto
        // venues (DaemonMakerEngine). Equity maker has no producer -> deferred.
        QJsonObject maker_lane = lane(v, QStringLiteral("maker"), kMakerMarginBps, 900, 15);
        if (v.is_crypto)
            maker_lane = with_source(maker_lane, QStringLiteral("maker_decisions"));
        grid.append({QStringLiteral("maker"), symbols, maker_lane});
    }
    return grid;
}

bool lane_is_tradeable(const QJsonObject& params) {
    return params.value(QStringLiteral("target_bps")).toDouble() >
           params.value(QStringLiteral("round_trip_cost_bps")).toDouble();
}

Result<QList<QString>> seed_default_strategies() {
    struct Seed {
        QString kind;
        QString symbols;
        QJsonObject params;
    };

    // Season-1 defaults — params objects are binding per the sandbox core plan
    // (task 2 brief); do not tweak field values without updating the brief.
    //
    // Real-horizon reshape (2026-07-07 plan): btc5m and chronos2_5m have no
    // venue / no edge and are retired outright. Scalp experiments are now
    // explicitly venue- and fee-profile-specific. spot and kalshi
    // Every experiment configuration is content-addressed. Kalshi uses a v2
    // protocol with explicit horizon, entry-time cohort, and exit policy so
    // unlike experiments can never share a score.
    QList<Seed> seeds = {
        {QStringLiteral("scalp"), QStringLiteral("BTC-USD,ETH-USD,SOL-USD"),
         QJsonObject{{"notional_usd", 50.0},
                     {"source", "scalp_decisions"},
                     {"venue", "kraken_pro"},
                     {"liquidity", "maker"},
                     {"maker_bps", 25.0},
                     {"taker_bps", 40.0},
                     {"max_age_sec", 15},
                     {"entry_offset_bps", 1.0},
                     {"target_bps", 85.0},
                     {"stop_bps", 45.0},
                     {"horizon_sec", 900},
                     {"paper_only", true},
                     {"fee_profile_source", "Kraken Pro account tier; verify before live"}}},
        {QStringLiteral("scalp"), QStringLiteral("BTC-USD,ETH-USD,SOL-USD"),
         QJsonObject{{"notional_usd", 50.0},
                     {"source", "scalp_decisions"},
                     {"venue", "coinbase_advanced"},
                     {"liquidity", "maker"},
                     {"maker_bps", 40.0},
                     {"taker_bps", 60.0},
                     {"max_age_sec", 15},
                     {"entry_offset_bps", 1.0},
                     {"target_bps", 120.0},
                     {"stop_bps", 60.0},
                     {"horizon_sec", 900},
                     {"paper_only", true},
                     {"fee_profile_source", "Coinbase Advanced account tier; verify before live"}}},
        {QStringLiteral("spot"), QStringLiteral("BTC-USD,ETH-USD,SOL-USD"),
         QJsonObject{{"notional_usd", 50.0},
                     {"source", "edge_journal"},
                     {"journal_source", "edge crypto-recommend"},
                     {"venue", "coinbase"},
                     {"min_confidence", 0.8},
                     {"min_horizon_sec", 3600},
                     {"max_age_sec", 900},
                     {"target_move_pct", 2.0},
                     {"stop_move_pct", 1.0},
                     {"horizon_sec", 3600}}},
        {QStringLiteral("spot"), QStringLiteral("BTC-USD,ETH-USD,SOL-USD"),
         QJsonObject{{"notional_usd", 50.0},
                     {"source", "edge_journal"},
                     {"journal_source", "edge crypto-recommend"},
                     {"venue", "coinbase"},
                     {"min_confidence", 0.8},
                     {"min_horizon_sec", 3600},
                     {"max_age_sec", 900},
                     {"target_move_pct", 3.0},
                     {"stop_move_pct", 1.5},
                     {"horizon_sec", 14400}}},
        {QStringLiteral("spot"), QStringLiteral("BTC-USD,ETH-USD,SOL-USD"),
         QJsonObject{{"notional_usd", 50.0},
                     {"source", "edge_journal"},
                     {"journal_source", "edge crypto-recommend"},
                     {"venue", "coinbase"},
                     {"min_confidence", 0.8},
                     {"min_horizon_sec", 3600},
                     {"max_age_sec", 900},
                     {"target_move_pct", 5.0},
                     {"stop_move_pct", 2.5},
                     {"horizon_sec", 86400}}},
        {QStringLiteral("long_short"), QStringLiteral("BTC-USD"),
         QJsonObject{{"notional_usd", 50.0},
                     {"source", "edge_journal"},
                     {"journal_source", "edge long-short-strategy"},
                     {"venue", "coinbase_perps"},
                     {"hypothetical", true},
                     {"paper_only", true},
                     {"max_age_sec", 600},
                     {"target_bps", 100.0},
                     {"stop_bps", 45.0},
                     {"horizon_sec", 300}}},
        {QStringLiteral("chronos2"), QStringLiteral("BTC-USD"),
         QJsonObject{{"notional_usd", 50.0},
                     {"source", "edge_journal"},
                     {"journal_source", "chronos2-forecast"},
                     {"venue", "coinbase"},
                     {"model_id", "amazon/chronos-2"},
                     {"horizon", "15m"},
                     {"min_expected_move_bps", 15.0},
                     {"price_forecast", true},
                     {"paper_only", true},
                     {"max_age_sec", 1800},
                     {"target_bps", 45.0},
                     {"stop_bps", 25.0},
                     {"horizon_sec", 900}}},
        {QStringLiteral("chronos2_1h"), QStringLiteral("BTC-USD"),
         QJsonObject{{"notional_usd", 50.0},
                     {"source", "edge_journal"},
                     {"journal_source", "chronos2-forecast"},
                     {"venue", "coinbase"},
                     {"model_id", "amazon/chronos-2"},
                     {"horizon", "1h"},
                     {"min_expected_move_bps", 35.0},
                     {"price_forecast", true},
                     {"paper_only", true},
                     {"max_age_sec", 7200},
                     {"target_bps", 100.0},
                     {"stop_bps", 50.0},
                     {"horizon_sec", 3600}}},
        {QStringLiteral("chronos2_1d"), QStringLiteral("BTC-USD"),
         QJsonObject{{"notional_usd", 50.0},
                     {"source", "edge_journal"},
                     {"journal_source", "chronos2-forecast"},
                     {"venue", "coinbase"},
                     {"model_id", "amazon/chronos-2"},
                     {"horizon", "1d"},
                     {"min_expected_move_bps", 75.0},
                     {"price_forecast", true},
                     {"paper_only", true},
                     {"max_age_sec", 93600},
                     {"target_bps", 250.0},
                     {"stop_bps", 125.0},
                     {"horizon_sec", 86400}}},
        {QStringLiteral("chronos2_equity"), QStringLiteral("AAPL,NVDA,MSFT,SPY,QQQ"),
         QJsonObject{{"notional_usd", 100.0},
                     {"source", "edge_journal"},
                     {"journal_source", "chronos2-equity-forecast"},
                     {"model_id", "amazon/chronos-2"},
                     {"horizon", "1d"},
                     {"min_expected_move_bps", 50.0},
                     {"price_forecast", true},
                     {"paper_only", true},
                     {"max_age_sec", 93600},
                     {"target_bps", 150.0},
                     {"stop_bps", 75.0},
                     {"horizon_sec", 86400}}},
    };

    struct KalshiCohort {
        const char* horizon;
        const char* entry_cohort;
        int min_seconds_left;
        int max_seconds_left;
        int horizon_sec;
    };
    static constexpr KalshiCohort kKalshiCohorts[] = {
        {"15m", "late", 20, 60, 900},
        {"15m", "middle", 61, 300, 900},
        {"15m", "early", 301, 900, 900},
        {"1h", "late", 30, 180, 3600},
        {"1h", "middle", 181, 900, 3600},
        {"1h", "early", 901, 3600, 3600},
        {"daily", "late", 300, 3600, 86400},
        {"daily", "middle", 3601, 21600, 86400},
        {"daily", "early", 21601, 172800, 86400},
    };
    for (const KalshiCohort& cohort : kKalshiCohorts) {
        // Managed (cash-out) exit is now the DEFAULT and the `settlement`
        // (ride-to-settlement) control cohort is retired. The A/B on the paper
        // prediction book answered the design question: managed beats settlement
        // -0.187 vs -0.253 realized P&L per $1 notional (+3.3pp win rate). Active
        // settlement strategies from the prior grid are auto-retired by the
        // reconciliation below (their ids fall out of the seed set); their
        // already-open positions still settle normally, preserving evidence.
        for (const QString& exit_policy : {QStringLiteral("managed")}) {
            QJsonObject params{{"notional_usd", 2.0},
                               {"source", "edge_journal"},
                               {"journal_source", "kalshi auto-plan"},
                               {"venue", "kalshi"},
                               {"prediction", true},
                               {"paper_only", true},
                               {"experiment_protocol", "kalshi-v2"},
                               {"horizon", QString::fromLatin1(cohort.horizon)},
                               {"entry_cohort", QString::fromLatin1(cohort.entry_cohort)},
                               {"exit_policy", exit_policy},
                               {"allowed_side", "both"},
                               {"min_seconds_left", cohort.min_seconds_left},
                               {"max_seconds_left", cohort.max_seconds_left},
                               {"min_entry_probability", 0.05},
                               {"max_entry_probability", 0.95},
                               {"horizon_sec", cohort.horizon_sec},
                               {"max_age_sec", 5},
                               {"max_open_positions", 0},
                               {"take_profit_pct", 0.20},
                               {"stop_loss_pct", 0.20},
                               {"fee_model", "journal_exact"}};
            seeds.append(Seed{QStringLiteral("kalshi"), QStringLiteral("BTC-USD"), params});
        }
    }

    // Kalshi WEATHER (daily city high-temp) — paper lane for the validated
    // forecast edge. Distinct journal_source ("kalshi weather-plan") isolates it
    // from the BTC crypto lane and bypasses the BTC-only micro-evidence gate.
    // Rows are produced by scripts/kalshi_advise/weather_producer.py (Open-Meteo
    // forecast vs Kalshi price on near-money brackets). Daily horizon -> a wide
    // seconds_left window and a 6h max_age (the producer runs periodically, not
    // per-second). Managed exits (take_profit/stop_loss/edge_reversal + LOCK_WIN)
    // apply exactly as the crypto lane. PAPER ONLY — no live weather rung.
    {
        QJsonObject wparams{{"notional_usd", 2.0},
                            {"source", "edge_journal"},
                            {"journal_source", "kalshi weather-plan"},
                            {"venue", "kalshi"},
                            {"prediction", true},
                            {"paper_only", true},
                            {"experiment_protocol", "kalshi-weather-v1"},
                            {"horizon", "daily"},
                            // HOLD to daily resolution: the weather edge (+10c OOS) was
                            // validated at settlement. Intraday managed exits (stop_loss/
                            // edge_reversal) shook positions out on illiquid-book noise
                            // before the forecast realized (observed live 2026-08-03).
                            {"exit_policy", "settlement"},
                            {"allowed_side", "both"},
                            {"min_seconds_left", 3600},
                            {"max_seconds_left", 86400},
                            {"min_entry_probability", 0.05},
                            {"max_entry_probability", 0.95},
                            {"horizon_sec", 86400},
                            {"max_age_sec", 21600},
                            {"max_open_positions", 0},
                            {"take_profit_pct", 0.20},
                            {"stop_loss_pct", 0.20},
                            {"fee_model", "journal_exact"}};
        seeds.append(Seed{QStringLiteral("kalshi_weather"), QStringLiteral("US-WEATHER"), wparams});
    }

    // Activate the spot measurement grid lanes that reuse an existing producer
    // (scalp -> scalp_decisions; swing -> edge crypto-recommend / chronos2-
    // equity-forecast). Lanes without a source (maker spread-capture, equity
    // scalp) are deferred until they have a signal feed. Each carries honest
    // execution params, so the resolver scores it cost-net; they mint new
    // strategy_ids and never disturb the legacy optimistic books.
    for (const auto& g : spot_lane_grid()) {
        // Seed only producer-backed lanes whose target clears its honest
        // round-trip cost -- a guaranteed loser is not a candidate strategy.
        if (g.params.contains(QStringLiteral("source")) && lane_is_tradeable(g.params))
            seeds.append(Seed{g.kind, g.symbols, g.params});
    }

    QList<QString> ids;
    ids.reserve(seeds.size());
    for (const auto& s : seeds) {
        auto r = register_strategy(s.kind, s.symbols, s.params);
        if (r.is_err())
            return Result<QList<QString>>::err(r.error());
        ids.append(r.value());
    }

    auto retired = retire_removed_kinds();
    if (retired.is_err())
        return Result<QList<QString>>::err(retired.error());

    // Quarantine every older parameterization of a managed seed kind. This
    // preserves its evidence for audit while preventing contaminated legacy
    // Kalshi books (and stale fee profiles in other lanes) from continuing to
    // collect positions or appearing in the active proof leaderboard.
    const QSet<QString> current_ids(ids.begin(), ids.end());
    const QSet<QString> managed_kinds = {
        QStringLiteral("scalp"), QStringLiteral("spot"), QStringLiteral("swing"),
        QStringLiteral("maker"),
        QStringLiteral("kalshi"), QStringLiteral("kalshi_weather"), QStringLiteral("long_short"), QStringLiteral("chronos2"),
        QStringLiteral("chronos2_1h"), QStringLiteral("chronos2_1d"), QStringLiteral("chronos2_equity")};
    auto active = list_strategies(QStringLiteral("active"));
    if (active.is_err())
        return Result<QList<QString>>::err(active.error());
    for (const auto& row : active.value()) {
        if (!managed_kinds.contains(row.kind) || current_ids.contains(row.strategy_id))
            continue;
        auto status = set_status(row.strategy_id, QStringLiteral("retired"));
        if (status.is_err())
            return Result<QList<QString>>::err(status.error());
    }

    return Result<QList<QString>>::ok(ids);
}

} // namespace openmarketterminal::services::sandbox
