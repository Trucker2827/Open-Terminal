// src/screens/algo_trading/AlgoTradingScreen.cpp
#include "screens/algo_trading/AlgoTradingScreen.h"

#include "algo_engine/AlgoEngine.h"
#include "core/logging/Logger.h"
#include "core/session/ScreenStateManager.h"
#include "screens/algo_trading/AlertsPanel.h"
#include "screens/algo_trading/DeploymentDashboard.h"
#include "screens/algo_trading/ScannerPanel.h"
#include "screens/algo_trading/SandboxBooksPanel.h"
#include "screens/algo_trading/StrategyBuilderPanel.h"
#include "screens/algo_trading/StrategyListPanel.h"
#include "screens/algo_trading/StrategyOpsMapPanel.h"
#include "screens/algo_trading/UniverseScannerPanel.h"
#include "services/algo_trading/AlgoTradingService.h"
#include "ui/theme/Theme.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QVBoxLayout>

namespace openmarketterminal::screens {

using namespace openmarketterminal::services::algo;

AlgoTradingScreen::AlgoTradingScreen(QWidget* parent) : QWidget(parent) {
    build_ui();

    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(5000);
    connect(poll_timer_, &QTimer::timeout, this, [this]() {
        if (active_tab_ == 2)
            openmarketterminal::algo::AlgoEngine::instance().list_deployments();
    });

    connect(&openmarketterminal::algo::AlgoEngine::instance(), &openmarketterminal::algo::AlgoEngine::deployments_loaded, this,
            [this](const QVector<AlgoDeployment>& deps) {
                int active = 0;
                for (const auto& d : deps) {
                    if (d.status == "running" || d.status == "starting")
                        ++active;
                }
                active_deployments_ = active;
                deploy_count_label_->setText(tr("%1 LIVE").arg(active));
            });

    LOG_INFO("AlgoTrading", "Screen constructed");
}

void AlgoTradingScreen::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    poll_timer_->start();
    if (first_show_) {
        first_show_ = false;
        on_tab_changed(0);
    }
}

void AlgoTradingScreen::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    poll_timer_->stop();
}

void AlgoTradingScreen::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(build_top_bar());

    content_stack_ = new QStackedWidget(this);
    ops_map_ = new StrategyOpsMapPanel(this);
    proof_books_ = new SandboxBooksPanel(this);
    dashboard_ = new DeploymentDashboard(this);
    strategies_ = new StrategyListPanel(this);
    builder_ = new StrategyBuilderPanel(this);
    scanner_ = new ScannerPanel(this);
    alerts_ = new AlertsPanel(this);
    universe_ = new UniverseScannerPanel(this);

    content_stack_->addWidget(ops_map_);     // 0
    content_stack_->addWidget(proof_books_); // 1
    content_stack_->addWidget(dashboard_);   // 2
    content_stack_->addWidget(strategies_);  // 3
    content_stack_->addWidget(builder_);     // 4
    content_stack_->addWidget(scanner_);     // 5
    content_stack_->addWidget(alerts_);      // 6
    content_stack_->addWidget(universe_);    // 7
    root->addWidget(content_stack_, 1);

    // "Edit" in Classic Rules opens the Builder pre-filled with that strategy.
    connect(strategies_, &StrategyListPanel::edit_requested, this,
            [this](const AlgoStrategy& s) {
                builder_->load_strategy(s);
                on_tab_changed(4);
            });

    // "Backtest" opens the Builder pre-filled and runs the backtest immediately.
    connect(strategies_, &StrategyListPanel::backtest_requested, this,
            [this](const AlgoStrategy& s, const QString& symbol, const QString& start, const QString& end) {
                on_tab_changed(4);
                builder_->load_and_backtest(s, symbol, start, end);
            });

    // Scanner → Alerts hand-off: pre-fills the AlertsPanel with the scan's conditions
    // and switches to the ALERTS tab. ScannerPanel::create_alert_requested is added in R3.
    connect(scanner_, &ScannerPanel::create_alert_requested, this,
            [this](const QJsonArray& conds, const QString& logic, const QStringList& syms,
                   const QString& tf, const QString& ds, const QString& acct) {
                on_tab_changed(6); // ALERTS
                alerts_->prefill(conds, logic, syms, tf, ds, acct);
            });

    // Deploying from the Builder jumps to Deployments; on_tab_changed(1)
    // refreshes list_deployments() so the just-persisted row shows immediately.
    connect(builder_, &StrategyBuilderPanel::deployed, this, [this]() { on_tab_changed(2); });

    root->addWidget(build_status_bar());
    setStyleSheet(QString("background:%1;").arg(ui::colors::BG_BASE()));
}

