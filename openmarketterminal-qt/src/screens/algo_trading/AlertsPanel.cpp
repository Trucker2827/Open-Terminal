// src/screens/algo_trading/AlertsPanel.cpp
#include "screens/algo_trading/AlertsPanel.h"

#include "algo_engine/ScanMonitor.h"
#include "core/logging/Logger.h"
#include "storage/repositories/ScanEventRepository.h"
#include "storage/repositories/ScanWatchRepository.h"
#include "trading/AccountManager.h"
#include "ui/theme/Theme.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonObject>
#include <QScrollArea>
#include <QVBoxLayout>
#include <functional>

namespace {

inline QString kMonoFont() { return QString("font-family: %1;").arg(openmarketterminal::ui::fonts::DATA_FAMILY); }
inline QString kLabelStyle() {
    return QString("color:%1; font-size:%2px; font-weight:700; letter-spacing:0.5px; %3"
                   " background:transparent; border:none;")
        .arg(openmarketterminal::ui::colors::TEXT_SECONDARY()).arg(openmarketterminal::ui::fonts::TINY).arg(kMonoFont());
}
inline QString kSectionLabel() {
    return QString("color:%1; font-size:%2px; font-weight:700; letter-spacing:0.5px; %3"
                   " background:transparent; border:none;")
        .arg(openmarketterminal::ui::colors::AMBER()).arg(openmarketterminal::ui::fonts::TINY).arg(kMonoFont());
}
inline QString kComboStyle() {
    return QString("QComboBox { background:%1; color:%2; border:1px solid %3; padding:4px 8px;"
                   " font-size:%4px; %5 } QComboBox::drop-down { border:none; }"
                   "QComboBox QAbstractItemView { background:%1; color:%2; border:1px solid %3;"
                   " selection-background-color:%6; %5 }")
        .arg(openmarketterminal::ui::colors::BG_SURFACE(), openmarketterminal::ui::colors::TEXT_PRIMARY(), openmarketterminal::ui::colors::BORDER_DIM())
        .arg(openmarketterminal::ui::fonts::SMALL).arg(kMonoFont()).arg(openmarketterminal::ui::colors::BG_HOVER());
}
inline QString kSpinStyle() {
    return QString("QSpinBox { background:%1; color:%2; border:1px solid %3; padding:4px 8px;"
                   " font-size:%4px; %5 } QSpinBox::up-button,QSpinBox::down-button { width:14px; }")
        .arg(openmarketterminal::ui::colors::BG_SURFACE(), openmarketterminal::ui::colors::TEXT_PRIMARY(), openmarketterminal::ui::colors::BORDER_DIM())
        .arg(openmarketterminal::ui::fonts::SMALL).arg(kMonoFont());
}
inline QString kEditStyle() {
    return QString("QTextEdit,QLineEdit { background:%1; border:1px solid %2; color:%3; padding:6px;"
                   " font-size:%4px; %5 } QTextEdit:focus,QLineEdit:focus { border-color:%6; }")
        .arg(openmarketterminal::ui::colors::BG_SURFACE(), openmarketterminal::ui::colors::BORDER_DIM(), openmarketterminal::ui::colors::TEXT_PRIMARY())
        .arg(openmarketterminal::ui::fonts::SMALL).arg(kMonoFont()).arg(openmarketterminal::ui::colors::BORDER_BRIGHT());
}

} // namespace

