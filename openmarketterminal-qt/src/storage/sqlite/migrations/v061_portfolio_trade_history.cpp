// v061_portfolio_trade_history — Trade-history sync for Portfolio Account
// Sync: lets a synced account's fills be imported into
// portfolio_transactions without ever duplicating on re-sync.
//
// Background: AccountSyncService (v060) mirrors a connected account's
// HOLDINGS into portfolio_assets. This migration adds what's needed to
// additionally import that account's TRANSACTION history:
//   - portfolio_transactions.external_id — the source's stable id for one
//     fill (e.g. "broker:<order_id>", "<exchange_id>:<trade_id>"). Empty
//     ('') for manually-entered transactions, which stay unconstrained.
//   - idx_ptx_external — a PARTIAL unique index on
//     (portfolio_id, external_id) WHERE external_id != ''. Only synced rows
//     (non-empty external_id) are deduplicated; manual rows (external_id ==
//     '') are excluded from the uniqueness constraint entirely, so multiple
//     manual transactions never collide with each other. Combined with
//     PortfolioRepository::import_transaction's INSERT OR IGNORE, re-syncing
//     the same account twice is a no-op the second time.
//
// Existing rows get '' defaults — backwards compatible.

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace openmarketterminal {
namespace {

// NOTE: static helpers are renamed v061_* on purpose — Unity builds may
// concatenate multiple migration .cpp files into a single TU, and an
// anonymous-namespace `sql()` here would clash with the same symbol in
// another migration. Rename so the file stays grep-able by version.
static Result<void> v061_sql(QSqlDatabase& db, const char* stmt) {
    QSqlQuery q(db);
    if (!q.exec(stmt))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}

static bool v061_column_exists(QSqlDatabase& db, const QString& table, const QString& column) {
    QSqlQuery q(db);
    q.exec(QString("PRAGMA table_info(%1)").arg(table));
    while (q.next()) {
        if (q.value(1).toString().compare(column, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

Result<void> apply_v061(QSqlDatabase& db) {
    if (!v061_column_exists(db, "portfolio_transactions", "external_id")) {
        auto r = v061_sql(db, "ALTER TABLE portfolio_transactions ADD COLUMN external_id TEXT DEFAULT ''");
        if (r.is_err())
            return r;
    }
    return v061_sql(db, "CREATE UNIQUE INDEX IF NOT EXISTS idx_ptx_external "
                        "ON portfolio_transactions(portfolio_id, external_id) WHERE external_id != ''");
}

} // anonymous namespace

void register_migration_v061() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({61, "portfolio_trade_history", apply_v061});
}

} // namespace openmarketterminal
