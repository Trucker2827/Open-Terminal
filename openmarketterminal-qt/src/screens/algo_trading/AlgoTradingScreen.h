// src/screens/algo_trading/AlgoTradingScreen.h
#pragma once
#include "screens/common/IStatefulScreen.h"

#include <QEvent>
#include <QHideEvent>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTimer>
#include <QWidget>

namespace openmarketterminal::screens {

class SandboxBooksPanel;
class StrategyOpsMapPanel;
class StrategyHandlersPanel;
class StrategyRiskPanel;
class StrategyRunHistoryPanel;
namespace widgets {
class DineroNetworkWidget;
}

/// Evidence-first strategy workspace. Every visible surface is backed by the
/// sandbox registry, paper ledger, scorer, or the daemon jobs that feed them.
class AlgoTradingScreen : public QWidget, public IStatefulScreen {
    Q_OBJECT
  public:
    explicit AlgoTradingScreen(QWidget* parent = nullptr);

    void restore_state(const QVariantMap& state) override;
    QVariantMap save_state() const override;
    QString state_key() const override { return "algo_trading"; }
    int state_version() const override { return 3; }

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;

  private slots:
    void on_tab_changed(int index);
    void on_cockpit_drilldown(int view, const QString& book_kind);

  private:
    void build_ui();
    QWidget* build_top_bar();
    QWidget* build_status_bar();
    void update_tab_buttons();
    void retranslateUi();

    QStackedWidget* content_stack_ = nullptr;
    StrategyOpsMapPanel* ops_map_ = nullptr;
    StrategyHandlersPanel* handlers_ = nullptr;
    SandboxBooksPanel* proof_books_ = nullptr;
    StrategyRiskPanel* risk_ = nullptr;
    StrategyRunHistoryPanel* run_history_ = nullptr;
    widgets::DineroNetworkWidget* dinero_widget_ = nullptr;

    QVector<QPushButton*> tab_buttons_;
    int active_tab_ = 0;
    QLabel* title_label_ = nullptr;
    QLabel* engine_caption_ = nullptr;
    QLabel* mode_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QTimer* poll_timer_ = nullptr;
    bool first_show_ = true;
};

} // namespace openmarketterminal::screens