namespace openmarketterminal::screens {

using namespace openmarketterminal::services::algo;

static openmarketterminal::ScanWatch watch_from_form(openmarketterminal::ui::algo::ConditionSection* section,
                                          openmarketterminal::ui::algo::SymbolChipInput* symbols_input,
                                          QComboBox* timeframe,
                                          QComboBox* data_source, QComboBox* account,
                                          QSpinBox* interval, QSpinBox* cooldown,
                                          QCheckBox* providers, const QString& name) {
    openmarketterminal::ScanWatch w;
    w.name        = name;
    w.conditions  = section->conditions();
    w.logic       = section->combined_logic();
    w.symbols     = symbols_input->symbols();
    w.timeframe   = timeframe->currentText();
    w.lookback_days = algo_default_lookback_days(w.timeframe); // auto — no UI field
    w.data_source = data_source->currentData().toString();
    w.account_id  = account->currentData().toString();
    if (!w.account_id.isEmpty())
        w.broker_id = openmarketterminal::trading::AccountManager::instance().get_account(w.account_id).broker_id;
    w.mode         = QStringLiteral("poll");
    w.interval_sec = interval->value();
    w.cooldown_min = cooldown->value();
    QJsonObject actions;
    actions["toast"]     = true;
    actions["providers"] = providers->isChecked();
    w.actions = actions;
    w.active = true;
    return w;
}

AlertsPanel::AlertsPanel(QWidget* parent) : QWidget(parent) {
    build_ui();
    connect(&openmarketterminal::algo::ScanMonitor::instance(),
            &openmarketterminal::algo::ScanMonitor::watch_status_changed, this,
            [this](const QString&, const QString&) { on_refresh_watches(); });
    on_refresh_watches();
    refresh_history();
    LOG_INFO("AlgoTrading", "AlertsPanel constructed");
}

void AlertsPanel::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(this);
    content->setStyleSheet(QString("background:%1;").arg(openmarketterminal::ui::colors::BG_BASE()));
    auto* vl = new QVBoxLayout(content);
    vl->setContentsMargins(16, 12, 16, 12);
    vl->setSpacing(8);

    cond_title_ = new QLabel(tr("ALERT CONDITIONS"), content);
    cond_title_->setStyleSheet(kSectionLabel());
    vl->addWidget(cond_title_);
    section_ = new openmarketterminal::ui::algo::ConditionSection(
        openmarketterminal::ui::algo::ConditionSection::Type::Entry, content);
    vl->addWidget(section_);

    sym_title_ = new QLabel(tr("SYMBOLS & FEED"), content);
    sym_title_->setStyleSheet(kSectionLabel());
    vl->addWidget(sym_title_);

    symbols_input_ = new openmarketterminal::ui::algo::SymbolChipInput(content);
    symbols_input_->setMinimumHeight(54);
    vl->addWidget(symbols_input_);
    connect(symbols_input_, &openmarketterminal::ui::algo::SymbolChipInput::price_resolved,
            this, [this](const QString& s, double px) { prefill_close_from_price(s, px); });

    auto* quick = new QHBoxLayout;
    auto* nifty = new QPushButton("MEGA CAP", content);
    nifty->setCursor(Qt::PointingHandCursor);
    nifty->setStyleSheet(QString("QPushButton { background:%1; color:%2; border:1px solid %3;"
        " font-size:%4px; font-weight:700; %5 padding:2px 10px; }"
        "QPushButton:hover { background:%6; color:%7; }")
        .arg(openmarketterminal::ui::colors::BG_RAISED(), openmarketterminal::ui::colors::TEXT_SECONDARY(), openmarketterminal::ui::colors::BORDER_DIM())
        .arg(openmarketterminal::ui::fonts::TINY).arg(kMonoFont())
        .arg(openmarketterminal::ui::colors::BG_HOVER(), openmarketterminal::ui::colors::TEXT_PRIMARY()));
    connect(nifty, &QPushButton::clicked, this, [this]() { symbols_input_->set_symbols(us_largecap_symbols()); });
    auto* bn = new QPushButton("TECH", content);
    bn->setCursor(Qt::PointingHandCursor);
    bn->setStyleSheet(nifty->styleSheet());
    connect(bn, &QPushButton::clicked, this, [this]() { symbols_input_->set_symbols(us_tech_symbols()); });
    quick->addWidget(nifty); quick->addWidget(bn); quick->addStretch();
    vl->addLayout(quick);

    auto* grid = new QHBoxLayout;
    auto* col1 = new QVBoxLayout; auto* col2 = new QVBoxLayout;

