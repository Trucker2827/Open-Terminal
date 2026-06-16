#include "app/AiActivityNotifier.h"

#include "core/events/EventBus.h"
#include "storage/repositories/TradeAuditRepository.h"
#include "ui/notifications/NotificationService.h"

#include <QMetaType>

namespace openmarketterminal::app {

using trading::ActivityView;

static ui::ToastService::Severity to_toast_sev(ActivityView::Severity s) {
    switch (s) {
        case ActivityView::Severity::Success: return ui::ToastService::Severity::Success;
        case ActivityView::Severity::Error:   return ui::ToastService::Severity::Error;
        case ActivityView::Severity::Warning: return ui::ToastService::Severity::Warning;
        case ActivityView::Severity::Info:    default: return ui::ToastService::Severity::Info;
    }
}

AiActivityNotifier& AiActivityNotifier::instance() {
    static AiActivityNotifier s_instance;
    return s_instance;
}

AiActivityNotifier::AiActivityNotifier(QObject* parent) : QObject(parent) {
    qRegisterMetaType<openmarketterminal::trading::ActivityView>(
        "openmarketterminal::trading::ActivityView");
    // Queued: append() fires on the bridge worker; this slot must run on the main thread.
    connect(&EventBus::instance(), &EventBus::eventPublished, this,
            &AiActivityNotifier::on_event, Qt::QueuedConnection);
}

void AiActivityNotifier::on_event(const QString& event, const QVariantMap& data) {
    if (event != QLatin1String("trade.audit")) return;
    const ActivityView v = trading::format_activity(audit_row_from_map(data));
    if (v.toast)
        ui::ToastService::instance().post(to_toast_sev(v.severity), v.message,
                                          QStringLiteral("ai-trading"));
    emit activity(v);
}

} // namespace openmarketterminal::app
