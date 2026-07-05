#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace openmarketterminal {
namespace {

Result<void> exec_sql(QSqlDatabase& db, const QString& sql) {
    QSqlQuery q(db);
    if (!q.exec(sql))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}

Result<void> apply_v054(QSqlDatabase& db) {
    auto r = exec_sql(db, QStringLiteral(
        "CREATE TABLE IF NOT EXISTS edge_prediction_raw_ticks ("
        " id TEXT PRIMARY KEY,"
        " symbol TEXT NOT NULL DEFAULT '',"
        " source TEXT NOT NULL DEFAULT '',"
        " price REAL NOT NULL DEFAULT 0,"
        " exchange_ts INTEGER NOT NULL DEFAULT 0,"
        " received_ts INTEGER NOT NULL DEFAULT 0"
        ")"));
    if (r.is_err())
        return r;

    r = exec_sql(db, QStringLiteral(
        "CREATE TABLE IF NOT EXISTS edge_prediction_market_snapshots ("
        " id TEXT PRIMARY KEY,"
        " venue TEXT NOT NULL DEFAULT '',"
        " symbol TEXT NOT NULL DEFAULT '',"
        " horizon TEXT NOT NULL DEFAULT '',"
        " market_id TEXT NOT NULL DEFAULT '',"
        " question TEXT NOT NULL DEFAULT '',"
        " yes_price REAL NOT NULL DEFAULT 0,"
        " no_price REAL NOT NULL DEFAULT 0,"
        " spread_cost REAL NOT NULL DEFAULT 0,"
        " liquidity_score REAL NOT NULL DEFAULT 0,"
        " seconds_left INTEGER NOT NULL DEFAULT -1,"
        " observed_at INTEGER NOT NULL DEFAULT 0"
        ")"));
    if (r.is_err())
        return r;

    r = exec_sql(db, QStringLiteral(
        "CREATE TABLE IF NOT EXISTS edge_prediction_model_outputs ("
        " id TEXT PRIMARY KEY,"
        " symbol TEXT NOT NULL DEFAULT '',"
        " horizon TEXT NOT NULL DEFAULT '',"
        " direction TEXT NOT NULL DEFAULT '',"
        " readiness TEXT NOT NULL DEFAULT '',"
        " source TEXT NOT NULL DEFAULT '',"
        " probability REAL NOT NULL DEFAULT 0.5,"
        " confidence REAL NOT NULL DEFAULT 0,"
        " calibration_score REAL NOT NULL DEFAULT 0,"
        " sample_count INTEGER NOT NULL DEFAULT 0,"
        " as_of INTEGER NOT NULL DEFAULT 0,"
        " trained_at INTEGER NOT NULL DEFAULT 0"
        ")"));
    if (r.is_err())
        return r;

    r = exec_sql(db, QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_edge_pred_ticks_scope "
        "ON edge_prediction_raw_ticks(symbol, received_ts)"));
    if (r.is_err())
        return r;

    r = exec_sql(db, QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_edge_pred_snapshots_scope "
        "ON edge_prediction_market_snapshots(symbol, horizon, observed_at)"));
    if (r.is_err())
        return r;

    return exec_sql(db, QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_edge_pred_outputs_scope "
        "ON edge_prediction_model_outputs(symbol, horizon, as_of)"));
}

} // namespace

void register_migration_v054() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({54, "edge_prediction_feature_store", apply_v054});
}

} // namespace openmarketterminal
