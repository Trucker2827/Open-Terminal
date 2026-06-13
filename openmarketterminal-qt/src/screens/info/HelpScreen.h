#pragma once
#include <QWidget>

class QScrollArea;

namespace openmarketterminal::screens {

/// Simple OpenMarketTerminal about/help page.
///
/// Internationalised: all visible strings flow through tr(). The page contents
/// are built once in the constructor and rebuilt on QEvent::LanguageChange.
class HelpScreen : public QWidget {
    Q_OBJECT
  public:
    explicit HelpScreen(QWidget* parent = nullptr);

  signals:
    void navigate_back();
    void navigate_register();
    void navigate_forgot_password();

  protected:
    void changeEvent(QEvent* event) override;

  private:
    QWidget* build_page();
    QScrollArea* scroll_ = nullptr;
};

} // namespace openmarketterminal::screens
