#pragma once
#include "storage/repositories/TradeAuditRepository.h"
#include <QMetaType>
#include <QString>

namespace openmarketterminal::trading {

struct ActivityView {
    bool toast = false;
    enum class Severity { Info, Success, Warning, Error };
    Severity severity = Severity::Info;
    QString message;       // "AI · fast_submit_order · BUY 1 AAPL → filled (live)"
    QString time, tool, account_mode, action, decision, reason;  // panel columns
};

/// Pure: decide toast/severity + format strings from a trade_audit row.
ActivityView format_activity(const TradeAuditRow& row);

} // namespace openmarketterminal::trading

// Required so a queued signal can carry ActivityView across threads (Task 3+).
Q_DECLARE_METATYPE(openmarketterminal::trading::ActivityView)
