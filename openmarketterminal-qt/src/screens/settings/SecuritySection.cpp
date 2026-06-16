// SecuritySection.cpp — PIN management, auto-lock policy, and audit log.

#include "screens/settings/SecuritySection.h"

#include "auth/InactivityGuard.h"
#include "auth/PinManager.h"
#include "auth/SecurityAuditLog.h"
#include "core/events/EventBus.h"
#include "core/logging/Logger.h"
#include "screens/settings/SettingsRowHelpers.h"
#include "screens/settings/SettingsStyles.h"
#include "storage/repositories/SettingsRepository.h"
#include "ui/theme/Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QString>
#include <QVBoxLayout>

namespace openmarketterminal::screens {

namespace {
// make_row() builds the label (and optional description) QLabel internally;
// grab them back from the row's direct child QLabels so retranslateUi() can
// re-apply text. Order is deterministic: title label first, description second.
void capture_row_labels(QWidget* row, QLabel** label_out, QLabel** desc_out = nullptr) {
    const auto labels = row->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly);
    if (!labels.isEmpty() && label_out)
        *label_out = labels.at(0);
    if (labels.size() > 1 && desc_out)
        *desc_out = labels.at(1);
}
} // namespace

SecuritySection::SecuritySection(QWidget* parent) : QWidget(parent) {
    build_ui();
}

void SecuritySection::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    reload();
}

