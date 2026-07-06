#pragma once

#include <QWidget>

class QLabel;
class QEvent;
class QTableWidget;

namespace openmarketterminal::screens {

class ToolsHealthScreen : public QWidget {
    Q_OBJECT

  public:
    explicit ToolsHealthScreen(QWidget* parent = nullptr);

  protected:
    void changeEvent(QEvent* event) override;

  private:
    void setup_ui();
    void populate();
    void retranslateUi();
    QLabel* make_stat(const QString& label, const QString& value, const QString& color);

    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QLabel* working_stat_ = nullptr;
    QLabel* cli_stat_ = nullptr;
    QLabel* mcp_stat_ = nullptr;
    QLabel* gaps_stat_ = nullptr;
    QTableWidget* table_ = nullptr;
};

} // namespace openmarketterminal::screens
