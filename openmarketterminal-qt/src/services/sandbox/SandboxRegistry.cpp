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
    const QList<Seed> seeds = {
        {QStringLiteral("scalp"), QStringLiteral("BTC-USD"),
         QJsonObject{{"notional_usd", 50.0},
                     {"source", "scalp_decisions"},
                     {"max_age_sec", 15},
                     {"entry_offset_bps", 1.0},
                     {"target_bps", 25.0},
                     {"stop_bps", 15.0},
                     {"horizon_sec", 900}}},
        {QStringLiteral("spot"), QStringLiteral("BTC-USD,ETH-USD,SOL-USD"),
         QJsonObject{{"notional_usd", 50.0},
                     {"source", "edge_journal"},
                     {"journal_source", "edge crypto-recommend"},
                     {"min_confidence", 0.8},
                     {"min_horizon_sec", 60},
                     {"max_age_sec", 900},
                     {"target_move_pct", 5.0},
                     {"stop_move_pct", 2.5},
                     {"horizon_sec", 86400}}},
        // max_age_sec on the prediction/hypothetical books (task-5 review
        // fix 3): the executor's staleness cutoff -- without it, activating
        // a book would backfill positions from arbitrarily old gate-pass
        // journal history. Changing these params changes the strategy_ids
        // (content-addressed) -- acceptable pre-season.
        {QStringLiteral("btc5m"), QStringLiteral("BTC-USD"),
         QJsonObject{{"notional_usd", 50.0},
                     {"source", "edge_journal"},
                     {"journal_source", "edge journal-evaluate-btc5m-live"},
                     {"max_age_sec", 900},
                     {"prediction", true}}},
        {QStringLiteral("kalshi"), QStringLiteral("BTC-USD,ETH-USD,SOL-USD"),
         QJsonObject{{"notional_usd", 50.0},
                     {"source", "edge_journal"},
                     {"journal_source", "edge journal-kalshi-scan"},
                     {"max_age_sec", 3600},
                     {"prediction", true}}},
        {QStringLiteral("long_short"), QStringLiteral("BTC-USD"),
         QJsonObject{{"notional_usd", 50.0},
                     {"source", "edge_journal"},
                     {"journal_source", "edge long-short-strategy"},
                     {"hypothetical", true},
                     {"max_age_sec", 600},
                     {"target_bps", 100.0},
                     {"stop_bps", 45.0},
                     {"horizon_sec", 300}}},
    };

    QList<QString> ids;
    ids.reserve(seeds.size());
    for (const auto& s : seeds) {
        auto r = register_strategy(s.kind, s.symbols, s.params);
        if (r.is_err())
            return Result<QList<QString>>::err(r.error());
        ids.append(r.value());
    }
    return Result<QList<QString>>::ok(ids);
}

} // namespace openmarketterminal::services::sandbox
