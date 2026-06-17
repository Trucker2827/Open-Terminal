// src/screens/ownership/InstitutionHoldingsScreen.h
// Read-only view of an institution's latest 13F holdings (SEC Form 13F-HR).
// Data via GovDataService -> scripts/mcp/edgar/main.py (13f_top_holdings), the
// verified Slice 0.5 parser. Display only — never trades.
#pragma once

#include "services/gov_data/GovDataService.h"

#include <QWidget>

class QLineEdit;
class QPushButton;
class QLabel;
class QStackedWidget;

namespace openmarketterminal::ui { class DataTable; }

namespace openmarketterminal::screens {

class InstitutionHoldingsScreen : public QWidget {
    Q_OBJECT
  public:
    explicit InstitutionHoldingsScreen(QWidget* parent = nullptr);

  private slots:
    void on_result(const QString& request_id, const services::GovDataResult& result);
    void load();

  private:
    void build_ui();
    void show_message(const QString& msg);
    void populate(const QJsonObject& data);

    QLineEdit* ticker_edit_ = nullptr;
    QPushButton* load_btn_ = nullptr;
    QLabel* header_ = nullptr;   // manager + total value summary
    QLabel* status_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    ui::DataTable* table_ = nullptr;
    QString request_id_ = "ownership_institution";
};

} // namespace openmarketterminal::screens
