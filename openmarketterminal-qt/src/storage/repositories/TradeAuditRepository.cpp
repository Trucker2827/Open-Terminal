#include "storage/repositories/TradeAuditRepository.h"

namespace openmarketterminal {

TradeAuditRepository& TradeAuditRepository::instance() {
    static TradeAuditRepository s;
    return s;
}

static const char* kAuditColumns =
    "ts, phase, tool, account, mode, intent_json, decision, reason, risk_snapshot_json";

TradeAuditRow TradeAuditRepository::map_row(QSqlQuery& q) {
    TradeAuditRow r;
    r.ts = q.value(0).toString();
    r.phase = q.value(1).toString();
    r.tool = q.value(2).toString();
    r.account = q.value(3).toString();
    r.mode = q.value(4).toString();
    r.intent_json = q.value(5).toString();
    r.decision = q.value(6).toString();
    r.reason = q.value(7).toString();
    r.risk_snapshot_json = q.value(8).toString();
    return r;
}

Result<void> TradeAuditRepository::append(const TradeAuditRow& row) {
    return exec_write("INSERT INTO trade_audit "
                      "(ts, phase, tool, account, mode, intent_json, decision, reason, risk_snapshot_json) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                      {row.ts, row.phase, row.tool, row.account, row.mode, row.intent_json, row.decision,
                       row.reason, row.risk_snapshot_json});
}

Result<QVector<TradeAuditRow>> TradeAuditRepository::recent(int limit) {
    return query_list(QString("SELECT %1 FROM trade_audit ORDER BY id DESC LIMIT ?").arg(kAuditColumns), {limit},
                      map_row);
}

} // namespace openmarketterminal
