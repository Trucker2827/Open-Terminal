#include "storage/sqlite/migrations/MigrationRunner.h"

namespace openmarketterminal {
namespace {

Result<void> exec(QSqlDatabase& db, const QString& sql) {
    QSqlQuery q(db);
    if (!q.exec(sql))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}

Result<void> apply_v056(QSqlDatabase& db) {
    auto r = exec(db,
        "CREATE TABLE IF NOT EXISTS sandbox_strategy ("
        " strategy_id TEXT PRIMARY KEY,"
        " kind TEXT NOT NULL,"                 // 'scalp'|'spot'|'btc5m'|'kalshi'|'long_short'
        " symbols TEXT NOT NULL,"              // CSV, normalized BTC-USD
        " params_json TEXT NOT NULL,"
        " status TEXT NOT NULL DEFAULT 'active',"  // 'active'|'paused'|'retired'
        " created_at INTEGER NOT NULL,"
        " notes TEXT NOT NULL DEFAULT '')");
    if (r.is_err()) return r;
    r = exec(db,
        "CREATE TABLE IF NOT EXISTS sandbox_position ("
        " position_id TEXT PRIMARY KEY,"
        " strategy_id TEXT NOT NULL,"
        " decision_id TEXT NOT NULL UNIQUE,"
        " symbol TEXT NOT NULL,"
        " side TEXT NOT NULL,"                 // 'buy'|'long'|'short'|'yes'|'no'
        " hypothetical INTEGER NOT NULL DEFAULT 0,"
        " qty REAL NOT NULL,"
        " limit_price REAL NOT NULL,"
        " target_price REAL,"
        " stop_price REAL,"
        " expires_at INTEGER NOT NULL,"        // ms epoch
        " state TEXT NOT NULL,"                // 'pending_fill'|'open'|'closed'|'unfilled'
        " opened_at INTEGER,"
        " closed_at INTEGER,"
        " entry_fee REAL NOT NULL DEFAULT 0,"
        " exit_fee REAL NOT NULL DEFAULT 0,"
        " realized_pnl REAL,"                  // net of all costs; NULL until closed
        " close_reason TEXT,"                  // 'target'|'stop'|'expiry'|'unfilled'|'resolved'
        " data_quality TEXT NOT NULL DEFAULT 'ok',"
        " notional_usd REAL NOT NULL,"
        " created_at INTEGER NOT NULL DEFAULT 0)");
    if (r.is_err()) return r;
    r = exec(db, "CREATE INDEX IF NOT EXISTS idx_sandbox_position_strategy ON sandbox_position (strategy_id, state, created_at)");
    if (r.is_err()) return r;
    r = exec(db,
        "CREATE TABLE IF NOT EXISTS sandbox_fill ("
        " fill_id TEXT PRIMARY KEY,"
        " position_id TEXT NOT NULL,"
        " ts INTEGER NOT NULL,"
        " kind TEXT NOT NULL,"                 // 'open'|'fill'|'target'|'stop'|'expiry'|'unfilled'|'resolved'
        " price REAL NOT NULL DEFAULT 0,"
        " fee REAL NOT NULL DEFAULT 0,"
        " note TEXT NOT NULL DEFAULT '')");
    if (r.is_err()) return r;
    r = exec(db, "CREATE INDEX IF NOT EXISTS idx_sandbox_fill_position ON sandbox_fill (position_id, ts)");
    if (r.is_err()) return r;
    r = exec(db,
        "CREATE TABLE IF NOT EXISTS sandbox_score ("
        " strategy_id TEXT NOT NULL,"
        " score_date TEXT NOT NULL,"           // UTC YYYY-MM-DD
        " resolved_count INTEGER NOT NULL DEFAULT 0,"
        " open_count INTEGER NOT NULL DEFAULT 0,"
        " unfilled_count INTEGER NOT NULL DEFAULT 0,"
        " net_pnl REAL NOT NULL DEFAULT 0,"
        " hit_rate REAL NOT NULL DEFAULT 0,"
        " avg_win REAL NOT NULL DEFAULT 0,"
        " avg_loss REAL NOT NULL DEFAULT 0,"
        " max_drawdown REAL NOT NULL DEFAULT 0,"
        " degraded_count INTEGER NOT NULL DEFAULT 0,"
        " gross_notional REAL NOT NULL DEFAULT 0,"
        " PRIMARY KEY (strategy_id, score_date))");
    return r;
}

} // namespace

void register_migration_v056() {
    static bool registered = false;
    if (registered)
        return;
    registered = true;
    MigrationRunner::register_migration({56, "strategy_sandbox", apply_v056});
}

} // namespace openmarketterminal