    tf_lbl_ = new QLabel(tr("TIMEFRAME"), content); tf_lbl_->setStyleSheet(kLabelStyle());
    timeframe_combo_ = new QComboBox(content);
    timeframe_combo_->addItems(algo_timeframes());
    timeframe_combo_->setCurrentIndex(algo_timeframes().indexOf("1m"));
    timeframe_combo_->setStyleSheet(kComboStyle());
    col1->addWidget(tf_lbl_); col1->addWidget(timeframe_combo_);

    ds_lbl_ = new QLabel(tr("DATA SOURCE"), content); ds_lbl_->setStyleSheet(kLabelStyle());
    data_source_combo_ = new QComboBox(content);
    data_source_combo_->addItem(tr("Broker Only"), "Broker");
    data_source_combo_->addItem(tr("Auto (Broker → YFinance)"), "Auto");
    data_source_combo_->addItem(tr("YFinance Only"), "YFinance");
    data_source_combo_->setStyleSheet(kComboStyle());
    col1->addWidget(ds_lbl_); col1->addWidget(data_source_combo_);

    acct_lbl_ = new QLabel(tr("BROKER ACCOUNT"), content); acct_lbl_->setStyleSheet(kLabelStyle());
    account_combo_ = new QComboBox(content);
    account_combo_->addItem(tr("None (use YFinance fallback)"), "");
    for (const auto& acct : openmarketterminal::trading::AccountManager::instance().list_accounts())
        account_combo_->addItem(QString("%1 (%2)").arg(acct.display_name, acct.broker_id), acct.account_id);
    account_combo_->setStyleSheet(kComboStyle());
    col1->addWidget(acct_lbl_); col1->addWidget(account_combo_);
    connect(data_source_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        const bool show = data_source_combo_->currentData().toString() != "YFinance";
        account_combo_->setVisible(show); acct_lbl_->setVisible(show);
    });

    iv_lbl_ = new QLabel(tr("POLL INTERVAL (SEC)"), content); iv_lbl_->setStyleSheet(kLabelStyle());
    interval_spin_ = new QSpinBox(content); interval_spin_->setRange(30, 86400); interval_spin_->setValue(60);
    interval_spin_->setStyleSheet(kSpinStyle());
    col2->addWidget(iv_lbl_); col2->addWidget(interval_spin_);

    cd_lbl_ = new QLabel(tr("COOLDOWN (MIN)"), content); cd_lbl_->setStyleSheet(kLabelStyle());
    cooldown_spin_ = new QSpinBox(content); cooldown_spin_->setRange(0, 1440); cooldown_spin_->setValue(15);
    cooldown_spin_->setStyleSheet(kSpinStyle());
    col2->addWidget(cd_lbl_); col2->addWidget(cooldown_spin_);

    providers_chk_ = new QCheckBox(tr("Send to external providers"), content);
    providers_chk_->setStyleSheet(kLabelStyle());
    col2->addWidget(providers_chk_);
    col2->addStretch();

    grid->addLayout(col1, 1); grid->addLayout(col2, 1);
    vl->addLayout(grid);

    name_lbl_ = new QLabel(tr("WATCH NAME"), content); name_lbl_->setStyleSheet(kLabelStyle());
    name_edit_ = new QLineEdit(content); name_edit_->setStyleSheet(kEditStyle());
    name_edit_->setPlaceholderText(tr("My Volume Watch"));
    vl->addWidget(name_lbl_); vl->addWidget(name_edit_);

    save_btn_ = new QPushButton(tr("SAVE WATCH"), content);
    save_btn_->setCursor(Qt::PointingHandCursor);
    save_btn_->setFixedHeight(36);
    save_btn_->setStyleSheet(QString("QPushButton { background:rgba(255,196,0,0.1); color:#FFC400;"
        " border:1px solid #FFC400; font-size:%1px; font-weight:700; %2 padding:6px 24px; }"
        "QPushButton:hover { background:#FFC400; color:%3; }")
        .arg(openmarketterminal::ui::fonts::DATA).arg(kMonoFont()).arg(openmarketterminal::ui::colors::BG_BASE()));
    connect(save_btn_, &QPushButton::clicked, this, &AlertsPanel::on_save_watch);
    vl->addWidget(save_btn_);

    status_label_ = new QLabel("", content);
    status_label_->setWordWrap(true);
    status_label_->setStyleSheet(kLabelStyle());
    vl->addWidget(status_label_);

    watches_title_ = new QLabel(tr("LIVE WATCHES"), content);
    watches_title_->setStyleSheet(kSectionLabel());
    vl->addWidget(watches_title_);

    watches_table_ = new QTableWidget(0, 5, content);
    watches_table_->setHorizontalHeaderLabels({tr("NAME"), tr("SYMBOLS"), tr("STATUS"), tr("ACTIVE"), tr("ACTIONS")});
    watches_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    watches_table_->setColumnWidth(0, 140); watches_table_->setColumnWidth(2, 90);
    watches_table_->setColumnWidth(3, 70);  watches_table_->setColumnWidth(4, 130);
    watches_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    watches_table_->verticalHeader()->setVisible(false);
    watches_table_->setMinimumHeight(160);
    watches_table_->setStyleSheet(QString("QTableWidget { background:%1; border:1px solid %2;"
        " gridline-color:%2; font-size:%3px; font-family:%4; color:%5; }"
        "QHeaderView::section { background:%6; color:%7; font-size:%3px; font-weight:700;"
        " font-family:%4; padding:4px 8px; border:none; border-bottom:1px solid %2; }")
        .arg(openmarketterminal::ui::colors::BG_BASE(), openmarketterminal::ui::colors::BORDER_DIM())
        .arg(openmarketterminal::ui::fonts::SMALL).arg(openmarketterminal::ui::fonts::DATA_FAMILY)
        .arg(openmarketterminal::ui::colors::TEXT_PRIMARY(), openmarketterminal::ui::colors::BG_RAISED(),
             openmarketterminal::ui::colors::TEXT_TERTIARY()));
    vl->addWidget(watches_table_);

    auto* hist_divider = new QFrame(content);
    hist_divider->setFrameShape(QFrame::HLine);
    hist_divider->setFixedHeight(1);
    hist_divider->setStyleSheet(QString("background:%1; border:none;").arg(openmarketterminal::ui::colors::BORDER_DIM()));
    vl->addSpacing(10);
    vl->addWidget(hist_divider);
    vl->addSpacing(10);

    history_title_ = new QLabel(tr("ALERT HISTORY"), content);
    history_title_->setStyleSheet(kSectionLabel());
    vl->addWidget(history_title_);
    history_table_ = new QTableWidget(0, 3, content);
    history_table_->setHorizontalHeaderLabels({tr("TIME"), tr("SYMBOL"), tr("DETAIL")});
    history_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    history_table_->setColumnWidth(0, 150); history_table_->setColumnWidth(1, 120);
    history_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    history_table_->verticalHeader()->setVisible(false);
    history_table_->setMinimumHeight(120);
    history_table_->setStyleSheet(watches_table_->styleSheet());
    vl->addWidget(history_table_);

    vl->addStretch();

    scroll->setWidget(content);
    root->addWidget(scroll);
}

