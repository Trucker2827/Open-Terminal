// v058_sandbox_position_dedup — Scope sandbox_position dedup to per-strategy.
//
// v056 made sandbox_position.decision_id globally UNIQUE, and the executor's
// anti-join (`id NOT IN (SELECT decision_id FROM sandbox_position)`) read
// that dedup globally too. That starves any two active books that share a
// journal source (e.g. spot_1h + spot_4h both reading 'edge crypto-recommend')
// -- whichever book's cycle runs first "claims" the decision_id and the
// second book never sees it again, first-come-wins forever.
//
// SQLite cannot drop a column-level UNIQUE constraint in place, so this is a
// table rebuild: recreate sandbox_position with the exact same v056 column
// set but decision_id no longer column-UNIQUE, replaced by a table-level
// UNIQUE(strategy_id, decision_id) -- the same decision_id may now be opened
// by multiple distinct strategy_ids (one position per book), while a single
// book still cannot open the same decision twice.

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace openmarketterminal {
namespace {

Result<void> v058_sql(QSqlDatabase& db, const QString& sql) {
    QSqlQuery q(db);
    if (!q.exec(sql))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}

Result<void> apply_v058(QSqlDatabase& db) {
    auto r = v058_sql(db,
        "CREATE TABLE sandbox_position_new ("
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

    r = v058_sql(db,
        "INSERT INTO sandbox_position_new (position_id, strategy_id, decision_id, symbol, side, hypothetical,"
        " qty, limit_price, target_price, stop_price, expires_at, state, opened_at, closed_at, entry_fee,"
        " exit_fee, realized_pnl, close_reason, data_quality, notional_usd, created_at)"
        " SELECT position_id, strategy_id, decision_id, symbol, side, hypothetical,"
        " qty, limit_price, target_price, stop_price, expires_at, state, opened_at, closed_at, entry_fee,"
        " exit_fee, realized_pnl, close_reason, data_quality, notional_usd, created_at"
        " FROM sandbox_position");
    if (r.is_err()) return r;

    r = v058_sql(db, "DROP INDEX IF EXISTS idx_sandbox_position_strategy");
    if (r.is_err()) return r;

    r = v058_sql(db, "DROP TABLE sandbox_position");
    if (r.is_err()) return r;

    r = v058_sql(db, "ALTER TABLE sandbox_position_new RENAME TO sandbox_position");
    if (r.is_err()) return r;

    return v058_sql(db,
        "CREATE INDEX IF NOT EXISTS idx_sandbox_position_strategy ON sandbox_position (strategy_id, state, created_at)");
}

} // namespace

void register_migration_v058() {
    static bool registered = false;
    if (registered)
        return;
    registered = true;
    MigrationRunner::register_migration({58, "sandbox_position_dedup", apply_v058});
}

} // namespace openmarketterminal
