#pragma once
#include "trading/ai_activity/AiActivityFormat.h"
#include <QObject>
#include <QString>
#include <QVariantMap>

namespace openmarketterminal::app {

/// Lives on the main thread. Bridges the "trade.audit" EventBus event to the UI:
/// toasts terminal outcomes (any screen) and re-emits a typed view for the feed.
class AiActivityNotifier : public QObject {
    Q_OBJECT
  public:
    explicit AiActivityNotifier(QObject* parent = nullptr);
  signals:
    void activity(const openmarketterminal::trading::ActivityView& view);
  private:
    void on_event(const QString& event, const QVariantMap& data);
};

} // namespace openmarketterminal::app
