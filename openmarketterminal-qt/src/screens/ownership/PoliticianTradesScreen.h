// src/screens/ownership/PoliticianTradesScreen.h
// Read-only view of US Congress (politician) trades for a ticker.
// Data via GovDataService -> scripts/ainvest_data.py (congress), AInvest API.
// Requires AINVEST_API_KEY in Settings -> Credentials; shows a "not configured"
// state until set. Display only — never trades.
#pragma once

#include "services/gov_data/GovDataService.h"

#include <QWidget>

class QLineEdit;
class QPushButton;
class QLabel;
class QStackedWidget;

namespace openmarketterminal::ui { class DataTable; }

namespace openmarketterminal::screens {

class PoliticianTradesScreen : public QWidget {
    Q_OBJECT
  public:
    explicit PoliticianTradesScreen(QWidget* parent = nullptr);

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
    QStackedWidget* stack_ = nullptr;
    ui::DataTable* table_ = nullptr;
    QString request_id_ = "ownership_politician";
};

} // namespace openmarketterminal::screens
