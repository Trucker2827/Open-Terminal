#pragma once
#include "trading/ai_activity/AiActivityFormat.h"
#include <QWidget>
class QTableWidget;
namespace openmarketterminal::screens::settings {

/// Settings → "AI Activity" tab: scrollable log of every AI trading action.
/// Loads TradeAuditRepository::recent(200) on construction; prepends live rows
/// via add_activity() (connected to AiActivityNotifier::activity at construction).
class AiActivitySection : public QWidget {
    Q_OBJECT
  public:
    explicit AiActivitySection(QWidget* parent = nullptr);
  public slots:
    void add_activity(const openmarketterminal::trading::ActivityView& view);  // prepend + flash
  private:
    void load_recent();
    void insert_row(int at, const openmarketterminal::trading::ActivityView& view);
    QTableWidget* table_ = nullptr;
    static constexpr int kCap = 200;
};

} // namespace openmarketterminal::screens::settings
