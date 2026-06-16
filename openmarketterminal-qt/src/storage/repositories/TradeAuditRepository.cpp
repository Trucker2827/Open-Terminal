#include "storage/repositories/TradeAuditRepository.h"
#include "core/events/EventBus.h"
#include "core/logging/Logger.h"
#include <QVariantMap>

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

QVariantMap audit_row_to_map(const TradeAuditRow& row) {
    return {{"ts", row.ts}, {"phase", row.phase}, {"tool", row.tool}, {"account", row.account},
            {"mode", row.mode}, {"intent_json", row.intent_json}, {"decision", row.decision},
            {"reason", row.reason}, {"risk_snapshot_json", row.risk_snapshot_json}};
}

TradeAuditRow audit_row_from_map(const QVariantMap& m) {
    TradeAuditRow r;
    r.ts = m.value("ts").toString(); r.phase = m.value("phase").toString();
    r.tool = m.value("tool").toString(); r.account = m.value("account").toString();
    r.mode = m.value("mode").toString(); r.intent_json = m.value("intent_json").toString();
    r.decision = m.value("decision").toString(); r.reason = m.value("reason").toString();
    r.risk_snapshot_json = m.value("risk_snapshot_json").toString();
    return r;
}

Result<void> TradeAuditRepository::append(const TradeAuditRow& row) {
    auto r = exec_write("INSERT INTO trade_audit "
                        "(ts, phase, tool, account, mode, intent_json, decision, reason, risk_snapshot_json) "
                        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                        {row.ts, row.phase, row.tool, row.account, row.mode, row.intent_json,
                         row.decision, row.reason, row.risk_snapshot_json});
    if (r.is_ok()) {
        // Fire-and-forget: the audit row is already committed. A publish failure
        // or a throwing subscriber must NEVER surface here or change the result.
        try {
            EventBus::instance().publish(QStringLiteral("trade.audit"), audit_row_to_map(row));
        } catch (...) {
            LOG_WARN("TradeAudit", "trade.audit publish threw — ignored (audit write already committed)");
        }
    }
    return r;
}

Result<QVector<TradeAuditRow>> TradeAuditRepository::recent(int limit) {
    return query_list(QString("SELECT %1 FROM trade_audit ORDER BY id DESC LIMIT ?").arg(kAuditColumns), {limit},
                      map_row);
}

} // namespace openmarketterminal
