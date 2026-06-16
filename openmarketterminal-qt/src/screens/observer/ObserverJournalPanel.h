#pragma once

// Read-only GUI panel showing the headless observer's journal (daily lesson / weekly
// review / alerts), rendered from Markdown. Thin shell over the pure markdown_for()
// selector + ObserverJournalService — displays only, never trades.

#include <QWidget>
#include "screens/observer/ObserverJournalView.h"   // JournalView

class QTextEdit;
class QTimer;

namespace openmarketterminal::screens {

class ObserverJournalPanel : public QWidget {
    Q_OBJECT
  public:
    explicit ObserverJournalPanel(QWidget* parent = nullptr);

  private:
    void build_ui();
    void set_view(observer::JournalView v);
    void refresh();

    observer::JournalView view_ = observer::JournalView::Latest;
    QTextEdit* display_ = nullptr;
    QTimer* timer_ = nullptr;
};

} // namespace openmarketterminal::screens
