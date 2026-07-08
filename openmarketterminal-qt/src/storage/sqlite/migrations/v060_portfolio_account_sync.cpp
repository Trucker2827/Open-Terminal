// v060_portfolio_account_sync — Track broker/CSV account-sync status for
// portfolios, and mark assets whose cost basis is known.
//
// Background: Portfolio Account Sync (this feature) lets a portfolio be
// kept in sync with an external source (a connected broker, or a
// periodically re-imported CSV). This migration adds the storage columns
// later tasks build on:
//   - portfolios.sync_source — identifier of the sync source (e.g. a
//     broker_account_id, or "csv"). Empty = manual, unsynced portfolio.
//   - portfolios.synced_at    — ISO-8601 timestamp of the last successful
//     sync. Empty = never synced.
//   - portfolios.sync_error   — last sync error message, if any. Empty =
//     no error / not applicable.
//   - portfolio_assets.has_cost_basis — 1 if the asset's cost basis is
//     known (e.g. from broker lot data), 0 if it had to be inferred/
//     defaulted. Defaults to 1 so existing rows (which already carry an
//     avg_cost) are treated as having a real cost basis.
//
// Existing rows get '' / 1 defaults — backwards compatible.

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace openmarketterminal {
namespace {

// NOTE: static helpers are renamed v060_* on purpose — Unity builds may
// concatenate multiple migration .cpp files into a single TU, and an
// anonymous-namespace `sql()` here would clash with the same symbol in v019
// or v021. The unity-build namespace trap is a documented gotcha for this
// codebase; rename or move into per-migration anonymous-namespaces. We pick
// rename so the file is grep-able by version.
static Result<void> v060_sql(QSqlDatabase& db, const char* stmt) {
    QSqlQuery q(db);
    if (!q.exec(stmt))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}

static bool v060_column_exists(QSqlDatabase& db, const QString& table, const QString& column) {
    QSqlQuery q(db);
    q.exec(QString("PRAGMA table_info(%1)").arg(table));
    while (q.next()) {
        if (q.value(1).toString().compare(column, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

Result<void> apply_v060(QSqlDatabase& db) {
    struct Add { const char* table; const char* col; const char* type_default; };
    const Add adds[] = {
        {"portfolios", "sync_source", "TEXT DEFAULT ''"},
        {"portfolios", "synced_at", "TEXT DEFAULT ''"},
        {"portfolios", "sync_error", "TEXT DEFAULT ''"},
        {"portfolio_assets", "has_cost_basis", "INTEGER DEFAULT 1"},
    };
    for (const auto& a : adds) {
        if (!v060_column_exists(db, a.table, a.col)) {
            auto r = v060_sql(db, QString("ALTER TABLE %1 ADD COLUMN %2 %3")
                                      .arg(a.table, a.col, a.type_default).toUtf8().constData());
            if (r.is_err())
                return r;
        }
    }
    return v060_sql(db, "CREATE INDEX IF NOT EXISTS idx_portfolios_sync_source "
                        "ON portfolios(sync_source)");
}

} // anonymous namespace

void register_migration_v060() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({60, "portfolio_account_sync", apply_v060});
}

} // namespace openmarketterminal
