// src/screens/portfolio/PortfolioCommandBar.h
#pragma once
#include "screens/portfolio/PortfolioTypes.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QWidget>

namespace openmarketterminal::screens {

class PortfolioCommandBar : public QWidget {
    Q_OBJECT
  public:
    explicit PortfolioCommandBar(QWidget* parent = nullptr);

    void set_portfolios(const QVector<portfolio::Portfolio>& portfolios);
    void set_selected_portfolio(const portfolio::Portfolio& p);
    /// Selects the virtual "All Accounts" aggregate (PortfolioService::
    /// kAllAccountsId) — not a real row in @ref portfolios_, so it can't go
    /// through set_selected_portfolio's by-id lookup.
    void set_selected_all_accounts();
    void set_refreshing(bool refreshing);
    void set_detail_view(std::optional<portfolio::DetailView> view);
    void set_has_selection(bool has_selection);
    void set_has_portfolios(bool has_portfolios);
    void set_refresh_interval(int ms);
    /// Toggles the Sync accounts button into a busy state while
    /// AccountSyncService::sync_all() is running.
    void set_syncing(bool syncing);
    /// "Synced Xm ago" / "Never synced" label text; empty hides the label
    /// (no synced accounts registered yet).
    void set_last_synced_text(const QString& text);
    /// Disables BUY/SELL/DIV when the active portfolio is a synced account
    /// mirror (or the All Accounts aggregate) — read-only guard. DIV is
    /// included because record_dividend() is a DB write too, and the feature
    /// is read-only end-to-end. View/analytics actions are unaffected.
    void set_trade_enabled(bool enabled);
    void refresh_theme();

  protected:
    void changeEvent(QEvent* event) override;

  signals:
    void portfolio_selected(QString id);
    void create_requested();
    void delete_requested(QString id);
    void buy_requested();
    void sell_requested();
    void dividend_requested();
    void refresh_requested();
    void export_csv_requested();
    void export_json_requested();
    void import_requested();
    void refresh_interval_changed(int ms);
    void ffn_toggled();
    void detail_view_selected(portfolio::DetailView view);
    void ai_analyze_requested();
    void agent_run_requested();
    void backtest_requested();
    void sync_accounts_requested();

  private:
    void build_ui();
    void build_row1(QHBoxLayout* layout);
    void build_row2(QHBoxLayout* layout);
    void build_portfolio_selector();
    void build_trade_cluster(QHBoxLayout* layout);
    void build_detail_tabs(QHBoxLayout* layout);
    void build_tools_cluster(QHBoxLayout* layout);
    void build_overflow_menu();
    void toggle_dropdown();
    void update_selector_label();
    void apply_row1_styles();
    void apply_row2_styles();
    void retranslateUi();

    // Portfolio selector
    QPushButton* selector_btn_ = nullptr;
    QWidget* dropdown_ = nullptr;
    QLineEdit* search_edit_ = nullptr;
    QListWidget* portfolio_list_ = nullptr;
    QPushButton* create_btn_ = nullptr;
    QPushButton* delete_btn_ = nullptr;

    // Account sync
    QPushButton* sync_btn_ = nullptr;
    QLabel* last_synced_label_ = nullptr;

    // Action buttons
    QPushButton* buy_btn_ = nullptr;
    QPushButton* sell_btn_ = nullptr;
    QPushButton* div_btn_ = nullptr;
    QPushButton* refresh_btn_ = nullptr;
    QComboBox* interval_cb_ = nullptr;
    QToolButton* overflow_btn_ = nullptr;
    QMenu* overflow_menu_ = nullptr;
    QAction* export_csv_action_ = nullptr;
    QAction* export_json_action_ = nullptr;
    QAction* import_action_ = nullptr;
    QAction* ffn_action_ = nullptr;
    QAction* backtest_action_ = nullptr;
    QPushButton* ai_btn_ = nullptr;
    QPushButton* agent_btn_ = nullptr;

    // Detail view buttons (neutral pill tabs)
    QVector<QPushButton*> detail_btns_;

    // Containers for show/hide
    QWidget* row1_ = nullptr;            // selector + refresh/interval/overflow
    QWidget* row2_ = nullptr;            // BUY/SELL/DIV | detail tabs | AI/AGENT
    QWidget* trade_cluster_ = nullptr;   // BUY/SELL/DIV
    QWidget* tabs_container_ = nullptr;  // 9 detail view pills
    QWidget* tools_cluster_ = nullptr;   // AI/AGENT

    // State
    QVector<portfolio::Portfolio> portfolios_;
    QString selected_id_;
    std::optional<portfolio::DetailView> active_detail_;
    bool dropdown_visible_ = false;
    bool has_portfolios_ = false; // tracked for retranslateUi() to pick the right selector text
};

} // namespace openmarketterminal::screens
