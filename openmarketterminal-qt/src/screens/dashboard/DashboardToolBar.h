#pragma once
#include <QDateTime>
#include <QHideEvent>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <QWidget>

namespace openmarketterminal::screens {

/// Top toolbar for dashboard status and actions.
class DashboardToolBar : public QWidget {
    Q_OBJECT
  public:
    explicit DashboardToolBar(QWidget* parent = nullptr);

    void set_widget_count(int count);
    void set_connected(bool connected);

  signals:
    void add_widget_clicked();
    void save_layout_clicked();
    void reset_layout_clicked();
    void refresh_clicked();
    void toggle_pulse_clicked();
    void toggle_compact_clicked();

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void update_clock();
    void refresh_theme();
    void retranslateUi();

    QWidget* left_container_ = nullptr;
    QWidget* right_container_ = nullptr;
    QPushButton* clock_btn_ = nullptr;
    bool clock_is_utc_ = true;
    bool connected_ = false;             // true once DataHub delivers data
    qint64 last_data_ms_ = 0;            // epoch-ms of last DataHub topic_updated
    QLabel* status_text_ = nullptr;
    QPushButton* pulse_btn_ = nullptr;
    QPushButton* compact_btn_ = nullptr;
    QPushButton* refresh_btn_ = nullptr;
    QPushButton* add_btn_ = nullptr;
    QPushButton* save_btn_ = nullptr;
    QPushButton* reset_btn_ = nullptr;
    QTimer clock_timer_;
};

} // namespace openmarketterminal::screens