void AlertsPanel::on_save_watch() {
    if (section_->conditions().isEmpty()) {
        status_label_->setText(tr("Add at least one condition before saving a watch."));
        return;
    }
    QString name = name_edit_->text().trimmed();
    if (name.isEmpty()) name = tr("Watch %1").arg(watches_table_->rowCount() + 1);
    auto w = watch_from_form(section_, symbols_input_, timeframe_combo_, data_source_combo_,
                             account_combo_, interval_spin_, cooldown_spin_, providers_chk_, name);
    if (w.symbols.isEmpty()) {
        status_label_->setText(tr("Enter at least one symbol."));
        return;
    }
    // Editing an existing watch → UPDATE in place + restart its monitor.
    if (!editing_id_.isEmpty()) {
        w.id = editing_id_;
        auto r = openmarketterminal::ScanWatchRepository::instance().update(w);
        if (r.is_err()) {
            status_label_->setText(tr("Update failed: %1").arg(QString::fromStdString(r.error())));
            return;
        }
        openmarketterminal::algo::ScanMonitor::instance().reload(editing_id_);
        status_label_->setText(tr("Watch '%1' updated and re-watching.").arg(name));
        editing_id_.clear();
        save_btn_->setText(tr("SAVE WATCH"));
        name_edit_->clear();
        on_refresh_watches();
        return;
    }
    auto r = openmarketterminal::ScanWatchRepository::instance().create(w);
    if (r.is_err()) {
        status_label_->setText(tr("Save failed: %1").arg(QString::fromStdString(r.error())));
        return;
    }
    openmarketterminal::algo::ScanMonitor::instance().reload(r.value().id);
    status_label_->setText(tr("Watch '%1' saved and monitoring.").arg(name));
    name_edit_->clear();
    on_refresh_watches();
}

