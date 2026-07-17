#include "storage/sqlite/migrations/MigrationRunner.h"

namespace openmarketterminal {
namespace {

Result<void> apply_v065(QSqlDatabase& db) {
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("ALTER TABLE ai_handler ADD COLUMN market TEXT NOT NULL DEFAULT ''")))
        return Result<void>::err(query.lastError().text().toStdString());
    return Result<void>::ok();
}

} // namespace

void register_migration_v065() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    MigrationRunner::register_migration({65, QStringLiteral("ai_handler_market"), apply_v065});
}

} // namespace openmarketterminal
