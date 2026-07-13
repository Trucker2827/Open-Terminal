#include "storage/sqlite/migrations/MigrationRunner.h"

namespace openmarketterminal {
namespace {

Result<void> run_sql(QSqlDatabase& db, const QString& sql) {
    QSqlQuery query(db);
    if (!query.exec(sql))
        return Result<void>::err(query.lastError().text().toStdString());
    return Result<void>::ok();
}

Result<void> apply_v062(QSqlDatabase& db) {
    auto result = run_sql(db, QStringLiteral(
        "CREATE TABLE IF NOT EXISTS decision_envelopes ("
        " id TEXT PRIMARY KEY,"
        " schema_version INTEGER NOT NULL,"
        " decision_ts INTEGER NOT NULL,"
        " created_at INTEGER NOT NULL,"
        " venue TEXT NOT NULL,"
        " symbol TEXT NOT NULL,"
        " horizon TEXT NOT NULL,"
        " market_id TEXT NOT NULL,"
        " side TEXT NOT NULL,"
        " verdict TEXT NOT NULL,"
        " source TEXT NOT NULL,"
        " journal_id TEXT NOT NULL DEFAULT '',"
        " content_hash TEXT NOT NULL UNIQUE,"
        " envelope_json TEXT NOT NULL"
        ")"));
    if (result.is_err()) return result;
    result = run_sql(db, QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_decision_envelopes_scope "
        "ON decision_envelopes(symbol,horizon,decision_ts DESC)"));
    if (result.is_err()) return result;
    result = run_sql(db, QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_decision_envelopes_journal "
        "ON decision_envelopes(journal_id)"));
    if (result.is_err()) return result;
    result = run_sql(db, QStringLiteral(
        "CREATE TRIGGER IF NOT EXISTS decision_envelopes_no_update "
        "BEFORE UPDATE ON decision_envelopes BEGIN "
        "SELECT RAISE(ABORT,'decision envelopes are immutable'); END"));
    if (result.is_err()) return result;
    return run_sql(db, QStringLiteral(
        "CREATE TRIGGER IF NOT EXISTS decision_envelopes_no_delete "
        "BEFORE DELETE ON decision_envelopes BEGIN "
        "SELECT RAISE(ABORT,'decision envelopes are append-only'); END"));
}

} // namespace

void register_migration_v062() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    MigrationRunner::register_migration({62, QStringLiteral("decision_envelopes"), apply_v062});
}

} // namespace openmarketterminal
