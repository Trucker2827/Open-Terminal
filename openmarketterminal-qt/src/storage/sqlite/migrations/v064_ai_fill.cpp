#include "storage/sqlite/migrations/MigrationRunner.h"

namespace openmarketterminal {
namespace {

Result<void> run_sql(QSqlDatabase& db, const QString& sql) {
    QSqlQuery query(db);
    if (!query.exec(sql))
        return Result<void>::err(query.lastError().text().toStdString());
    return Result<void>::ok();
}

Result<void> apply_v064(QSqlDatabase& db) {
    auto result = run_sql(db, QStringLiteral(
        "CREATE TABLE IF NOT EXISTS ai_fill ("
        " id TEXT PRIMARY KEY,"
        " handler TEXT NOT NULL,"
        " symbol TEXT NOT NULL,"
        " side TEXT NOT NULL,"
        " quantity REAL NOT NULL,"
        " fill_price REAL NOT NULL,"
        " fee REAL NOT NULL DEFAULT 0,"
        " realized_pnl REAL NOT NULL DEFAULT 0,"
        " ts INTEGER NOT NULL,"
        " draft_id TEXT"
        ")"));
    if (result.is_err()) return result;
    result = run_sql(db, QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_ai_fill_scope "
        "ON ai_fill(handler,symbol,ts)"));
    if (result.is_err()) return result;
    // Append-only: reject any UPDATE/DELETE at the DB layer (defense in depth).
    result = run_sql(db, QStringLiteral(
        "CREATE TRIGGER IF NOT EXISTS ai_fill_no_update "
        "BEFORE UPDATE ON ai_fill BEGIN "
        "SELECT RAISE(ABORT,'ai_fill rows are immutable'); END"));
    if (result.is_err()) return result;
    return run_sql(db, QStringLiteral(
        "CREATE TRIGGER IF NOT EXISTS ai_fill_no_delete "
        "BEFORE DELETE ON ai_fill BEGIN "
        "SELECT RAISE(ABORT,'ai_fill rows are append-only'); END"));
}

} // namespace

void register_migration_v064() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    MigrationRunner::register_migration({64, QStringLiteral("ai_fill"), apply_v064});
}

} // namespace openmarketterminal
