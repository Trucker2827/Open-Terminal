#pragma once
#include "storage/repositories/BaseRepository.h"
#include <QVariantMap>

namespace openmarketterminal {

/// One append-only decision-log entry for the AI-trading order flow (v049): the
/// intent, the allow/deny decision + reason, and a risk snapshot, per phase.
/// The DB-managed autoincrement id is not part of this struct.
struct TradeAuditRow {
    QString ts;
    QString phase;
    QString tool;
    QString account;
    QString mode;
    QString intent_json;
    QString decision;
    QString reason;
    QString risk_snapshot_json;
};

/// Data-access layer for the append-only trade_audit table.
class TradeAuditRepository : public BaseRepository<TradeAuditRow> {
  public:
    static TradeAuditRepository& instance();

    /// Append one audit row (insert only — the id autoincrements).
    Result<void> append(const TradeAuditRow& row);

    /// Most-recent rows first, capped at `limit`.
    Result<QVector<TradeAuditRow>> recent(int limit);

  private:
    TradeAuditRepository() = default;
    static TradeAuditRow map_row(QSqlQuery& q);
};

/// Lossless map↔row helpers for serialising TradeAuditRow to/from a QVariantMap
/// (used by the EventBus "trade.audit" payload and any other in-process consumers).
QVariantMap audit_row_to_map(const TradeAuditRow& row);
TradeAuditRow audit_row_from_map(const QVariantMap& m);

} // namespace openmarketterminal
