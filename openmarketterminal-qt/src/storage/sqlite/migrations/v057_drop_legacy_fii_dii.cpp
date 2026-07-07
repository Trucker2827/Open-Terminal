// v057_drop_legacy_fii_dii — Remove dormant India NSE FII/DII storage (legacy v027).
//
// The planned FiiDiiService / fii_dii_scraper.py pipeline was never shipped and
// the F&O UI is out of the current build. Drop the orphaned table on upgrade.

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace openmarketterminal {
namespace {

Result<void> v057_sql(QSqlDatabase& db, const char* stmt) {
    QSqlQuery q(db);
    if (!q.exec(stmt))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}

Result<void> apply_v057(QSqlDatabase& db) {
    auto r = v057_sql(db, "DROP INDEX IF EXISTS idx_fii_dii_date");
    if (r.is_err())
        return r;
    return v057_sql(db, "DROP TABLE IF EXISTS fii_dii_daily");
}

} // anonymous namespace

void register_migration_v057() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({57, "drop_legacy_fii_dii", apply_v057});
}

} // namespace openmarketterminal
