#include "storage/sqlite/migrations/MigrationRunner.h"

namespace openmarketterminal {
namespace {
// EdgePredictionModelRepository::list_spot_price_series_since ranges on
// exchange_ts, but the only index on edge_prediction_raw_ticks was
// idx_edge_pred_ticks_scope(symbol, received_ts). SQLite could therefore bind
// only symbol=? and fell back to the query's degenerate received_ts>0 guard as
// its range, walking the entire per-symbol index partition on every call --
// 34.6M rows for BTC on the live database, 3.4s per call.
//
// The scalp/swing-gate jobs run that query on a schedule, several at once, so
// the scan starved the event-driven Kalshi planner of CPU and disk until it
// exceeded its deadline and was killed before it could journal anything. That
// is what froze kalshi-auto-plans.jsonl for a week.
//
// Measured on the live 43M-row table: 3.356s -> 0.056s (60x), same result set.
//
// exchange_ts and received_ts are NOT interchangeable -- 714,601 rows carry
// exchange_ts <= 0, which is why the query guards with exchange_ts>0. The fix
// has to be an index matching the predicate, never a swap to the indexed
// column.
Result<void> apply_v069(QSqlDatabase& db) {
    QSqlQuery q(db);
    if (!q.exec("CREATE INDEX IF NOT EXISTS idx_edge_pred_ticks_exchange "
                "ON edge_prediction_raw_ticks(symbol, exchange_ts)"))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}
}
void register_migration_v069() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    MigrationRunner::register_migration({69, "edge_ticks_exchange_index", apply_v069});
}
} // namespace openmarketterminal
