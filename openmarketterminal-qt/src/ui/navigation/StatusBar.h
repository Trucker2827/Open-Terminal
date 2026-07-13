#pragma once
#include <QHBoxLayout>
#include <QLabel>
#include <QWidget>

namespace openmarketterminal::ui {

/// Bottom status bar: version, feed indicators, and ready status.
/// Internationalised — READY/BUSY label retranslates on QEvent::LanguageChange.
class StatusBar : public QWidget {
    Q_OBJECT
  public:
    explicit StatusBar(QWidget* parent = nullptr);
    void set_ready(bool ready);

  protected:
    void changeEvent(QEvent* e) override;

  private:
    void refresh_theme();
    void retranslateUi();

    QLabel* ready_label_ = nullptr;
    /// Tracks the current ready/busy state so retranslateUi() can recompute
    /// the translated label without depending on the existing widget text.
    bool ready_state_ = true;
};

} // namespace openmarketterminal::ui
