// src/screens/algo_trading/AlgoTradingScreen.cpp
#include "screens/algo_trading/AlgoTradingScreen.h"

#include "core/logging/Logger.h"
#include "core/events/EventBus.h"
#include "core/session/ScreenStateManager.h"
#include "screens/algo_trading/SandboxBooksPanel.h"
#include "screens/algo_trading/StrategyOpsMapPanel.h"
#include "screens/algo_trading/StrategyCockpitNavigation.h"
#include "screens/algo_trading/StrategyWorkspacePanels.h"
#include "screens/common/DineroNetworkGadget.h"
#include "ui/theme/Theme.h"

#include <QHBoxLayout>
#include <QGridLayout>
#include <QVBoxLayout>

namespace openmarketterminal::screens {

AlgoTradingScreen::AlgoTradingScreen(QWidget* parent) : QWidget(parent) {
    build_ui();

    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(5000);
    connect(poll_timer_, &QTimer::timeout, this, [this]() {
        if (active_tab_ == 1 && handlers_)
            handlers_->refresh();
        else if (active_tab_ == 3 && risk_)
            risk_->refresh();
        else if (active_tab_ == 4 && run_history_)
            run_history_->refresh();
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
    handlers_ = new StrategyHandlersPanel(this);
    proof_books_ = new SandboxBooksPanel(this);
    risk_ = new StrategyRiskPanel(this);
    run_history_ = new StrategyRunHistoryPanel(this);
    connect(ops_map_, &StrategyOpsMapPanel::drilldownRequested,
            this, &AlgoTradingScreen::on_cockpit_drilldown);
    connect(proof_books_, &SandboxBooksPanel::returnToCockpit,
            this, [this]() { on_tab_changed(0); });

    auto* cockpit = new QWidget(this);
    auto* cockpit_layout = new QGridLayout(cockpit);
    cockpit_layout->setContentsMargins(0, 0, 0, 0);
    cockpit_layout->setSpacing(0);
    cockpit_layout->addWidget(ops_map_, 0, 0);

    // Compact overlay: preserve the full strategy map instead of dedicating a
    // tall side column to the same telemetry.
    dinero_gadget_ = new DineroNetworkGadget(cockpit);
    dinero_gadget_->setObjectName(QStringLiteral("strategyCockpitDineroGadget"));
    dinero_gadget_->setFixedWidth(270);
    cockpit_layout->addWidget(dinero_gadget_, 0, 0, Qt::AlignRight | Qt::AlignBottom);
    dinero_gadget_->raise();

    content_stack_->addWidget(cockpit);      // 0
    content_stack_->addWidget(handlers_);    // 1
    content_stack_->addWidget(proof_books_); // 2
    content_stack_->addWidget(risk_);        // 3
    content_stack_->addWidget(run_history_); // 4
    root->addWidget(content_stack_, 1);

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

    title_label_ = new QLabel(tr("STRATEGIES"), bar);
    title_label_->setStyleSheet(QString("color:%1; font-size:12px; font-weight:700;"
                                        "letter-spacing:1.5px; background:transparent;")
                                    .arg(ui::colors::TEXT_PRIMARY()));
    hl->addWidget(title_label_);

    auto* div = new QWidget(bar);
    div->setFixedSize(1, 20);
    div->setStyleSheet(QString("background:%1;").arg(ui::colors::BORDER_DIM()));
    hl->addWidget(div);

    const QStringList tabs = {tr("COCKPIT"), tr("HANDLERS"), tr("EVIDENCE"), tr("RISK & SAFETY"), tr("RUN HISTORY")};
    const QStringList colors = {"#D97706", "#00E5FF", "#14B8A6", "#EF4444", "#A78BFA"};

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

    mode_label_ = new QLabel(tr("PAPER ONLY"), bar);
    mode_label_->setStyleSheet(QString("color:%1; font-size:9px; font-weight:700; font-family:%2;"
                                       "padding:3px 8px; background:rgba(217,119,6,0.08);"
                                       "border:1px solid rgba(217,119,6,0.35); border-radius:2px;")
                                   .arg(ui::colors::AMBER())
                                   .arg(ui::fonts::DATA_FAMILY()));
    mode_label_->setToolTip(tr("This workspace collects and scores evidence. It cannot place live orders."));
    hl->addWidget(mode_label_);

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
    auto s = QString("color:%1; font-size:8px; font-family:%2;")
                 .arg(ui::colors::TEXT_TERTIARY())
                 .arg(ui::fonts::DATA_FAMILY);
    engine_caption_ = new QLabel(tr("ENGINE:"), bar);
    engine_caption_->setStyleSheet(s);
    auto* v1 = new QLabel(tr("LOCAL PROOF ENGINE"), bar);
    v1->setStyleSheet(QString("color:%1; font-size:8px; font-weight:700; font-family:%2;")
                          .arg(ui::colors::TEXT_PRIMARY())
                          .arg(ui::fonts::DATA_FAMILY));
    hl->addWidget(engine_caption_);
    hl->addWidget(v1);
    hl->addStretch();
    status_label_ = new QLabel(tr("NO LIVE EXECUTION"), bar);
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
    if (index == 1 && handlers_)
        handlers_->refresh();
    if (index == 2 && proof_books_)
        proof_books_->refresh();
    if (index == 3 && risk_)
        risk_->refresh();
    if (index == 4 && run_history_)
        run_history_->refresh();
}

void AlgoTradingScreen::on_cockpit_drilldown(int raw_view, const QString& book_kind) {
    const auto view = static_cast<StrategyCockpitView>(raw_view);
    if (view == StrategyCockpitView::ResearchInputs) {
        EventBus::instance().publish(QStringLiteral("nav.switch_screen"),
                                     {{QStringLiteral("screen_id"), QStringLiteral("code_editor")}});
        return;
    }
    if (view == StrategyCockpitView::RiskSafety || view == StrategyCockpitView::DecisionEnvelopes) {
        on_tab_changed(3);
        if (view == StrategyCockpitView::DecisionEnvelopes)
            risk_->focus_decision_envelopes();
        return;
    }
    if (view == StrategyCockpitView::PaperHandlers) {
        on_tab_changed(1);
        return;
    }
    if (view == StrategyCockpitView::Outcomes) {
        on_tab_changed(4);
        run_history_->focus_outcomes();
        return;
    }
    proof_books_->apply_cockpit_drilldown(raw_view, book_kind);
    on_tab_changed(2);
}

void AlgoTradingScreen::update_tab_buttons() {
    const QStringList colors = {"#D97706", "#00E5FF", "#14B8A6", "#EF4444", "#A78BFA"};
    for (int i = 0; i < tab_buttons_.size(); ++i) {
        bool active = (i == active_tab_);
        tab_buttons_[i]->setStyleSheet(active ? QString("QPushButton { color:%1; font-size:10px; font-family:%2;"
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
    if (title_label_)
        title_label_->setText(tr("STRATEGIES"));
    if (engine_caption_)
        engine_caption_->setText(tr("ENGINE:"));
    if (status_label_)
        status_label_->setText(tr("NO LIVE EXECUTION"));
    if (mode_label_)
        mode_label_->setText(tr("PAPER ONLY"));

    if (tab_buttons_.size() == 5) {
        tab_buttons_[0]->setText(tr("COCKPIT"));
        tab_buttons_[1]->setText(tr("HANDLERS"));
        tab_buttons_[2]->setText(tr("EVIDENCE"));
        tab_buttons_[3]->setText(tr("RISK & SAFETY"));
        tab_buttons_[4]->setText(tr("RUN HISTORY"));
    }
}

// ── IStatefulScreen ───────────────────────────────────────────────────────────

QVariantMap AlgoTradingScreen::save_state() const {
    return {{"tab_index", active_tab_}};
}

void AlgoTradingScreen::restore_state(const QVariantMap& state) {
    const int idx = state.value("tab_index", 0).toInt();
    on_tab_changed(idx >= 0 && idx < tab_buttons_.size() ? idx : 0);
}

} // namespace openmarketterminal::screens
