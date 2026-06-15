// v050_pm_paper — storage for the prediction-market paper-trading book (Phase B).
//
//   pm_paper_account   — the single (id=1) cash ledger row for the paper book.
//                        Seeded lazily by PmPaperRepository::cash() with the
//                        default 100000 USD; adjust_cash() debits/credits it.
//   pm_paper_positions — one row per opened paper position. "One OPEN long per
//                        (venue,asset_id)" is enforced in the repo (get_open
//                        filters status='open'), NOT by a UNIQUE constraint:
//                        closed rows persist as history so a later close→re-buy
//                        cannot collide.
//
// CREATE TABLE IF NOT EXISTS makes re-run idempotent; any exec failure is a real
// error (no duplicate-column swallowing — this is a CREATE migration).

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace openmarketterminal {
namespace {

Result<void> apply_v050(QSqlDatabase& db) {
    QSqlQuery account(db);
    const char* account_sql =
        "CREATE TABLE IF NOT EXISTS pm_paper_account ("
        " id INTEGER PRIMARY KEY CHECK(id=1),"
        " cash REAL NOT NULL DEFAULT 100000,"
        " currency TEXT NOT NULL DEFAULT 'USD',"
        " created_at TEXT NOT NULL DEFAULT ''"
        ")";
    if (!account.exec(QString::fromUtf8(account_sql)))
        return Result<void>::err(account.lastError().text().toStdString());

    QSqlQuery positions(db);
    const char* positions_sql =
        "CREATE TABLE IF NOT EXISTS pm_paper_positions ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " venue TEXT NOT NULL,"
        " market_id TEXT NOT NULL,"
        " asset_id TEXT NOT NULL,"
        " outcome TEXT NOT NULL,"
        " category TEXT NOT NULL DEFAULT '',"
        " contracts REAL NOT NULL DEFAULT 0,"
        " avg_price REAL NOT NULL DEFAULT 0,"
        " cost_basis REAL NOT NULL DEFAULT 0,"
        " opened_at TEXT NOT NULL DEFAULT '',"
        " status TEXT NOT NULL DEFAULT 'open'"
        ")";
    if (!positions.exec(QString::fromUtf8(positions_sql)))
        return Result<void>::err(positions.lastError().text().toStdString());

    return Result<void>::ok();
}

} // namespace

void register_migration_v050() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({50, "pm_paper", apply_v050});
}

} // namespace openmarketterminal
