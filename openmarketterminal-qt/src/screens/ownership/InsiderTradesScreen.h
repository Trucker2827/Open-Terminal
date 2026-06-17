// src/screens/ownership/InsiderTradesScreen.h
// Read-only view of corporate insider trades (SEC Form 4) for a ticker.
// Data via GovDataService -> scripts/mcp/edgar/main.py (insider_transactions),
// which uses edgartools and returns a verified row contract (see Slice 0.5).
// Display only — never trades.
#pragma once

#include "services/gov_data/GovDataService.h"

#include <QWidget>

class QLineEdit;
class QPushButton;
class QLabel;
class QStackedWidget;

namespace openmarketterminal::ui { class DataTable; }

namespace openmarketterminal::screens {

class InsiderTradesScreen : public QWidget {
    Q_OBJECT
  public:
    explicit InsiderTradesScreen(QWidget* parent = nullptr);

  private slots:
    void on_result(const QString& request_id, const services::GovDataResult& result);
    void load();

  private:
    void build_ui();
    void show_message(const QString& msg);
    void populate(const QJsonObject& data);

    QLineEdit* ticker_edit_ = nullptr;
    QPushButton* load_btn_ = nullptr;
    QLabel* status_ = nullptr;
    QStackedWidget* stack_ = nullptr;   // 0 = message, 1 = table
    ui::DataTable* table_ = nullptr;
    QString request_id_ = "ownership_insider";
};

} // namespace openmarketterminal::screens
