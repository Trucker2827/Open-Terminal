#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace openmarketterminal {
namespace {

Result<void> apply_v053(QSqlDatabase& db) {
    QSqlQuery obs(db);
    const char* observations =
        "CREATE TABLE IF NOT EXISTS edge_prediction_observations ("
        " id TEXT PRIMARY KEY,"
        " venue TEXT NOT NULL DEFAULT '',"
        " symbol TEXT NOT NULL DEFAULT '',"
        " horizon TEXT NOT NULL DEFAULT '',"
        " market_id TEXT NOT NULL DEFAULT '',"
        " question TEXT NOT NULL DEFAULT '',"
        " direction TEXT NOT NULL DEFAULT '',"
        " market_probability REAL NOT NULL DEFAULT 0,"
        " btc_anchor_probability REAL NOT NULL DEFAULT 0.5,"
        " move_5s_pct REAL NOT NULL DEFAULT 0,"
        " move_15s_pct REAL NOT NULL DEFAULT 0,"
        " move_60s_pct REAL NOT NULL DEFAULT 0,"
        " liquidity_score REAL NOT NULL DEFAULT 0,"
        " spread_cost REAL NOT NULL DEFAULT 0,"
        " fee_cost REAL NOT NULL DEFAULT 0,"
        " seconds_left INTEGER NOT NULL DEFAULT -1,"
        " outcome INTEGER NOT NULL DEFAULT -1,"
        " source TEXT NOT NULL DEFAULT '',"
        " observed_at INTEGER NOT NULL DEFAULT 0,"
        " resolved_at INTEGER NOT NULL DEFAULT 0"
        ")";
    if (!obs.exec(QString::fromUtf8(observations)))
        return Result<void>::err(obs.lastError().text().toStdString());

    QSqlQuery ticks(db);
    const char* raw_ticks =
        "CREATE TABLE IF NOT EXISTS edge_prediction_raw_ticks ("
        " id TEXT PRIMARY KEY,"
        " symbol TEXT NOT NULL DEFAULT '',"
        " source TEXT NOT NULL DEFAULT '',"
        " price REAL NOT NULL DEFAULT 0,"
        " exchange_ts INTEGER NOT NULL DEFAULT 0,"
        " received_ts INTEGER NOT NULL DEFAULT 0"
        ")";
    if (!ticks.exec(QString::fromUtf8(raw_ticks)))
        return Result<void>::err(ticks.lastError().text().toStdString());

    QSqlQuery snaps(db);
    const char* snapshots =
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
        ")";
    if (!snaps.exec(QString::fromUtf8(snapshots)))
        return Result<void>::err(snaps.lastError().text().toStdString());

    QSqlQuery models(db);
    const char* model_sql =
        "CREATE TABLE IF NOT EXISTS edge_prediction_models ("
        " id TEXT PRIMARY KEY,"
        " symbol TEXT NOT NULL DEFAULT '',"
        " horizon TEXT NOT NULL DEFAULT '',"
        " sample_count INTEGER NOT NULL DEFAULT 0,"
        " positive_count INTEGER NOT NULL DEFAULT 0,"
        " base_rate REAL NOT NULL DEFAULT 0.5,"
        " brier_score REAL NOT NULL DEFAULT 0,"
        " weights_json TEXT NOT NULL DEFAULT '{}',"
        " trained_at INTEGER NOT NULL DEFAULT 0"
        ")";
    if (!models.exec(QString::fromUtf8(model_sql)))
        return Result<void>::err(models.lastError().text().toStdString());

    QSqlQuery outputs(db);
    const char* output_sql =
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
        ")";
    if (!outputs.exec(QString::fromUtf8(output_sql)))
        return Result<void>::err(outputs.lastError().text().toStdString());

    QSqlQuery idx1(db);
    if (!idx1.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_edge_pred_obs_scope ON edge_prediction_observations(symbol, horizon, outcome, observed_at)")))
        return Result<void>::err(idx1.lastError().text().toStdString());

    QSqlQuery idx2(db);
    if (!idx2.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_edge_pred_models_scope ON edge_prediction_models(symbol, horizon)")))
        return Result<void>::err(idx2.lastError().text().toStdString());

    QSqlQuery idx3(db);
    if (!idx3.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_edge_pred_ticks_scope ON edge_prediction_raw_ticks(symbol, received_ts)")))
        return Result<void>::err(idx3.lastError().text().toStdString());

    QSqlQuery idx4(db);
    if (!idx4.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_edge_pred_snapshots_scope ON edge_prediction_market_snapshots(symbol, horizon, observed_at)")))
        return Result<void>::err(idx4.lastError().text().toStdString());

    QSqlQuery idx5(db);
    if (!idx5.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_edge_pred_outputs_scope ON edge_prediction_model_outputs(symbol, horizon, as_of)")))
        return Result<void>::err(idx5.lastError().text().toStdString());

    return Result<void>::ok();
}

} // namespace

void register_migration_v053() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({53, "edge_prediction_models", apply_v053});
}

} // namespace openmarketterminal
