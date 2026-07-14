#include "services/sandbox/SandboxRegistry.h"

#include "storage/sqlite/Database.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QSet>
#include <QVariantList>

namespace openmarketterminal::services::sandbox {

namespace {

QString params_json_compact(const QJsonObject& params) {
    return QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact));
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
    static const Venue venues[] = {
        {"coinbase", "BTC-USD,ETH-USD,SOL-USD", true, 40.0, 60.0, 2.0, 1.0,
         "Coinbase Advanced account tier; verify before live"},
        {"kraken", "BTC-USD,ETH-USD,SOL-USD", true, 25.0, 40.0, 2.0, 1.0,
         "Kraken Pro account tier; verify before live"},
        {"alpaca", "AAPL,NVDA,MSFT,SPY,QQQ", false, 0.0, 0.0, 3.0, 2.0,
         "Alpaca commission-free equities; spread/slippage is the cost"},
    };
    constexpr double kMakerThroughBps = 5.0;

    const auto lane = [&](const Venue& v, const QString& liquidity, double target_bps,
                          double stop_bps, int horizon_sec, int max_age_sec) {
        return QJsonObject{
            {"notional_usd", 50.0},
            {"venue", QString::fromLatin1(v.venue)},
            {"liquidity", liquidity},
            {"maker_bps", v.maker_bps},
            {"taker_bps", v.taker_bps},
            {"half_spread_bps", v.half_spread_bps},
            // Only a resting maker pays the queue/adverse-selection cost; a
            // taker crosses immediately and has no through requirement.
            {"maker_fill_through_bps",
             liquidity == QLatin1String("maker") ? kMakerThroughBps : 0.0},
            {"slippage_bps", v.slippage_bps},
            {"entry_offset_bps", 1.0},
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

        // scalp maker/taker: reuse scalp_decisions on crypto venues. Equity
        // scalp has no producer yet, so those lanes stay deferred (no source).
        QJsonObject scalp_maker = lane(v, QStringLiteral("maker"), 85.0, 45.0, 900, 15);
        QJsonObject scalp_taker = lane(v, QStringLiteral("taker"), 85.0, 45.0, 900, 15);
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
                     with_source(lane(v, QStringLiteral("taker"), 300.0, 150.0, 14400, 900),
                                 QStringLiteral("edge_journal"), swing_journal)});

        // maker spread-capture: market-making, no directional producer -> deferred.
        grid.append({QStringLiteral("maker"), symbols,
                     lane(v, QStringLiteral("maker"), 2.0 * v.half_spread_bps, 40.0, 900, 15)});
    }
    return grid;
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
        for (const QString& exit_policy : {QStringLiteral("settlement"), QStringLiteral("managed")}) {
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

    // Activate the spot measurement grid lanes that reuse an existing producer
    // (scalp -> scalp_decisions; swing -> edge crypto-recommend / chronos2-
    // equity-forecast). Lanes without a source (maker spread-capture, equity
    // scalp) are deferred until they have a signal feed. Each carries honest
    // execution params, so the resolver scores it cost-net; they mint new
    // strategy_ids and never disturb the legacy optimistic books.
    for (const auto& g : spot_lane_grid()) {
        if (g.params.contains(QStringLiteral("source")))
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
        QStringLiteral("kalshi"), QStringLiteral("long_short"), QStringLiteral("chronos2"),
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
