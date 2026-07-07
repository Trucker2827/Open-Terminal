// v034_strategy_portfolio — Strategy Portfolio persistence (Phase 3 §16) and
// Quantity Freeze limits (Phase 3 §17).
//
// saved_strategies: user-saved multi-leg option strategies. Legs are serialized
//   as a JSON array (one object per OptionsLeg) in the legs_json TEXT column —
//   strikes/expiries/quantities vary per leg so a child table buys little here.
//   See StrategyPortfolio.cpp for the leg schema.
//
// qty_freeze: per (symbol, exchange) maximum quantity an exchange accepts in a
//   single order. Looked up before placing an order; over-limit orders are
//   auto-split via place_split_orders. See QtyFreeze.cpp.
//
// Version note: v033 is reserved for the parallel Historify (DuckDB) work; this
// migration deliberately takes v034 to avoid colliding with it.

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace openmarketterminal {
namespace {

static Result<void> sql(QSqlDatabase& db, const char* stmt) {
    QSqlQuery q(db);
    if (!q.exec(stmt))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}

Result<void> apply_v034(QSqlDatabase& db) {
    // ── Saved strategy portfolio (§16) ──────────────────────────────────────
    auto r = sql(db, "CREATE TABLE IF NOT EXISTS saved_strategies ("
                     "  id TEXT PRIMARY KEY,"
                     "  name TEXT NOT NULL,"
                     "  underlying TEXT,"
                     "  notes TEXT,"
                     "  legs_json TEXT NOT NULL DEFAULT '[]',"
                     "  created_at TEXT"
                     ")");
    if (r.is_err())
        return r;

    r = sql(db, "CREATE INDEX IF NOT EXISTS idx_saved_strategies_underlying "
                "ON saved_strategies(underlying)");
    if (r.is_err())
        return r;

    // ── Quantity freeze limits (§17) ────────────────────────────────────────
    r = sql(db, "CREATE TABLE IF NOT EXISTS qty_freeze ("
                "  symbol TEXT NOT NULL,"
                "  exchange TEXT NOT NULL,"
                "  freeze_qty INTEGER NOT NULL,"
                "  PRIMARY KEY (symbol, exchange)"
                ")");
    if (r.is_err())
        return r;

    return Result<void>::ok();
}

} // anonymous namespace

void register_migration_v034() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({34, "strategy_portfolio", apply_v034});
}

} // namespace openmarketterminal