void AlertsPanel::load_watch(const openmarketterminal::ScanWatch& w) {
    section_->set_conditions(w.conditions, w.logic);
    symbols_input_->set_symbols(w.symbols);
    if (int i = timeframe_combo_->findText(w.timeframe); i >= 0) timeframe_combo_->setCurrentIndex(i);
    if (int i = data_source_combo_->findData(w.data_source); i >= 0) data_source_combo_->setCurrentIndex(i);
    if (int i = account_combo_->findData(w.account_id); i >= 0) account_combo_->setCurrentIndex(i);
    interval_spin_->setValue(w.interval_sec);
    cooldown_spin_->setValue(w.cooldown_min);
    providers_chk_->setChecked(w.actions.value("providers").toBool(false));
    name_edit_->setText(w.name);
    editing_id_ = w.id;
    save_btn_->setText(tr("UPDATE WATCH"));
    status_label_->setText(tr("Editing '%1' — change conditions/symbols and press UPDATE WATCH.").arg(w.name));
}

void AlertsPanel::on_refresh_watches() {
    watches_table_->setRowCount(0);
    watches_table_->setColumnWidth(4, 200); // fit Edit / Test / Delete
    auto r = openmarketterminal::ScanWatchRepository::instance().list_all();
    if (r.is_err()) return;
    for (const auto& w : r.value()) {
        const int row = watches_table_->rowCount();
        watches_table_->insertRow(row);
        watches_table_->setItem(row, 0, new QTableWidgetItem(w.name));
        watches_table_->setItem(row, 1, new QTableWidgetItem(w.symbols.join(", ")));
        watches_table_->setItem(row, 2, new QTableWidgetItem(w.status));
        auto* active = new QCheckBox(watches_table_);
        active->setChecked(w.active);
        const QString id = w.id;
        connect(active, &QCheckBox::toggled, this, [id](bool on) {
            openmarketterminal::ScanWatchRepository::instance().set_active(id, on);
            openmarketterminal::algo::ScanMonitor::instance().reload(id);
        });
        watches_table_->setCellWidget(row, 3, active);
        auto* cell = new QWidget(watches_table_);
        auto* hl = new QHBoxLayout(cell); hl->setContentsMargins(2, 0, 2, 0);
        auto* edit = new QPushButton(tr("Edit"), cell);
        connect(edit, &QPushButton::clicked, this, [this, w]() { load_watch(w); });
        auto* test = new QPushButton(tr("Test"), cell);
        connect(test, &QPushButton::clicked, this, [id]() { openmarketterminal::algo::ScanMonitor::instance().test_fire(id, QString()); });
        auto* del = new QPushButton(tr("Delete"), cell);
        connect(del, &QPushButton::clicked, this, [this, id]() {
            openmarketterminal::algo::ScanMonitor::instance().stop_watch(id);
            openmarketterminal::ScanWatchRepository::instance().remove(id);
            // Keep the fired history: deleting a live watch must NOT erase its
            // already-triggered events from ALERT HISTORY.
            on_refresh_watches();
        });
        hl->addWidget(edit); hl->addWidget(test); hl->addWidget(del);
        watches_table_->setCellWidget(row, 4, cell);
    }
    refresh_history();
}

