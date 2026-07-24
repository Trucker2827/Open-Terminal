#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QWidget>

namespace openmarketterminal::screens {

class SandboxBooksPanel : public QWidget {
    Q_OBJECT
  public:
    explicit SandboxBooksPanel(QWidget* parent = nullptr);

  public slots:
    void refresh();
    void apply_cockpit_drilldown(int view, const QString& book_kind = {});

  signals:
    void returnToCockpit();

  protected:
    void showEvent(QShowEvent* event) override;

  private:
    void build_ui();
    void seed_books();
    void install_jobs();
    void run_tick();
    void run_score();
    void run_cli_command(const QStringList& command_args,
                         std::function<void(const QJsonObject&, const QString&)> on_success,
                         std::function<void(const QString&)> on_error);
    QString cli_path() const;
    void set_status(const QString& text, const QString& color = {});
    void populate_leaderboard();
    void populate_position_counts();
    void update_selected_detail();
    void apply_current_drilldown();

    QLabel* status_label_ = nullptr;
    QLabel* active_count_ = nullptr;
    QLabel* open_count_ = nullptr;
    QLabel* no_edge_count_ = nullptr;
    QLabel* resolved_count_ = nullptr;
    QLabel* net_pnl_ = nullptr;
    QLabel* eligible_count_ = nullptr;
    QTableWidget* leaderboard_table_ = nullptr;
    QLabel* detail_label_ = nullptr;
    QLabel* drilldown_badge_ = nullptr;
    QPushButton* back_button_ = nullptr;
    QPushButton* show_all_button_ = nullptr;
    int drilldown_view_ = 0;
    QString drilldown_book_kind_;
    bool first_show_ = true;
};

} // namespace openmarketterminal::screens
