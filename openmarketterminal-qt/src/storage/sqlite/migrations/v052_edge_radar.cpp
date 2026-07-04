#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace openmarketterminal {
namespace {

Result<void> apply_v052(QSqlDatabase& db) {
    QSqlQuery q(db);
    const char* sql =
        "CREATE TABLE IF NOT EXISTS edge_radar_ideas ("
        " id TEXT PRIMARY KEY,"
        " asset_class TEXT NOT NULL DEFAULT '',"
        " venue TEXT NOT NULL DEFAULT '',"
        " symbol TEXT NOT NULL DEFAULT '',"
        " market_id TEXT NOT NULL DEFAULT '',"
        " question TEXT NOT NULL DEFAULT '',"
        " side TEXT NOT NULL DEFAULT '',"
        " market_probability REAL NOT NULL DEFAULT 0,"
        " model_probability REAL NOT NULL DEFAULT 0,"
        " spread_cost REAL NOT NULL DEFAULT 0,"
        " fee_cost REAL NOT NULL DEFAULT 0,"
        " liquidity_score REAL NOT NULL DEFAULT 0,"
        " confidence REAL NOT NULL DEFAULT 0,"
        " raw_edge REAL NOT NULL DEFAULT 0,"
        " edge_after_cost REAL NOT NULL DEFAULT 0,"
        " recommendation TEXT NOT NULL DEFAULT '',"
        " thesis TEXT NOT NULL DEFAULT '',"
        " risk_notes TEXT NOT NULL DEFAULT '',"
        " status TEXT NOT NULL DEFAULT 'watching',"
        " tags TEXT NOT NULL DEFAULT '',"
        " created_at INTEGER NOT NULL DEFAULT 0,"
        " updated_at INTEGER NOT NULL DEFAULT 0,"
        " last_evaluated_at INTEGER NOT NULL DEFAULT 0"
        ")";
    if (!q.exec(QString::fromUtf8(sql)))
        return Result<void>::err(q.lastError().text().toStdString());

    QSqlQuery idx1(db);
    if (!idx1.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_edge_radar_status ON edge_radar_ideas(status, venue, asset_class)")))
        return Result<void>::err(idx1.lastError().text().toStdString());

    QSqlQuery idx2(db);
    if (!idx2.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_edge_radar_symbol ON edge_radar_ideas(symbol, market_id)")))
        return Result<void>::err(idx2.lastError().text().toStdString());

    return Result<void>::ok();
}

} // namespace

void register_migration_v052() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({52, "edge_radar", apply_v052});
}

} // namespace openmarketterminal
