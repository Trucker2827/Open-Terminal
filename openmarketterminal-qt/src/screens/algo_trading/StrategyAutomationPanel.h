#pragma once

#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <QWidget>

namespace openmarketterminal::screens {

class StrategyAutomationPanel : public QWidget {
    Q_OBJECT
  public:
    explicit StrategyAutomationPanel(QWidget* parent = nullptr);

  public slots:
    void refresh();

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    void build_ui();
    void run_selected();
    void set_selected_enabled(bool enabled);
    void clear_selected_failures();
    void run_cli(const QStringList& args, const QString& activity);
    QString cli_path() const;
    QString selected_job_id() const;
    void populate(const QJsonObject& document);
    void update_selection_detail();

    QTableWidget* jobs_table_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* detail_label_ = nullptr;
    QLabel* enabled_count_ = nullptr;
    QLabel* healthy_count_ = nullptr;
    QLabel* running_count_ = nullptr;
    QLabel* failed_count_ = nullptr;
    QPushButton* run_button_ = nullptr;
    QPushButton* toggle_button_ = nullptr;
    QPushButton* clear_button_ = nullptr;
    QTimer refresh_timer_;
    bool first_show_ = true;
};

} // namespace openmarketterminal::screens
