#include "storage/sqlite/migrations/MigrationRunner.h"

namespace openmarketterminal {
namespace {

Result<void> run_sql(QSqlDatabase& db, const QString& sql) {
    QSqlQuery query(db);
    if (!query.exec(sql))
        return Result<void>::err(query.lastError().text().toStdString());
    return Result<void>::ok();
}

Result<void> apply_v063(QSqlDatabase& db) {
    return run_sql(db, QStringLiteral(
        "CREATE TABLE IF NOT EXISTS ai_handler ("
        " name TEXT PRIMARY KEY,"
        " strategy TEXT NOT NULL,"
        " provider TEXT,"
        " symbols TEXT,"
        " interval_sec INTEGER NOT NULL DEFAULT 60,"
        " allowed_venues TEXT,"
        " max_notional REAL NOT NULL DEFAULT 0,"
        " max_position REAL NOT NULL DEFAULT 0,"
        " enabled INTEGER NOT NULL DEFAULT 0,"
        " notes TEXT,"
        " created_at INTEGER"
        ")"));
}

} // namespace

void register_migration_v063() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    MigrationRunner::register_migration({63, QStringLiteral("ai_handler"), apply_v063});
}

} // namespace openmarketterminal
