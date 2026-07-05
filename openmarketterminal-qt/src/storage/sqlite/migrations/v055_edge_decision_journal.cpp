#include "storage/sqlite/migrations/MigrationRunner.h"

namespace openmarketterminal {
namespace {

Result<void> exec(QSqlDatabase& db, const QString& sql) {
    QSqlQuery q(db);
    if (!q.exec(sql))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}

Result<void> apply_v055(QSqlDatabase& db) {
    auto r = exec(db,
        "CREATE TABLE IF NOT EXISTS edge_decision_journal ("
        " id TEXT PRIMARY KEY,"
        " created_at INTEGER NOT NULL,"
        " updated_at INTEGER NOT NULL,"
        " venue TEXT NOT NULL DEFAULT '',"
        " symbol TEXT NOT NULL DEFAULT '',"
        " horizon TEXT NOT NULL DEFAULT '',"
        " market_id TEXT NOT NULL DEFAULT '',"
        " question TEXT NOT NULL DEFAULT '',"
        " direction TEXT NOT NULL DEFAULT '',"
        " side TEXT NOT NULL DEFAULT '',"
        " call TEXT NOT NULL DEFAULT '',"
        " gate TEXT NOT NULL DEFAULT '',"
        " market_probability REAL NOT NULL DEFAULT 0,"
        " model_probability REAL NOT NULL DEFAULT 0,"
        " raw_edge REAL NOT NULL DEFAULT 0,"
        " edge_after_cost REAL NOT NULL DEFAULT 0,"
        " gate_edge REAL NOT NULL DEFAULT 0,"
        " spread_cost REAL NOT NULL DEFAULT 0,"
        " fee_cost REAL NOT NULL DEFAULT 0,"
        " liquidity_score REAL NOT NULL DEFAULT 0,"
        " confidence REAL NOT NULL DEFAULT 0,"
        " seconds_left INTEGER NOT NULL DEFAULT -1,"
        " data_status TEXT NOT NULL DEFAULT '',"
        " freshness_json TEXT NOT NULL DEFAULT '{}',"
        " features_json TEXT NOT NULL DEFAULT '{}',"
        " reasons TEXT NOT NULL DEFAULT '',"
        " outcome INTEGER NOT NULL DEFAULT -1,"
        " resolved_at INTEGER NOT NULL DEFAULT 0,"
        " source TEXT NOT NULL DEFAULT '')");
    if (r.is_err()) return r;

    r = exec(db,
        "CREATE INDEX IF NOT EXISTS idx_edge_decision_journal_scope "
        "ON edge_decision_journal(symbol, horizon, created_at)");
    if (r.is_err()) return r;

    r = exec(db,
        "CREATE INDEX IF NOT EXISTS idx_edge_decision_journal_market "
        "ON edge_decision_journal(market_id, created_at)");
    if (r.is_err()) return r;

    return exec(db,
        "CREATE INDEX IF NOT EXISTS idx_edge_decision_journal_outcome "
        "ON edge_decision_journal(outcome, created_at)");
}

} // namespace

void register_migration_v055() {
    static bool registered = false;
    if (registered)
        return;
    registered = true;
    MigrationRunner::register_migration({55, "edge_decision_journal", apply_v055});
}

} // namespace openmarketterminal
