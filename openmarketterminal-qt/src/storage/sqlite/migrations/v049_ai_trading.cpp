// v049_ai_trading — storage for the AI-trading two-phase order flow (Phase A).
//
//   order_drafts — a prepared-but-not-submitted order intent plus its risk
//                  verdict, keyed by an opaque draft_id. status walks
//                  prepared → submitted/cancelled/expired. Read/written by
//                  OrderDraftRepository; backs the prepare/submit tool pair.
//   trade_audit  — append-only decision log: one row per prepare/submit phase
//                  with the intent, the allow/deny decision + reason, and a risk
//                  snapshot. Written by TradeAuditRepository (insert only).
//
// CREATE TABLE IF NOT EXISTS makes re-run idempotent; any exec failure is a real
// error (no duplicate-column swallowing — this is a CREATE migration).

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace openmarketterminal {
namespace {

Result<void> apply_v049(QSqlDatabase& db) {
    QSqlQuery drafts(db);
    const char* drafts_sql =
        "CREATE TABLE IF NOT EXISTS order_drafts ("
        " draft_id TEXT PRIMARY KEY,"
        " intent_json TEXT NOT NULL,"
        " risk_verdict_json TEXT NOT NULL DEFAULT '',"
        " account TEXT NOT NULL DEFAULT '',"
        " mode_hint TEXT NOT NULL DEFAULT '',"
        " status TEXT NOT NULL DEFAULT 'prepared',"
        " created_at TEXT NOT NULL DEFAULT '',"
        " expires_at TEXT NOT NULL DEFAULT ''"
        ")";
    if (!drafts.exec(QString::fromUtf8(drafts_sql)))
        return Result<void>::err(drafts.lastError().text().toStdString());

    QSqlQuery audit(db);
    const char* audit_sql =
        "CREATE TABLE IF NOT EXISTS trade_audit ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " ts TEXT NOT NULL DEFAULT '',"
        " phase TEXT NOT NULL DEFAULT '',"
        " tool TEXT NOT NULL DEFAULT '',"
        " account TEXT NOT NULL DEFAULT '',"
        " mode TEXT NOT NULL DEFAULT '',"
        " intent_json TEXT NOT NULL DEFAULT '',"
        " decision TEXT NOT NULL DEFAULT '',"
        " reason TEXT NOT NULL DEFAULT '',"
        " risk_snapshot_json TEXT NOT NULL DEFAULT ''"
        ")";
    if (!audit.exec(QString::fromUtf8(audit_sql)))
        return Result<void>::err(audit.lastError().text().toStdString());

    return Result<void>::ok();
}

} // namespace

void register_migration_v049() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({49, "ai_trading", apply_v049});
}

} // namespace openmarketterminal