QWidget* AlgoTradingScreen::build_top_bar() {
    auto* bar = new QWidget(this);
    bar->setFixedHeight(40);
    bar->setStyleSheet(
        QString("background:%1; border-bottom:1px solid %2;").arg(ui::colors::BG_BASE(), ui::colors::BORDER_DIM()));

    auto* hl = new QHBoxLayout(bar);
    hl->setContentsMargins(12, 0, 12, 0);
    hl->setSpacing(8);

    // Title + subtitle matching Economics header style
    title_label_ = new QLabel(tr("STRATEGIES"), bar);
    title_label_->setStyleSheet(QString("color:%1; font-size:12px; font-weight:700;"
                                         "letter-spacing:1.5px; background:transparent;")
                                    .arg(ui::colors::TEXT_PRIMARY()));
    hl->addWidget(title_label_);

    auto* div = new QWidget(bar);
    div->setFixedSize(1, 20);
    div->setStyleSheet(QString("background:%1;").arg(ui::colors::BORDER_DIM()));
    hl->addWidget(div);

    // Tab buttons
    QStringList tabs   = {tr("OPS MAP"), tr("PROOF BOOKS"), tr("DEPLOYMENTS"), tr("CLASSIC RULES"), tr("BUILDER"),
                          tr("SCANNER"), tr("ALERTS"), tr("UNIVERSE")};
    QStringList colors = {"#D97706", "#14B8A6", "#00D66F", "#00E5FF", "#FF6B35", "#FFC400", "#FF4081", "#A78BFA"};

    for (int i = 0; i < tabs.size(); ++i) {
        auto* btn = new QPushButton(tabs[i], bar);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(QString("QPushButton { color:%1; font-size:10px; font-family:%2;"
                                   "padding:4px 12px; border:none;"
                                   "background:transparent; font-weight:400; }"
                                   "QPushButton:hover { color:%3; }")
                               .arg(ui::colors::TEXT_TERTIARY())
                               .arg(ui::fonts::DATA_FAMILY())
                               .arg(colors[i]));
        connect(btn, &QPushButton::clicked, this, [this, i]() { on_tab_changed(i); });
        hl->addWidget(btn);
        tab_buttons_.append(btn);
    }

    hl->addStretch(1);

    // Deployment count badge
    deploy_count_label_ = new QLabel(tr("%1 LIVE").arg(0), bar);
    deploy_count_label_->setStyleSheet(QString("color:%1; font-size:9px; font-weight:700; font-family:%2;"
                                               "padding:3px 8px; background:rgba(22,163,74,0.08);"
                                               "border:1px solid rgba(22,163,74,0.25); border-radius:2px;")
                                           .arg(ui::colors::POSITIVE())
                                           .arg(ui::fonts::DATA_FAMILY()));
    hl->addWidget(deploy_count_label_);

    return bar;
}

QWidget* AlgoTradingScreen::build_status_bar() {
    auto* bar = new QWidget(this);
    bar->setFixedHeight(24);
    bar->setStyleSheet(
        QString("background:%1; border-top:1px solid %2;").arg(ui::colors::BG_RAISED(), ui::colors::BORDER_DIM()));
    auto* hl = new QHBoxLayout(bar);
    hl->setContentsMargins(12, 0, 12, 0);
    hl->setSpacing(16);
    auto s =
        QString("color:%1; font-size:8px; font-family:%2;").arg(ui::colors::TEXT_TERTIARY()).arg(ui::fonts::DATA_FAMILY);
    engine_caption_ = new QLabel(tr("ENGINE:"), bar);
    engine_caption_->setStyleSheet(s);
    auto* v1 = new QLabel("PROOF v2.0", bar);
    v1->setStyleSheet(QString("color:%1; font-size:8px; font-weight:700; font-family:%2;")
                          .arg(ui::colors::TEXT_PRIMARY())
                          .arg(ui::fonts::DATA_FAMILY));
    hl->addWidget(engine_caption_);
    hl->addWidget(v1);
    hl->addStretch();
    status_label_ = new QLabel(tr("IDLE"), bar);
    status_label_->setStyleSheet(QString("color:%1; font-size:8px; font-weight:700; font-family:%2;")
                                     .arg(ui::colors::POSITIVE())
                                     .arg(ui::fonts::DATA_FAMILY));
    hl->addWidget(status_label_);
    return bar;
}

