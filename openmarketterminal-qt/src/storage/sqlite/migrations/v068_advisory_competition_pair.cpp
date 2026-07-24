#include "storage/sqlite/migrations/MigrationRunner.h"

namespace openmarketterminal {
namespace {
Result<void> apply_v068(QSqlDatabase& db) {
    QSqlQuery q(db);
    if (!q.exec("ALTER TABLE edge_advisory_challenge ADD COLUMN competition_pair_id TEXT NOT NULL DEFAULT ''"))
        return Result<void>::err(q.lastError().text().toStdString());
    if (!q.exec("CREATE INDEX IF NOT EXISTS idx_advisory_competition_pair "
                "ON edge_advisory_challenge(competition_pair_id, created_at)"))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}
}
void register_migration_v068() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    MigrationRunner::register_migration({68, "advisory_competition_pair", apply_v068});
}
} // namespace openmarketterminal
