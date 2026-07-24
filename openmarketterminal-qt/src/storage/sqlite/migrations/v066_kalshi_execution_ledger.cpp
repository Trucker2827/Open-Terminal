#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace openmarketterminal {
namespace {

Result<void> apply_v066(QSqlDatabase& db) {
    QSqlQuery orders(db);
    if (!orders.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS kalshi_live_orders ("
            " client_order_id TEXT PRIMARY KEY,"
            " order_id TEXT NOT NULL DEFAULT '',"
            " draft_id TEXT NOT NULL DEFAULT '',"
            " market_id TEXT NOT NULL DEFAULT '',"
            " asset_id TEXT NOT NULL DEFAULT '',"
            " action TEXT NOT NULL DEFAULT '',"
            " outcome TEXT NOT NULL DEFAULT '',"
            " state TEXT NOT NULL DEFAULT 'submitting',"
            " requested_count REAL NOT NULL DEFAULT 0,"
            " filled_count REAL NOT NULL DEFAULT 0,"
            " remaining_count REAL NOT NULL DEFAULT 0,"
            " limit_price REAL NOT NULL DEFAULT 0,"
            " average_fill_price REAL NOT NULL DEFAULT 0,"
            " fees_paid REAL NOT NULL DEFAULT 0,"
            " exchange_ts_ms INTEGER NOT NULL DEFAULT 0,"
            " created_at TEXT NOT NULL DEFAULT '',"
            " updated_at TEXT NOT NULL DEFAULT '',"
            " last_reconciled_at TEXT NOT NULL DEFAULT '',"
            " raw_json TEXT NOT NULL DEFAULT ''"
            ")")))
        return Result<void>::err(orders.lastError().text().toStdString());

    QSqlQuery fills(db);
    if (!fills.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS kalshi_live_fills ("
            " fill_key TEXT PRIMARY KEY,"
            " trade_id TEXT NOT NULL DEFAULT '',"
            " order_id TEXT NOT NULL DEFAULT '',"
            " client_order_id TEXT NOT NULL DEFAULT '',"
            " market_id TEXT NOT NULL DEFAULT '',"
            " asset_id TEXT NOT NULL DEFAULT '',"
            " action TEXT NOT NULL DEFAULT '',"
            " outcome TEXT NOT NULL DEFAULT '',"
            " count REAL NOT NULL DEFAULT 0,"
            " price REAL NOT NULL DEFAULT 0,"
            " fee REAL NOT NULL DEFAULT 0,"
            " exchange_ts_ms INTEGER NOT NULL DEFAULT 0,"
            " received_ts_ms INTEGER NOT NULL DEFAULT 0,"
            " source TEXT NOT NULL DEFAULT '',"
            " raw_json TEXT NOT NULL DEFAULT ''"
            ")")))
        return Result<void>::err(fills.lastError().text().toStdString());

    const QStringList indexes{
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_kalshi_orders_order_id ON kalshi_live_orders(order_id) WHERE order_id<>''"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_kalshi_orders_state ON kalshi_live_orders(state, updated_at)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_kalshi_orders_market ON kalshi_live_orders(market_id, state)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_kalshi_fills_client ON kalshi_live_fills(client_order_id, exchange_ts_ms)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_kalshi_fills_market ON kalshi_live_fills(market_id, exchange_ts_ms)")};
    for (const QString& sql : indexes) {
        QSqlQuery index(db);
        if (!index.exec(sql))
            return Result<void>::err(index.lastError().text().toStdString());
    }
    return Result<void>::ok();
}

} // namespace

void register_migration_v066() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    MigrationRunner::register_migration({66, QStringLiteral("kalshi_execution_ledger"), apply_v066});
}

} // namespace openmarketterminal