void AlgoTradingScreen::on_tab_changed(int index) {
    if (index < 0 || index >= tab_buttons_.size())
        return;
    active_tab_ = index;
    content_stack_->setCurrentIndex(index);
    update_tab_buttons();
    ScreenStateManager::instance().notify_changed(this);

    // Refresh data when switching tabs
    if (index == 0 && ops_map_)
        ops_map_->refresh();
    if (index == 1 && proof_books_)
        proof_books_->refresh();
    if (index == 2)
        openmarketterminal::algo::AlgoEngine::instance().list_deployments();
    if (index == 3)
        AlgoTradingService::instance().list_strategies();
}

void AlgoTradingScreen::update_tab_buttons() {
    QStringList colors = {"#D97706", "#14B8A6", "#00D66F", "#00E5FF", "#FF6B35", "#FFC400", "#FF4081", "#A78BFA"};
    for (int i = 0; i < tab_buttons_.size(); ++i) {
        bool active = (i == active_tab_);
        tab_buttons_[i]->setStyleSheet(
            active
                ? QString("QPushButton { color:%1; font-size:10px; font-family:%2;"
                          " padding:4px 12px; border:none; border-bottom:2px solid %1;"
                          " background:transparent; font-weight:700; }")
                      .arg(colors[i])
                      .arg(ui::fonts::DATA_FAMILY())
                : QString("QPushButton { color:%1; font-size:10px; font-family:%2;"
                          " padding:4px 12px; border:none;"
                          " background:transparent; font-weight:400; }"
                          "QPushButton:hover { color:%3; }")
                      .arg(ui::colors::TEXT_TERTIARY())
                      .arg(ui::fonts::DATA_FAMILY())
                      .arg(colors[i]));
    }
}

void AlgoTradingScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void AlgoTradingScreen::retranslateUi() {
    if (title_label_)   title_label_->setText(tr("STRATEGIES"));
    if (engine_caption_) engine_caption_->setText(tr("ENGINE:"));
    if (status_label_)  status_label_->setText(tr("IDLE"));
    if (deploy_count_label_) deploy_count_label_->setText(tr("%1 LIVE").arg(active_deployments_));

    // Tab button labels — fixed order matches build_top_bar().
    if (tab_buttons_.size() == 8) {
        tab_buttons_[0]->setText(tr("OPS MAP"));
        tab_buttons_[1]->setText(tr("PROOF BOOKS"));
        tab_buttons_[2]->setText(tr("DEPLOYMENTS"));
        tab_buttons_[3]->setText(tr("CLASSIC RULES"));
        tab_buttons_[4]->setText(tr("BUILDER"));
        tab_buttons_[5]->setText(tr("SCANNER"));
        tab_buttons_[6]->setText(tr("ALERTS"));
        tab_buttons_[7]->setText(tr("UNIVERSE"));
    }
}

// ── IStatefulScreen ───────────────────────────────────────────────────────────

QVariantMap AlgoTradingScreen::save_state() const {
    QVariantMap state{{"tab_index", active_tab_}};
    if (builder_)
        state["builder_draft"] = builder_->save_draft();
    return state;
}

void AlgoTradingScreen::restore_state(const QVariantMap& state) {
    const int idx = state.value("tab_index", 0).toInt();
    if (idx >= 0 && idx < tab_buttons_.size())
        on_tab_changed(idx);
    if (builder_ && state.contains("builder_draft"))
        builder_->restore_draft(state.value("builder_draft").toMap());
}

} // namespace openmarketterminal::screens
