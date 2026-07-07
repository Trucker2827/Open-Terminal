// v059_sandbox_position_dedup_repair -- make v058's per-strategy dedup robust.
//
// v058 is the intended schema change from global UNIQUE(decision_id) to
// UNIQUE(strategy_id, decision_id). This migration deliberately rebuilds the
// table again as a forward repair so developer/profile databases that already
// recorded v058 while still carrying the old unique constraint heal on open.

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace openmarketterminal {
namespace {

Result<void> v059_sql(QSqlDatabase& db, const QString& sql) {
    QSqlQuery q(db);
    if (!q.exec(sql))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}

Result<void> apply_v059(QSqlDatabase& db) {
    auto r = v059_sql(db,
        "CREATE TABLE sandbox_position_repair ("
        " position_id TEXT PRIMARY KEY,"
        " strategy_id TEXT NOT NULL,"
        " decision_id TEXT NOT NULL,"
        " symbol TEXT NOT NULL,"
        " side TEXT NOT NULL,"
        " hypothetical INTEGER NOT NULL DEFAULT 0,"
        " qty REAL NOT NULL,"
        " limit_price REAL NOT NULL,"
        " target_price REAL,"
        " stop_price REAL,"
        " expires_at INTEGER NOT NULL,"
        " state TEXT NOT NULL,"
        " opened_at INTEGER,"
        " closed_at INTEGER,"
        " entry_fee REAL NOT NULL DEFAULT 0,"
        " exit_fee REAL NOT NULL DEFAULT 0,"
        " realized_pnl REAL,"
        " close_reason TEXT,"
        " data_quality TEXT NOT NULL DEFAULT 'ok',"
        " notional_usd REAL NOT NULL,"
        " created_at INTEGER NOT NULL DEFAULT 0,"
        " UNIQUE(strategy_id, decision_id))");
    if (r.is_err()) return r;

    r = v059_sql(db,
        "INSERT INTO sandbox_position_repair (position_id, strategy_id, decision_id, symbol, side, hypothetical,"
        " qty, limit_price, target_price, stop_price, expires_at, state, opened_at, closed_at, entry_fee,"
        " exit_fee, realized_pnl, close_reason, data_quality, notional_usd, created_at)"
        " SELECT position_id, strategy_id, decision_id, symbol, side, hypothetical,"
        " qty, limit_price, target_price, stop_price, expires_at, state, opened_at, closed_at, entry_fee,"
        " exit_fee, realized_pnl, close_reason, data_quality, notional_usd, created_at"
        " FROM sandbox_position");
    if (r.is_err()) return r;

    r = v059_sql(db, "DROP INDEX IF EXISTS idx_sandbox_position_strategy");
    if (r.is_err()) return r;

    r = v059_sql(db, "DROP TABLE sandbox_position");
    if (r.is_err()) return r;

    r = v059_sql(db, "ALTER TABLE sandbox_position_repair RENAME TO sandbox_position");
    if (r.is_err()) return r;

    return v059_sql(db,
        "CREATE INDEX IF NOT EXISTS idx_sandbox_position_strategy ON sandbox_position (strategy_id, state, created_at)");
}

} // namespace

void register_migration_v059() {
    static bool registered = false;
    if (registered)
        return;
    registered = true;
    MigrationRunner::register_migration({59, "sandbox_position_dedup_repair", apply_v059});
}

} // namespace openmarketterminal