void AlertsPanel::refresh_history() {
    history_table_->setRowCount(0);
    auto r = openmarketterminal::ScanEventRepository::instance().recent(50);
    if (r.is_err()) return;
    for (const auto& e : r.value()) {
        const int row = history_table_->rowCount();
        history_table_->insertRow(row);
        history_table_->setItem(row, 0, new QTableWidgetItem(
            QDateTime::fromMSecsSinceEpoch(e.fired_at).toString("yyyy-MM-dd HH:mm:ss")));
        history_table_->setItem(row, 1, new QTableWidgetItem(e.symbol));
        history_table_->setItem(row, 2, new QTableWidgetItem(e.detail));
    }
}

void AlertsPanel::prefill_close_from_price(const QString& /*symbol*/, double price) {
    if (price <= 0) return;
    QJsonArray conds = section_->conditions();
    bool changed = false;
    std::function<void(QJsonArray&)> walk = [&](QJsonArray& arr) {
        for (int i = 0; i < arr.size(); ++i) {
            QJsonObject o = arr[i].toObject();
            if (o.contains("children")) { QJsonArray ch = o.value("children").toArray(); walk(ch); o["children"] = ch; }
            else if (o.value("indicator").toString() == "CLOSE"
                     && o.value("compare_mode").toString("value") == "value"
                     && qFuzzyIsNull(o.value("value").toDouble())) {
                o["value"] = price; changed = true;
            }
            arr[i] = o;
        }
    };
    walk(conds);
    if (changed) section_->set_conditions(conds, section_->combined_logic());
}

void AlertsPanel::prefill(const QJsonArray& conditions, const QString& logic,
                          const QStringList& symbols, const QString& timeframe,
                          const QString& data_source, const QString& account_id) {
    section_->set_conditions(conditions, logic);
    symbols_input_->set_symbols(symbols);
    if (int i = timeframe_combo_->findText(timeframe); i >= 0) timeframe_combo_->setCurrentIndex(i);
    if (int i = data_source_combo_->findData(data_source); i >= 0) data_source_combo_->setCurrentIndex(i);
    if (int i = account_combo_->findData(account_id); i >= 0) account_combo_->setCurrentIndex(i);
    editing_id_.clear();
    save_btn_->setText(tr("SAVE WATCH"));
    status_label_->setText(tr("Pre-filled from Scanner — name it and SAVE WATCH."));
}

void AlertsPanel::changeEvent(QEvent* e) {
    if (e->type() == QEvent::LanguageChange) retranslateUi();
    QWidget::changeEvent(e);
}

void AlertsPanel::retranslateUi() {
    if (cond_title_) cond_title_->setText(tr("ALERT CONDITIONS"));
    if (sym_title_) sym_title_->setText(tr("SYMBOLS & FEED"));
    if (tf_lbl_) tf_lbl_->setText(tr("TIMEFRAME"));
    if (ds_lbl_) ds_lbl_->setText(tr("DATA SOURCE"));
    if (acct_lbl_) acct_lbl_->setText(tr("BROKER ACCOUNT"));
    if (iv_lbl_) iv_lbl_->setText(tr("POLL INTERVAL (SEC)"));
    if (cd_lbl_) cd_lbl_->setText(tr("COOLDOWN (MIN)"));
    if (name_lbl_) name_lbl_->setText(tr("WATCH NAME"));
    if (providers_chk_) providers_chk_->setText(tr("Send to external providers"));
    if (save_btn_) save_btn_->setText(tr("SAVE WATCH"));
    if (watches_title_) watches_title_->setText(tr("LIVE WATCHES"));
}

} // namespace openmarketterminal::screens
