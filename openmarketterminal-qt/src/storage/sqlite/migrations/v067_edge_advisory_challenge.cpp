#include "storage/sqlite/migrations/MigrationRunner.h"

namespace openmarketterminal {
namespace {

Result<void> exec(QSqlDatabase& db, const QString& sql) {
    QSqlQuery q(db);
    if (!q.exec(sql))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}

Result<void> apply_v067(QSqlDatabase& db) {
    auto r = exec(db,
        "CREATE TABLE IF NOT EXISTS edge_advisory_challenge ("
        " challenge_id TEXT PRIMARY KEY,"
        " state TEXT NOT NULL DEFAULT 'OPEN',"
        " created_at INTEGER NOT NULL,"
        " prediction_ttl_at INTEGER NOT NULL,"
        " execution_relevance_at INTEGER NOT NULL,"
        " ticker TEXT NOT NULL DEFAULT '',"
        " market_id TEXT NOT NULL DEFAULT '',"
        " horizon TEXT NOT NULL DEFAULT '',"
        " settlement_def TEXT NOT NULL DEFAULT '',"
        " context_json TEXT NOT NULL DEFAULT '{}',"
        " context_hash TEXT NOT NULL DEFAULT '',"
        " sealed_hash TEXT NOT NULL DEFAULT '',"
        " nonce TEXT NOT NULL DEFAULT '',"
        " market_at_open_json TEXT NOT NULL DEFAULT '{}',"
        " market_at_blind_json TEXT NOT NULL DEFAULT '{}',"
        " market_at_reveal_json TEXT NOT NULL DEFAULT '{}',"
        " market_at_post_json TEXT NOT NULL DEFAULT '{}',"
        " daemon_prob_at_open REAL NOT NULL DEFAULT -1,"
        " daemon_prob_at_reveal REAL NOT NULL DEFAULT -1,"
        " provider TEXT NOT NULL DEFAULT '', model TEXT NOT NULL DEFAULT '',"
        " prompt_version TEXT NOT NULL DEFAULT '', context_schema_version INTEGER NOT NULL DEFAULT 1,"
        " protocol_version INTEGER NOT NULL DEFAULT 1, agent_id TEXT NOT NULL DEFAULT '',"
        " run_id TEXT NOT NULL DEFAULT '', temperature REAL NOT NULL DEFAULT -1,"
        " p_pre REAL NOT NULL DEFAULT -1, p_post REAL NOT NULL DEFAULT -1,"
        " confidence_pre REAL NOT NULL DEFAULT -1, confidence_post REAL NOT NULL DEFAULT -1,"
        " rationale_pre TEXT NOT NULL DEFAULT '', rationale_post TEXT NOT NULL DEFAULT '',"
        " commit_id_blind TEXT NOT NULL DEFAULT '', commit_id_post TEXT NOT NULL DEFAULT '',"
        " journal_id TEXT NOT NULL DEFAULT '',"
        " ts_blind INTEGER NOT NULL DEFAULT 0, ts_reveal INTEGER NOT NULL DEFAULT 0,"
        " ts_post INTEGER NOT NULL DEFAULT 0)");
    if (r.is_err()) return r;
    r = exec(db, "CREATE INDEX IF NOT EXISTS idx_advisory_challenge_state "
                 "ON edge_advisory_challenge(state, created_at)");
    if (r.is_err()) return r;
    return exec(db, "CREATE INDEX IF NOT EXISTS idx_advisory_challenge_forecaster "
                    "ON edge_advisory_challenge(provider, model, created_at)");
}

} // namespace

void register_migration_v067() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    MigrationRunner::register_migration({67, "edge_advisory_challenge", apply_v067});
}

} // namespace openmarketterminal
