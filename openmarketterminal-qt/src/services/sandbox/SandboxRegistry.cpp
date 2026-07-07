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

// Retires (status='retired') any ACTIVE book whose kind is no longer part of
// the season-1 seed set: scalp/btc5m (no venue / no edge; fee-dead sub-minute
// or unlisted 5m book) and chronos2_5m (dropped alongside scalp/btc5m in the
// horizon reshape). This runs at the end of every seed_default_strategies()
// call so a reseed durably kills a pre-existing row of a removed kind even
// though register_strategy() itself only ever inserts, never mutates.
Result<void> retire_removed_kinds() {
    auto r = Database::instance().execute(
        "UPDATE sandbox_strategy SET status = 'retired' "
        "WHERE status = 'active' AND kind IN ('scalp', 'btc5m', 'chronos2_5m')",
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

Result<QList<QString>> seed_default_strategies() {
    struct Seed {
        QString kind;
        QString symbols;
        QJsonObject params;
    };

    // Season-1 defaults — params objects are binding per the sandbox core plan
    // (task 2 brief); do not tweak field values without updating the brief.
    //
    // Real-horizon reshape (2026-07-07 plan): scalp, btc5m, and chronos2_5m
    // have no venue / no edge (sub-minute or unlisted horizons) and are
    // retired outright -- see retire_removed_kinds() below. spot and kalshi
    // are each three horizon variants of the SAME kind ('spot'/'kalshi'):
    // params differ (horizon_sec/target_move_pct/stop_move_pct for spot,
    // horizon_sec/max_age_sec for kalshi), so each variant content-hashes to
    // a distinct strategy_id and all three coexist under one journal feed
    // (migration v058's per-strategy dedup makes that safe).
    const QList<Seed> seeds = {
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
        // max_age_sec on the prediction/hypothetical books (task-5 review
        // fix 3): the executor's staleness cutoff -- without it, activating
        // a book would backfill positions from arbitrarily old gate-pass
        // journal history. Changing these params changes the strategy_ids
        // (content-addressed) -- acceptable pre-season.
        {QStringLiteral("kalshi"), QStringLiteral("BTC-USD,ETH-USD,SOL-USD"),
         QJsonObject{{"notional_usd", 50.0},
                     {"source", "edge_journal"},
                     {"journal_source", "edge journal-kalshi-scan"},
                     {"max_age_sec", 900},
                     {"prediction", true},
                     {"horizon_sec", 900}}},
        {QStringLiteral("kalshi"), QStringLiteral("BTC-USD,ETH-USD,SOL-USD"),
         QJsonObject{{"notional_usd", 50.0},
                     {"source", "edge_journal"},
                     {"journal_source", "edge journal-kalshi-scan"},
                     {"max_age_sec", 3600},
                     {"prediction", true},
                     {"horizon_sec", 3600}}},
        {QStringLiteral("kalshi"), QStringLiteral("BTC-USD,ETH-USD,SOL-USD"),
         QJsonObject{{"notional_usd", 50.0},
                     {"source", "edge_journal"},
                     {"journal_source", "edge journal-kalshi-scan"},
                     {"max_age_sec", 86400},
                     {"prediction", true},
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

    return Result<QList<QString>>::ok(ids);
}

} // namespace openmarketterminal::services::sandbox
