#include "trading/ai_activity/AiActivityFormat.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>

namespace openmarketterminal::trading {
namespace {
const QSet<QString>& toast_decisions() {
    static const QSet<QString> s = {"filled","partially_filled","accepted","submitted","new",
                                    "open","cancelled","canceled","rejected","denied"};
    return s;
}
ActivityView::Severity severity_for(const QString& d) {
    using S = ActivityView::Severity;
    if (d=="filled"||d=="partially_filled"||d=="cancelled"||d=="canceled") return S::Success;
    if (d=="rejected"||d=="denied") return S::Error;
    return S::Info;
}
QString action_summary(const QString& intent_json) {
    const QJsonDocument doc = QJsonDocument::fromJson(intent_json.toUtf8());
    if (!doc.isObject()) return {};
    const QJsonObject o = doc.object();
    const QString side = o.value("side").toString().toUpper();
    const QString sym  = o.value("symbol").toString().toUpper();
    const double qty   = o.value("quantity").toVariant().toDouble();
    QStringList parts;
    if (!side.isEmpty()) parts << side;
    if (qty > 0) parts << QString::number(qty, 'g', 10);
    if (!sym.isEmpty()) parts << sym;
    return parts.join(' ');
}
} // namespace

ActivityView format_activity(const TradeAuditRow& row) {
    ActivityView v;
    const QString d = row.decision.trimmed().toLower();
    v.toast = (row.phase.compare("prepare", Qt::CaseInsensitive) != 0) && toast_decisions().contains(d);
    v.severity = severity_for(d);
    v.time = row.ts;
    v.tool = row.tool;
    v.account_mode = row.mode.isEmpty() ? row.account : (row.account + " · " + row.mode);
    v.action = action_summary(row.intent_json);
    v.decision = row.decision;
    v.reason = row.reason;
    const QString act = v.action.isEmpty() ? row.tool : (row.tool + " · " + v.action);
    v.message = QStringLiteral("AI · %1 → %2%3")
                    .arg(act, row.decision,
                         row.mode.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(row.mode));
    return v;
}
} // namespace openmarketterminal::trading
