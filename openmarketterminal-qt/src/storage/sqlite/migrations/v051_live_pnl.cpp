// v051_live_pnl — storage for the LIVE realized-P&L ledger (Phase C, Task 2).
//
//   live_positions — one row per opened LIVE position. "One OPEN per
//                    (account,venue,instrument)" is a repo invariant
//                    (get_open filters status='open'), NOT a UNIQUE constraint:
//                    closed rows persist as history so a later close→re-open
//                    cannot collide. P&L-ONLY (NO cash side) — distinct from the
//                    pm_paper_* cash book. `instrument` = equity symbol or PM
//                    asset_id.
//   daily_pnl      — one row per UTC day: the running realized-P&L tally that the
//                    deterministic, AI-uncontrolled daily-loss gate reads.
//
// SCOPE (honest): accurate live realized P&L would need per-order broker fill
// reconciliation (a big subsystem, deferred). This MVP tracks realized P&L from
// the substrate's OWN fills at the price the substrate executed at (resolved /
// submitted price + tracked cost basis) — exact for sandbox/paper,
// resolved-price-approximate for real equity fills. Deterministic + conservative.
//
// CREATE TABLE IF NOT EXISTS makes re-run idempotent; any exec failure is a real
// error (no duplicate-column swallowing — this is a CREATE migration).

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace openmarketterminal {
namespace {

Result<void> apply_v051(QSqlDatabase& db) {
    QSqlQuery positions(db);
    const char* positions_sql =
        "CREATE TABLE IF NOT EXISTS live_positions ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " account TEXT NOT NULL DEFAULT '',"
        " venue TEXT NOT NULL DEFAULT '',"
        " instrument TEXT NOT NULL,"
        " qty REAL NOT NULL DEFAULT 0,"
        " avg_cost REAL NOT NULL DEFAULT 0,"
        " cost_basis REAL NOT NULL DEFAULT 0,"
        " opened_at TEXT NOT NULL DEFAULT '',"
        " status TEXT NOT NULL DEFAULT 'open'"
        ")";
    if (!positions.exec(QString::fromUtf8(positions_sql)))
        return Result<void>::err(positions.lastError().text().toStdString());

    QSqlQuery daily(db);
    const char* daily_sql =
        "CREATE TABLE IF NOT EXISTS daily_pnl ("
        " utc_day TEXT PRIMARY KEY,"
        " realized_pnl REAL NOT NULL DEFAULT 0,"
        " updated_at TEXT NOT NULL DEFAULT ''"
        ")";
    if (!daily.exec(QString::fromUtf8(daily_sql)))
        return Result<void>::err(daily.lastError().text().toStdString());

    return Result<void>::ok();
}

} // namespace

void register_migration_v051() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({51, "live_pnl", apply_v051});
}

} // namespace openmarketterminal
