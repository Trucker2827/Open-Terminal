#include "storage/sqlite/migrations/MigrationRunner.h"

namespace openmarketterminal {
namespace {
// PaperExecutor::open_prediction_candidates filters edge_decision_journal on
// (source, gate, created_at), but no index covered that shape -- the table had
// only (symbol, horizon, created_at), (market_id, created_at) and
// (outcome, created_at). SQLite fell back to a full SCAN of 301,472 rows, plus
// a TEMP B-TREE for the ORDER BY, on EVERY call.
//
// The sandbox tick runs that query once per ACTIVE STRATEGY -- 31 of them on
// the live database -- so a single cycle paid for 31 full scans.
//
// Two independent measurements agreed:
//   * query plan: SCAN edge_decision_journal, 1,130ms per execution live,
//     i.e. ~35s of query time per cycle across 31 strategies
//   * `sample` of a live tick: 7,604 of 9,867 stacks (77%) sat in
//     open_prediction_candidates -> QSqlQuery::exec -> sqlite3_step, with
//     vdbeColumnFromOverflow prominent -- the scan was materialising the large
//     features_json/freshness_json blobs of every row it walked
//
// That is what put the tick's median at 26.4s and p90 at 38.5s against a 45s
// timeout, killing 25% of runs. With the index the plan becomes
// SEARCH ... USING INDEX (source=? AND gate=? AND created_at>?) and the temp
// B-tree disappears; the scan never touches the JSON columns at all.
//
// Column order matters: source and gate are equality predicates and must lead,
// with created_at last to serve the range and the ORDER BY from the same
// index.
Result<void> apply_v070(QSqlDatabase& db) {
    QSqlQuery q(db);
    if (!q.exec("CREATE INDEX IF NOT EXISTS idx_edge_decision_journal_source_gate "
                "ON edge_decision_journal(source, gate, created_at)"))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}
}
void register_migration_v070() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    MigrationRunner::register_migration({70, "edge_journal_source_gate_index", apply_v070});
}
} // namespace openmarketterminal
