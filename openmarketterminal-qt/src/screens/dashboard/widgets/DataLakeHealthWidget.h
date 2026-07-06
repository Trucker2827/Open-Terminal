#pragma once
// DataLakeHealthWidget - local DuckDB/data-lake visibility for the dashboard.
//
// Shows profile-local lake status only. It does not read API keys, call brokers,
// or run DuckDB queries from the GUI.

#include "screens/dashboard/widgets/BaseWidget.h"

class QLabel;
class QTableWidget;
class QTimer;

namespace openmarketterminal::screens::widgets {

class DataLakeHealthWidget : public BaseWidget {
    Q_OBJECT
  public:
    explicit DataLakeHealthWidget(const QJsonObject& cfg = {}, QWidget* parent = nullptr);

    QJsonObject config() const override;
    void apply_config(const QJsonObject& cfg) override;

  protected:
    void on_theme_changed() override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void retranslateUi() override;
    QDialog* make_config_dialog(QWidget* parent) override;

  private:
    void refresh_data();
    void apply_styles();
    static QString fmt_bytes(qint64 bytes);
    static QString fmt_age(qint64 last_modified_ms);

    int refresh_sec_ = 15;
    QLabel* status_label_ = nullptr;
    QLabel* path_label_ = nullptr;
    QTableWidget* table_ = nullptr;
    QTimer* timer_ = nullptr;
};

} // namespace openmarketterminal::screens::widgets