void SecuritySection::build_ui() {
    using namespace settings_styles;
    using namespace settings_helpers;

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet(QString("QScrollArea { border: none; background: transparent; }"
                                  "QScrollBar:vertical { background: %1; width: 6px; }"
                                  "QScrollBar::handle:vertical { background: %2; }"
                                  "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }")
                              .arg(ui::colors::BG_SURFACE(), ui::colors::BORDER_MED()));

    auto* page = new QWidget(this);
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(24, 24, 24, 24);
    vl->setSpacing(8);

    // ── PIN STATUS ────────────────────────────────────────────────────────────
    title_pin_ = new QLabel(tr("PIN AUTHENTICATION"));
    title_pin_->setStyleSheet(section_title_ss());
    vl->addWidget(title_pin_);
    vl->addWidget(make_sep());
    vl->addSpacing(8);

    sec_pin_status_ = new QLabel;
    sec_pin_status_->setStyleSheet(QString("color:%1;background:transparent;").arg(ui::colors::TEXT_PRIMARY()));
    auto* row_pin_status = make_row(tr("PIN Status"), sec_pin_status_, tr("A 6-digit PIN is required to unlock the terminal."));
    capture_row_labels(row_pin_status, &row_pin_status_lbl_, &row_pin_status_desc_);
    vl->addWidget(row_pin_status);

    sec_lockout_status_ = new QLabel;
    sec_lockout_status_->setStyleSheet(QString("color:%1;background:transparent;").arg(ui::colors::TEXT_SECONDARY()));
    auto* row_attempts = make_row(tr("Failed Attempts"), sec_lockout_status_,
                                  tr("PIN lockout engages after 5 consecutive failures."));
    capture_row_labels(row_attempts, &row_attempts_lbl_, &row_attempts_desc_);
    vl->addWidget(row_attempts);

    vl->addSpacing(8);
    vl->addWidget(make_sep());
    vl->addSpacing(8);

    // ── CHANGE PIN ────────────────────────────────────────────────────────────
    title_change_ = new QLabel(tr("CHANGE PIN"));
    title_change_->setStyleSheet(sub_title_ss());
    vl->addWidget(title_change_);
    vl->addSpacing(4);

    sec_change_pin_btn_ = new QPushButton(tr("Change PIN"));
    sec_change_pin_btn_->setFixedWidth(140);
    sec_change_pin_btn_->setStyleSheet(btn_secondary_ss());
    vl->addWidget(sec_change_pin_btn_);

    sec_change_pin_form_ = new QWidget(this);
    sec_change_pin_form_->setStyleSheet("background: transparent;");
    auto* cpfl = new QVBoxLayout(sec_change_pin_form_);
    cpfl->setContentsMargins(0, 8, 0, 0);
    cpfl->setSpacing(6);

    auto make_pin_field = [&](const QString& placeholder) {
        auto* input = new QLineEdit;
        input->setPlaceholderText(placeholder);
        input->setMaxLength(6);
        input->setEchoMode(QLineEdit::Password);
        input->setFixedWidth(200);
        input->setStyleSheet(input_ss());
        return input;
    };

    sec_current_pin_ = make_pin_field(tr("Current PIN"));
    auto* row_current = make_row(tr("Current PIN"), sec_current_pin_);
    capture_row_labels(row_current, &row_current_lbl_);
    cpfl->addWidget(row_current);

    sec_new_pin_ = make_pin_field(tr("New PIN"));
    auto* row_new = make_row(tr("New PIN"), sec_new_pin_);
    capture_row_labels(row_new, &row_new_lbl_);
    cpfl->addWidget(row_new);

    sec_confirm_pin_ = make_pin_field(tr("Confirm PIN"));
    auto* row_confirm = make_row(tr("Confirm PIN"), sec_confirm_pin_);
    capture_row_labels(row_confirm, &row_confirm_lbl_);
    cpfl->addWidget(row_confirm);

    sec_pin_error_ = new QLabel;
    sec_pin_error_->setWordWrap(true);
    sec_pin_error_->setStyleSheet(
        QString("color:%1;background:rgba(220,38,38,0.08);border:1px solid %2;padding:6px 8px;")
            .arg(ui::colors::NEGATIVE(), ui::colors::NEGATIVE_DIM()));
    sec_pin_error_->hide();
    cpfl->addWidget(sec_pin_error_);

    sec_pin_success_ = new QLabel;
    sec_pin_success_->setWordWrap(true);
    sec_pin_success_->setStyleSheet(
        QString("color:%1;background:rgba(22,163,74,0.08);border:1px solid %2;padding:6px 8px;")
            .arg(ui::colors::POSITIVE(), ui::colors::POSITIVE_DIM()));
    sec_pin_success_->hide();
    cpfl->addWidget(sec_pin_success_);

    save_pin_btn_ = new QPushButton(tr("Update PIN"));
    save_pin_btn_->setFixedWidth(140);
    save_pin_btn_->setStyleSheet(btn_primary_ss());
    cpfl->addWidget(save_pin_btn_);

    sec_change_pin_form_->hide();
    vl->addWidget(sec_change_pin_form_);

    connect(sec_change_pin_btn_, &QPushButton::clicked, this, [this]() {
        bool showing = sec_change_pin_form_->isVisible();
        sec_change_pin_form_->setVisible(!showing);
        sec_change_pin_btn_->setText(showing ? tr("Change PIN") : tr("Cancel"));
        if (!showing) {
            sec_current_pin_->clear();
            sec_new_pin_->clear();
            sec_confirm_pin_->clear();
            sec_pin_error_->hide();
            sec_pin_success_->hide();
            sec_current_pin_->setFocus();
        }
    });

    // Save PIN — all validation in PinManager::change_pin so wrong-current-PIN
    // correctly feeds the shared lockout counter; UI only checks new == confirm.
    connect(save_pin_btn_, &QPushButton::clicked, this, [this]() {
        sec_pin_error_->hide();
        sec_pin_success_->hide();

        auto& pm = auth::PinManager::instance();
        const QString current = sec_current_pin_->text();
        const QString new_pin = sec_new_pin_->text();
        const QString confirm = sec_confirm_pin_->text();

        if (pm.is_locked_out()) {
            int secs = pm.lockout_remaining_seconds();
            sec_pin_error_->setText(tr("Locked out — try again in %1s").arg(secs));
            sec_pin_error_->show();
            sec_current_pin_->clear();
            sec_new_pin_->clear();
            sec_confirm_pin_->clear();
            return;
        }

        if (new_pin != confirm) {
            sec_pin_error_->setText(tr("New PINs do not match"));
            sec_pin_error_->show();
            sec_new_pin_->clear();
            sec_confirm_pin_->clear();
            sec_new_pin_->setFocus();
            return;
        }

        auto result = pm.change_pin(current, new_pin);
        if (result.is_err()) {
            if (pm.is_locked_out()) {
                int secs = pm.lockout_remaining_seconds();
                sec_pin_error_->setText(tr("Too many failed attempts — locked for %1s").arg(secs));
            } else {
                sec_pin_error_->setText(QString::fromStdString(result.error()));
            }
            sec_pin_error_->show();
            sec_current_pin_->clear();
            sec_current_pin_->setFocus();
            return;
        }

        sec_pin_success_->setText(tr("PIN updated successfully"));
        sec_pin_success_->show();
        sec_current_pin_->clear();
        sec_new_pin_->clear();
        sec_confirm_pin_->clear();
        reload();

        LOG_INFO("Settings", "PIN changed via Security settings");
    });

    vl->addSpacing(8);
    vl->addWidget(make_sep());
    vl->addSpacing(8);

    // ── AUTO-LOCK ─────────────────────────────────────────────────────────────
    title_lock_ = new QLabel(tr("AUTO-LOCK"));
    title_lock_->setStyleSheet(sub_title_ss());
    vl->addWidget(title_lock_);
    vl->addSpacing(4);

    sec_autolock_toggle_ = new QCheckBox(tr("Enable auto-lock on inactivity"));
    sec_autolock_toggle_->setChecked(true);
    sec_autolock_toggle_->setStyleSheet(check_ss());
    auto* row_autolock = make_row(tr("Auto-Lock"), sec_autolock_toggle_, tr("Locks the terminal after a period of inactivity."));
    capture_row_labels(row_autolock, &row_autolock_lbl_, &row_autolock_desc_);
    vl->addWidget(row_autolock);

    sec_lock_timeout_ = new QComboBox;
    sec_lock_timeout_->addItem(tr("1 min"),  1);
    sec_lock_timeout_->addItem(tr("2 min"),  2);
    sec_lock_timeout_->addItem(tr("5 min"),  5);
    sec_lock_timeout_->addItem(tr("10 min"), 10);
    sec_lock_timeout_->addItem(tr("15 min"), 15);
    sec_lock_timeout_->addItem(tr("30 min"), 30);
    sec_lock_timeout_->addItem(tr("60 min"), 60);
    sec_lock_timeout_->setCurrentIndex(3);
    sec_lock_timeout_->setStyleSheet(combo_ss());
    auto* row_timeout = make_row(tr("Lock Timeout"), sec_lock_timeout_, tr("Time of inactivity before the terminal locks."));
    capture_row_labels(row_timeout, &row_timeout_lbl_, &row_timeout_desc_);
    vl->addWidget(row_timeout);

    connect(sec_autolock_toggle_, &QCheckBox::toggled, this,
            [this](bool checked) { sec_lock_timeout_->setEnabled(checked); });

    sec_lock_on_minimize_ = new QCheckBox(tr("Lock when the window is minimized"));
    sec_lock_on_minimize_->setStyleSheet(check_ss());
    auto* row_minimize = make_row(tr("Lock on Minimize"), sec_lock_on_minimize_,
                                  tr("When on, minimizing the terminal immediately shows the PIN screen."));
    capture_row_labels(row_minimize, &row_minimize_lbl_, &row_minimize_desc_);
    vl->addWidget(row_minimize);

    vl->addSpacing(8);
    vl->addWidget(make_sep());
    vl->addSpacing(8);

    // ── CLI ACCESS ──────────────────────────────────────────────────────────────
    // Both default OFF. These are the canonical, human-owned gates the headless
    // CLI / attached client read and obey (cli.allow_settings_write,
    // cli.allow_trading). They lift only the CLI's own gate — never the app's
    // deterministic risk limits / kill-switch.
    title_cli_ = new QLabel(tr("CLI ACCESS"));
    title_cli_->setStyleSheet(sub_title_ss());
    vl->addWidget(title_cli_);
    vl->addSpacing(4);

    cli_settings_write_toggle_ = new QCheckBox(tr("Allow CLI to write settings"));
    cli_settings_write_toggle_->setStyleSheet(check_ss());
    auto* row_cli_write = make_row(
        tr("CLI Settings Write"), cli_settings_write_toggle_,
        tr("When on, the command-line interface may change app settings (config-write tools). Reads are always allowed."));
    capture_row_labels(row_cli_write, &row_cli_write_lbl_, &row_cli_write_desc_);
    vl->addWidget(row_cli_write);

    cli_trading_toggle_ = new QCheckBox(tr("Allow CLI trading / destructive actions"));
    cli_trading_toggle_->setStyleSheet(check_ss());
    auto* row_cli_trade = make_row(
        tr("CLI Trading / Destructive"), cli_trading_toggle_,
        tr("When on, the command-line interface may run destructive / trading-execution tools (live orders, Python exec, agent mutation). The app's risk limits and kill-switch still apply."));
    capture_row_labels(row_cli_trade, &row_cli_trade_lbl_, &row_cli_trade_desc_);
    vl->addWidget(row_cli_trade);

    cli_paper_trading_toggle_ = new QCheckBox(tr("Allow CLI paper trading"));
    cli_paper_trading_toggle_->setStyleSheet(check_ss());
    auto* row_cli_paper = make_row(
        tr("CLI Paper Trading"), cli_paper_trading_toggle_,
        tr("When on, the command-line interface / AI agent may submit simulated (paper) orders. No real money. Off by default."));
    capture_row_labels(row_cli_paper, &row_cli_paper_lbl_, &row_cli_paper_desc_);
    vl->addWidget(row_cli_paper);

    cli_live_trading_toggle_ = new QCheckBox(tr("Arm CLI LIVE trading (advanced)"));
    cli_live_trading_toggle_->setStyleSheet(check_ss());
    auto* row_cli_live = make_row(
        tr("CLI LIVE Trading (advanced)"), cli_live_trading_toggle_,
        tr("Arms LIVE order execution for the command-line interface / AI agent. Real money. Only a human can set this here; the CLI/agent can never arm itself. Off by default."));
    capture_row_labels(row_cli_live, &row_cli_live_lbl_, &row_cli_live_desc_);
    vl->addWidget(row_cli_live);

    cli_fast_live_toggle_ = new QCheckBox(tr("Arm FAST live mode (advanced)"));
    cli_fast_live_toggle_->setStyleSheet(check_ss());
    auto* row_cli_fast = make_row(
        tr("FAST Live Mode (advanced)"), cli_fast_live_toggle_,
        tr("Arms the low-latency fast-trading tool set for the AI agent — a SECOND arm on top of LIVE trading. Real money. Requires CLI LIVE trading to also be armed. Only a human can set this here; the CLI/agent can never arm itself. Off by default."));
    capture_row_labels(row_cli_fast, &row_cli_fast_lbl_, &row_cli_fast_desc_);
    vl->addWidget(row_cli_fast);

    // AI-trading constitution (Phase B): which venues the agent may trade and the
    // per-topic exposure cap. GUI-only (cli.* keys the CLI/agent can never write).
    cli_allowed_venues_edit_ = new QLineEdit;
    cli_allowed_venues_edit_->setPlaceholderText(tr("e.g. polymarket, kalshi"));
    cli_allowed_venues_edit_->setFixedWidth(280);
    cli_allowed_venues_edit_->setStyleSheet(input_ss());
    auto* row_cli_venues = make_row(
        tr("Allowed AI venues (comma list)"), cli_allowed_venues_edit_,
        tr("Comma-separated venues the AI agent may trade (e.g. polymarket, kalshi). Empty means none are allowed."));
    capture_row_labels(row_cli_venues, &row_cli_venues_lbl_, &row_cli_venues_desc_);
    vl->addWidget(row_cli_venues);

    cli_max_exposure_edit_ = new QLineEdit;
    cli_max_exposure_edit_->setPlaceholderText(tr("e.g. 500"));
    cli_max_exposure_edit_->setFixedWidth(200);
    cli_max_exposure_edit_->setStyleSheet(input_ss());
    auto* row_cli_expo = make_row(
        tr("Max exposure per topic ($)"), cli_max_exposure_edit_,
        tr("Maximum total stake (USD) the AI agent may hold open in a single topic/category."));
    capture_row_labels(row_cli_expo, &row_cli_expo_lbl_, &row_cli_expo_desc_);
    vl->addWidget(row_cli_expo);

    // AI-trading constitution (Phase C): the kill switch (panic button), the
    // single account the AI may live-trade, and the max daily loss cap. GUI-only
    // (cli.* keys the CLI/agent can never write).
    cli_kill_switch_toggle_ = new QCheckBox(tr("Kill switch — halt all AI trading"));
    cli_kill_switch_toggle_->setStyleSheet(check_ss());
    auto* row_cli_kill = make_row(
        tr("Kill Switch"), cli_kill_switch_toggle_,
        tr("PANIC button. When on, ALL AI trading is halted immediately — paper AND live, every venue. The CLI/agent can never clear it."));
    capture_row_labels(row_cli_kill, &row_cli_kill_lbl_, &row_cli_kill_desc_);
    vl->addWidget(row_cli_kill);

    cli_allowed_account_edit_ = new QLineEdit;
    cli_allowed_account_edit_->setPlaceholderText(tr("e.g. acct-1"));
    cli_allowed_account_edit_->setFixedWidth(280);
    cli_allowed_account_edit_->setStyleSheet(input_ss());
    auto* row_cli_acct = make_row(
        tr("AI-allowed account id"), cli_allowed_account_edit_,
        tr("The single account the AI agent may LIVE-trade. Empty means none is allowed."));
    capture_row_labels(row_cli_acct, &row_cli_acct_lbl_, &row_cli_acct_desc_);
    vl->addWidget(row_cli_acct);

    cli_max_daily_loss_edit_ = new QLineEdit;
    cli_max_daily_loss_edit_->setPlaceholderText(tr("e.g. 5000"));
    cli_max_daily_loss_edit_->setFixedWidth(200);
    cli_max_daily_loss_edit_->setStyleSheet(input_ss());
    auto* row_cli_dloss = make_row(
        tr("Max daily loss ($)"), cli_max_daily_loss_edit_,
        tr("Maximum total loss (USD) the AI agent may incur in a single day before trading is halted."));
    capture_row_labels(row_cli_dloss, &row_cli_dloss_lbl_, &row_cli_dloss_desc_);
    vl->addWidget(row_cli_dloss);

    cli_max_order_value_edit_ = new QLineEdit;
    cli_max_order_value_edit_->setPlaceholderText(tr("e.g. 50"));
    cli_max_order_value_edit_->setFixedWidth(200);
    cli_max_order_value_edit_->setStyleSheet(input_ss());
    auto* row_cli_ordval = make_row(
        tr("Max order value ($)"), cli_max_order_value_edit_,
        tr("Maximum notional value (USD) of any single order the AI agent may place. Unset = 25000."));
    capture_row_labels(row_cli_ordval, &row_cli_ordval_lbl_, &row_cli_ordval_desc_);
    vl->addWidget(row_cli_ordval);

    vl->addSpacing(16);

    // ── SAVE ──────────────────────────────────────────────────────────────────
    save_btn_ = new QPushButton(tr("Save Security Settings"));
    save_btn_->setFixedWidth(200);
    save_btn_->setStyleSheet(btn_primary_ss());
    connect(save_btn_, &QPushButton::clicked, this, [this]() {
        auto& repo  = SettingsRepository::instance();
        auto& guard = auth::InactivityGuard::instance();
        auto& pm    = auth::PinManager::instance();

        bool autolock = sec_autolock_toggle_->isChecked();
        int  minutes  = sec_lock_timeout_->currentData().toInt();

        repo.set("security.autolock_enabled",     autolock ? "true" : "false", "security");
        repo.set("security.lock_timeout_minutes", QString::number(minutes),    "security");
        if (sec_lock_on_minimize_) {
            repo.set("security.lock_on_minimize",
                     sec_lock_on_minimize_->isChecked() ? "true" : "false", "security");
        }

        // CLI capability gates — canonical, human-owned. Persisted under the
        // exact keys the CLI/headless runtime reads (default-deny on absence).
        if (cli_settings_write_toggle_) {
            repo.set("cli.allow_settings_write",
                     cli_settings_write_toggle_->isChecked() ? "true" : "false", "cli");
        }
        if (cli_trading_toggle_) {
            repo.set("cli.allow_trading",
                     cli_trading_toggle_->isChecked() ? "true" : "false", "cli");
        }
        if (cli_paper_trading_toggle_) {
            repo.set("cli.allow_paper_trading",
                     cli_paper_trading_toggle_->isChecked() ? "true" : "false", "cli");
        }
        if (cli_live_trading_toggle_) {
            repo.set("cli.live_trading_armed",
                     cli_live_trading_toggle_->isChecked() ? "true" : "false", "cli");
        }
        if (cli_fast_live_toggle_) {
            repo.set("cli.fast_live_armed",
                     cli_fast_live_toggle_->isChecked() ? "true" : "false", "cli");
        }
        // AI-trading constitution (Phase B): allowed venues + per-topic exposure
        // cap. Stored under cli.* so the CLI/agent reads them but can never write.
        if (cli_allowed_venues_edit_) {
            repo.set("cli.allowed_venues", cli_allowed_venues_edit_->text().trimmed(), "cli");
        }
        if (cli_max_exposure_edit_) {
            repo.set("cli.risk.max_exposure_per_topic", cli_max_exposure_edit_->text().trimmed(), "cli");
        }
        // AI-trading constitution (Phase C): kill switch + allowed account + daily
        // loss cap. Stored under cli.* so the CLI/agent reads them but can never write.
        if (cli_kill_switch_toggle_) {
            repo.set("cli.kill_switch",
                     cli_kill_switch_toggle_->isChecked() ? "true" : "false", "cli");
        }
        if (cli_allowed_account_edit_) {
            repo.set("cli.allowed_account", cli_allowed_account_edit_->text().trimmed(), "cli");
        }
        if (cli_max_daily_loss_edit_) {
            repo.set("cli.risk.max_daily_loss", cli_max_daily_loss_edit_->text().trimmed(), "cli");
        }
        if (cli_max_order_value_edit_) {
            repo.set("cli.risk.max_order_value", cli_max_order_value_edit_->text().trimmed(), "cli");
        }
        // Notify listeners. These cli.* flags are read directly from the DB by
        // the CLI's headless auth-checker on every tool call, so revoking
        // cli.allow_trading here takes effect on the next call with no restart and
        // no token hand-off (there is no destructive token in bridge.json — that
        // mechanism was removed; the toggles are now the authoritative gate).
        EventBus::instance().publish("settings.changed", QVariantMap{{"key", "cli.*"}});

        // Always update interval; only flip enabled if a PIN is configured
        // (otherwise the timer fires but show_lock_screen suppresses the
        // request, leaving an invisible churning timer).
        guard.set_timeout_minutes(minutes);
        guard.set_enabled(autolock && pm.has_pin());

        LOG_INFO("Settings",
                 QString("Security settings saved: autolock=%1, timeout=%2min, has_pin=%3")
                     .arg(autolock).arg(minutes).arg(pm.has_pin()));
    });
    vl->addWidget(save_btn_);

    // ── AUDIT LOG ─────────────────────────────────────────────────────────────
    vl->addSpacing(16);
    title_audit_ = new QLabel(tr("AUDIT LOG"));
    title_audit_->setStyleSheet(sub_title_ss());
    vl->addWidget(title_audit_);
    vl->addSpacing(4);

    audit_note_ = new QLabel(tr("Recent security events (PIN setup, failed unlocks, inactivity locks)."));
    audit_note_->setWordWrap(true);
    audit_note_->setStyleSheet(QString("color:%1;font-size:12px;background:transparent;")
                                  .arg(ui::colors::TEXT_DIM()));
    vl->addWidget(audit_note_);

    sec_audit_list_ = new QListWidget;
    sec_audit_list_->setFixedHeight(180);
    sec_audit_list_->setStyleSheet(
        QString("QListWidget { background:%1; color:%2; border:1px solid %3;"
                "font-family:'Consolas','Courier New',monospace; font-size:12px; }"
                "QListWidget::item { padding:2px 6px; }")
            .arg(ui::colors::BG_SURFACE(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM()));
    vl->addWidget(sec_audit_list_);

    refresh_audit_btn_ = new QPushButton(tr("Refresh"));
    refresh_audit_btn_->setFixedWidth(140);
    refresh_audit_btn_->setStyleSheet(btn_secondary_ss());
    connect(refresh_audit_btn_, &QPushButton::clicked, this, [this]() { refresh_audit_log(); });
    vl->addWidget(refresh_audit_btn_);

    vl->addStretch();
    scroll->setWidget(page);
    root->addWidget(scroll);
}

void SecuritySection::refresh_audit_log() {
    if (!sec_audit_list_) return;
    sec_audit_list_->clear();
    const auto events = auth::SecurityAuditLog::instance().recent(100);
    for (const auto& e : events) {
        const QString ts = e.timestamp.toString("yyyy-MM-dd hh:mm:ss");
        const QString line = e.detail.isEmpty()
                                 ? QString("%1  %2").arg(ts, e.event)
                                 : QString("%1  %2  (%3)").arg(ts, e.event, e.detail);
        sec_audit_list_->addItem(line);
    }
    if (events.isEmpty()) sec_audit_list_->addItem(tr("(no events recorded yet)"));
}

void SecuritySection::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void SecuritySection::retranslateUi() {
    // Section titles.
    if (title_pin_)    title_pin_->setText(tr("PIN AUTHENTICATION"));
    if (title_change_) title_change_->setText(tr("CHANGE PIN"));
    if (title_lock_)   title_lock_->setText(tr("AUTO-LOCK"));
    if (title_cli_)    title_cli_->setText(tr("CLI ACCESS"));
    if (title_audit_)  title_audit_->setText(tr("AUDIT LOG"));
    if (audit_note_)   audit_note_->setText(tr("Recent security events (PIN setup, failed unlocks, inactivity locks)."));

    // Row labels + descriptions.
    if (row_pin_status_lbl_)  row_pin_status_lbl_->setText(tr("PIN Status"));
    if (row_pin_status_desc_) row_pin_status_desc_->setText(tr("A 6-digit PIN is required to unlock the terminal."));
    if (row_attempts_lbl_)    row_attempts_lbl_->setText(tr("Failed Attempts"));
    if (row_attempts_desc_)   row_attempts_desc_->setText(tr("PIN lockout engages after 5 consecutive failures."));
    if (row_current_lbl_)     row_current_lbl_->setText(tr("Current PIN"));
    if (row_new_lbl_)         row_new_lbl_->setText(tr("New PIN"));
    if (row_confirm_lbl_)     row_confirm_lbl_->setText(tr("Confirm PIN"));
    if (row_autolock_lbl_)    row_autolock_lbl_->setText(tr("Auto-Lock"));
    if (row_autolock_desc_)   row_autolock_desc_->setText(tr("Locks the terminal after a period of inactivity."));
    if (row_timeout_lbl_)     row_timeout_lbl_->setText(tr("Lock Timeout"));
    if (row_timeout_desc_)    row_timeout_desc_->setText(tr("Time of inactivity before the terminal locks."));
    if (row_minimize_lbl_)    row_minimize_lbl_->setText(tr("Lock on Minimize"));
    if (row_minimize_desc_)   row_minimize_desc_->setText(tr("When on, minimizing the terminal immediately shows the PIN screen."));
    if (row_cli_write_lbl_)   row_cli_write_lbl_->setText(tr("CLI Settings Write"));
    if (row_cli_write_desc_)  row_cli_write_desc_->setText(tr("When on, the command-line interface may change app settings (config-write tools). Reads are always allowed."));
    if (row_cli_trade_lbl_)   row_cli_trade_lbl_->setText(tr("CLI Trading / Destructive"));
    if (row_cli_trade_desc_)  row_cli_trade_desc_->setText(tr("When on, the command-line interface may run destructive / trading-execution tools (live orders, Python exec, agent mutation). The app's risk limits and kill-switch still apply."));
    if (row_cli_paper_lbl_)   row_cli_paper_lbl_->setText(tr("CLI Paper Trading"));
    if (row_cli_paper_desc_)  row_cli_paper_desc_->setText(tr("When on, the command-line interface / AI agent may submit simulated (paper) orders. No real money. Off by default."));
    if (row_cli_live_lbl_)    row_cli_live_lbl_->setText(tr("CLI LIVE Trading (advanced)"));
    if (row_cli_live_desc_)   row_cli_live_desc_->setText(tr("Arms LIVE order execution for the command-line interface / AI agent. Real money. Only a human can set this here; the CLI/agent can never arm itself. Off by default."));
    if (row_cli_fast_lbl_)    row_cli_fast_lbl_->setText(tr("FAST Live Mode (advanced)"));
    if (row_cli_fast_desc_)   row_cli_fast_desc_->setText(tr("Arms the low-latency fast-trading tool set for the AI agent — a SECOND arm on top of LIVE trading. Real money. Requires CLI LIVE trading to also be armed. Only a human can set this here; the CLI/agent can never arm itself. Off by default."));
    if (row_cli_venues_lbl_)  row_cli_venues_lbl_->setText(tr("Allowed AI venues (comma list)"));
    if (row_cli_venues_desc_) row_cli_venues_desc_->setText(tr("Comma-separated venues the AI agent may trade (e.g. polymarket, kalshi). Empty means none are allowed."));
    if (row_cli_expo_lbl_)    row_cli_expo_lbl_->setText(tr("Max exposure per topic ($)"));
    if (row_cli_expo_desc_)   row_cli_expo_desc_->setText(tr("Maximum total stake (USD) the AI agent may hold open in a single topic/category."));
    if (row_cli_kill_lbl_)    row_cli_kill_lbl_->setText(tr("Kill Switch"));
    if (row_cli_kill_desc_)   row_cli_kill_desc_->setText(tr("PANIC button. When on, ALL AI trading is halted immediately — paper AND live, every venue. The CLI/agent can never clear it."));
    if (row_cli_acct_lbl_)    row_cli_acct_lbl_->setText(tr("AI-allowed account id"));
    if (row_cli_acct_desc_)   row_cli_acct_desc_->setText(tr("The single account the AI agent may LIVE-trade. Empty means none is allowed."));
    if (row_cli_dloss_lbl_)   row_cli_dloss_lbl_->setText(tr("Max daily loss ($)"));
    if (row_cli_dloss_desc_)  row_cli_dloss_desc_->setText(tr("Maximum total loss (USD) the AI agent may incur in a single day before trading is halted."));
    if (row_cli_ordval_lbl_)  row_cli_ordval_lbl_->setText(tr("Max order value ($)"));
    if (row_cli_ordval_desc_) row_cli_ordval_desc_->setText(tr("Maximum notional value (USD) of any single order the AI agent may place. Unset = 25000."));

    // Checkbox texts.
    if (sec_autolock_toggle_)  sec_autolock_toggle_->setText(tr("Enable auto-lock on inactivity"));
    if (sec_lock_on_minimize_) sec_lock_on_minimize_->setText(tr("Lock when the window is minimized"));
    if (cli_settings_write_toggle_) cli_settings_write_toggle_->setText(tr("Allow CLI to write settings"));
    if (cli_trading_toggle_)        cli_trading_toggle_->setText(tr("Allow CLI trading / destructive actions"));
    if (cli_paper_trading_toggle_)  cli_paper_trading_toggle_->setText(tr("Allow CLI paper trading"));
    if (cli_live_trading_toggle_)   cli_live_trading_toggle_->setText(tr("Arm CLI LIVE trading (advanced)"));
    if (cli_fast_live_toggle_)      cli_fast_live_toggle_->setText(tr("Arm FAST live mode (advanced)"));
    if (cli_kill_switch_toggle_)    cli_kill_switch_toggle_->setText(tr("Kill switch — halt all AI trading"));

    // PIN field placeholders.
    if (sec_current_pin_) sec_current_pin_->setPlaceholderText(tr("Current PIN"));
    if (sec_new_pin_)     sec_new_pin_->setPlaceholderText(tr("New PIN"));
    if (sec_confirm_pin_) sec_confirm_pin_->setPlaceholderText(tr("Confirm PIN"));
    if (cli_allowed_venues_edit_) cli_allowed_venues_edit_->setPlaceholderText(tr("e.g. polymarket, kalshi"));
    if (cli_max_exposure_edit_)   cli_max_exposure_edit_->setPlaceholderText(tr("e.g. 500"));
    if (cli_allowed_account_edit_) cli_allowed_account_edit_->setPlaceholderText(tr("e.g. acct-1"));
    if (cli_max_daily_loss_edit_)  cli_max_daily_loss_edit_->setPlaceholderText(tr("e.g. 5000"));

    // Buttons. The change-PIN button toggles label with form visibility.
    if (sec_change_pin_btn_)
        sec_change_pin_btn_->setText(sec_change_pin_form_ && sec_change_pin_form_->isVisible() ? tr("Cancel")
                                                                                              : tr("Change PIN"));
    if (save_pin_btn_)      save_pin_btn_->setText(tr("Update PIN"));
    if (save_btn_)          save_btn_->setText(tr("Save Security Settings"));
    if (refresh_audit_btn_) refresh_audit_btn_->setText(tr("Refresh"));

    // Lock-timeout combo items are fixed UI labels (data = minutes). Re-apply
    // text per index without disturbing the selection.
    if (sec_lock_timeout_ && sec_lock_timeout_->count() >= 7) {
        const QSignalBlocker b(sec_lock_timeout_);
        sec_lock_timeout_->setItemText(0, tr("1 min"));
        sec_lock_timeout_->setItemText(1, tr("2 min"));
        sec_lock_timeout_->setItemText(2, tr("5 min"));
        sec_lock_timeout_->setItemText(3, tr("10 min"));
        sec_lock_timeout_->setItemText(4, tr("15 min"));
        sec_lock_timeout_->setItemText(5, tr("30 min"));
        sec_lock_timeout_->setItemText(6, tr("60 min"));
    }

    // State-dependent text (PIN status, lockout counter, audit log) is re-derived
    // by reload() in the current language.
    reload();
}

void SecuritySection::reload() {
    auto& pm   = auth::PinManager::instance();
    auto& repo = SettingsRepository::instance();

    if (sec_pin_status_) {
        sec_pin_status_->setText(pm.has_pin() ? tr("CONFIGURED") : tr("NOT SET"));
        sec_pin_status_->setStyleSheet(QString("color:%1;font-weight:700;background:transparent;")
                                           .arg(pm.has_pin() ? ui::colors::POSITIVE() : ui::colors::NEGATIVE()));
    }

    if (sec_lockout_status_) {
        int attempts = pm.failed_attempts();
        sec_lockout_status_->setText(QString("%1 / 5").arg(attempts));
        sec_lockout_status_->setStyleSheet(
            QString("color:%1;font-weight:700;background:transparent;")
                .arg(attempts > 0 ? ui::colors::WARNING() : ui::colors::TEXT_SECONDARY()));
    }

    if (sec_change_pin_btn_) sec_change_pin_btn_->setEnabled(pm.has_pin());

    if (sec_autolock_toggle_ && sec_lock_timeout_) {
        const QSignalBlocker b1(sec_autolock_toggle_);
        const QSignalBlocker b2(sec_lock_timeout_);

        auto r_enabled = repo.get("security.autolock_enabled");
        bool enabled = !r_enabled.is_ok() || r_enabled.value() != "false";
        sec_autolock_toggle_->setChecked(enabled);
        sec_lock_timeout_->setEnabled(enabled);

        auto r_timeout = repo.get("security.lock_timeout_minutes");
        int minutes = r_timeout.is_ok() ? r_timeout.value().toInt() : 10;
        for (int i = 0; i < sec_lock_timeout_->count(); ++i) {
            if (sec_lock_timeout_->itemData(i).toInt() == minutes) {
                sec_lock_timeout_->setCurrentIndex(i);
                break;
            }
        }
    }

    if (sec_lock_on_minimize_) {
        const QSignalBlocker b(sec_lock_on_minimize_);
        auto r = repo.get("security.lock_on_minimize");
        sec_lock_on_minimize_->setChecked(r.is_ok() && r.value() == "true");
    }

    // CLI gates default OFF — only a literal "true" checks the box.
    if (cli_settings_write_toggle_) {
        const QSignalBlocker b(cli_settings_write_toggle_);
        auto r = repo.get("cli.allow_settings_write", "false");
        cli_settings_write_toggle_->setChecked(r.is_ok() && r.value() == "true");
    }
    if (cli_trading_toggle_) {
        const QSignalBlocker b(cli_trading_toggle_);
        auto r = repo.get("cli.allow_trading", "false");
        cli_trading_toggle_->setChecked(r.is_ok() && r.value() == "true");
    }
    if (cli_paper_trading_toggle_) {
        const QSignalBlocker b(cli_paper_trading_toggle_);
        auto r = repo.get("cli.allow_paper_trading", "false");
        cli_paper_trading_toggle_->setChecked(r.is_ok() && r.value() == "true");
    }
    if (cli_live_trading_toggle_) {
        const QSignalBlocker b(cli_live_trading_toggle_);
        auto r = repo.get("cli.live_trading_armed", "false");
        cli_live_trading_toggle_->setChecked(r.is_ok() && r.value() == "true");
    }
    if (cli_fast_live_toggle_) {
        const QSignalBlocker b(cli_fast_live_toggle_);
        auto r = repo.get("cli.fast_live_armed", "false");
        cli_fast_live_toggle_->setChecked(r.is_ok() && r.value() == "true");
    }
    if (cli_allowed_venues_edit_) {
        const QSignalBlocker b(cli_allowed_venues_edit_);
        auto r = repo.get("cli.allowed_venues", "");
        cli_allowed_venues_edit_->setText(r.is_ok() ? r.value() : QString());
    }
    if (cli_max_exposure_edit_) {
        const QSignalBlocker b(cli_max_exposure_edit_);
        auto r = repo.get("cli.risk.max_exposure_per_topic", "");
        cli_max_exposure_edit_->setText(r.is_ok() ? r.value() : QString());
    }
    if (cli_kill_switch_toggle_) {
        const QSignalBlocker b(cli_kill_switch_toggle_);
        auto r = repo.get("cli.kill_switch", "false");
        cli_kill_switch_toggle_->setChecked(r.is_ok() && r.value() == "true");
    }
    if (cli_allowed_account_edit_) {
        const QSignalBlocker b(cli_allowed_account_edit_);
        auto r = repo.get("cli.allowed_account", "");
        cli_allowed_account_edit_->setText(r.is_ok() ? r.value() : QString());
    }
    if (cli_max_daily_loss_edit_) {
        const QSignalBlocker b(cli_max_daily_loss_edit_);
        auto r = repo.get("cli.risk.max_daily_loss", "");
        cli_max_daily_loss_edit_->setText(r.is_ok() ? r.value() : QString());
    }
    if (cli_max_order_value_edit_) {
        const QSignalBlocker b(cli_max_order_value_edit_);
        auto r = repo.get("cli.risk.max_order_value", "");
        cli_max_order_value_edit_->setText(r.is_ok() ? r.value() : QString());
    }

    refresh_audit_log();
}

} // namespace openmarketterminal::screens
