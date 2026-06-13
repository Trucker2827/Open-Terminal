// AccountManagementDialog.cpp — multi-account credential management
#include "screens/equity_trading/AccountManagementDialog.h"

#include "core/logging/Logger.h"

#include "trading/AccountManager.h"
#include "trading/BrokerRegistry.h"
#include "trading/auth/RedirectServer.h"
#include "trading/brokers/metaapi/MetaApiBroker.h"
#include "ui/theme/Theme.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QRadioButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace openmarketterminal::screens::equity {

using namespace openmarketterminal::ui;
using namespace openmarketterminal::trading;

// ── helpers ──────────────────────────────────────────────────────────────────

static QLineEdit* make_field(const QString& placeholder, bool password = false) {
    auto* f = new QLineEdit;
    f->setPlaceholderText(placeholder);
    if (password)
        f->setEchoMode(QLineEdit::Password);
    return f;
}

static QLabel* make_field_label(const QString& text) {
    auto* l = new QLabel(text);
    l->setObjectName("fieldLabel");
    return l;
}

// Assembled exchange_token() inputs. The generic dialog collects one value per
// declared CredentialField, but each broker's exchange_token(api_key, api_secret,
// auth_code) parses those three args differently (some pack multiple secrets into
// one). Centralising the packing here keeps per-broker quirks in ONE place
// instead of scattered branches in the connect handler — add a broker by adding
// one case below, not by editing the form + handler + loader separately.
struct ExchangeArgs {
    QString api_key;
    QString api_secret;
    QString auth_code;
    QString additional_data; // pre-exchange additional_data to merge after token exchange
};

static ExchangeArgs assemble_exchange_args(const QString& broker_id,
                                           const QMap<int, QString>& f) {
    using CF = CredentialField;
    ExchangeArgs a;
    a.api_key = f.value(static_cast<int>(CF::ApiKey));
    a.api_secret = f.value(static_cast<int>(CF::ApiSecret));
    a.auth_code = f.value(static_cast<int>(CF::AuthCode));
    const QString client_code = f.value(static_cast<int>(CF::ClientCode));

    // Default mapping (unchanged historical behaviour): api_key/api_secret/auth_code
    // pass straight through; a ClientCode field rides in additional_data as JSON so
    // load_saved_credentials() can repopulate it on reopen.
    if (!client_code.isEmpty())
        a.additional_data =
            QJsonDocument(QJsonObject{{"client_code", client_code}}).toJson(QJsonDocument::Compact);
    return a;
}

// ── Custom multi-sub-field credential forms ──────────────────────────────────
// Hook for brokers whose exchange_token() packs several distinct secrets into a
// single arg with a delimiter, needing one labelled box PER secret instead of the
// plain one-box-per-CredentialField form. No currently-supported broker (Alpaca,
// IBKR, Tradier, Saxo, MT4) uses this, so the list is empty and the plain
// profile-driven form is always used; kept as a seam for future brokers.
struct CredSubField {
    QString key;   // identifier used by the packer below
    QString label; // UI label
    bool secret;   // echo as password
};

// Returns the custom sub-field list for a broker, or empty if the broker uses the
// plain profile-driven form. Keys MUST match pack_custom_credentials() below.
static QVector<CredSubField> custom_cred_fields(const QString& /*broker_id*/) {
    return {};
}

// Pack custom sub-field values into the exact exchange_token() args a broker
// parses. No-op until a broker registers custom sub-fields above.
static ExchangeArgs pack_custom_credentials(const QString& /*broker_id*/,
                                            const QMap<QString, QString>& /*v*/) {
    return {};
}

// ── constructor ──────────────────────────────────────────────────────────────

AccountManagementDialog::AccountManagementDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Manage Broker Accounts"));
    setMinimumSize(700, 480);
    setStyleSheet(QString("QDialog { background: %1; color: %2; }"
                          "QLabel#fieldLabel { color: %3; font-size: 11px; font-weight: 700; }"
                          "QLabel#titleLabel { color: %4; font-size: 14px; font-weight: 700; }"
                          "QLabel#statusLabel { font-size: 11px; }"
                          "QLineEdit { background: %5; border: 1px solid %6; color: %2;"
                          "  padding: 6px; font-size: 12px; border-radius: 2px; }"
                          "QLineEdit:focus { border-color: %4; }"
                          "QComboBox { background: %5; border: 1px solid %6; color: %2;"
                          "  padding: 4px 6px; font-size: 12px; }"
                          "QComboBox::drop-down { border: none; }"
                          "QComboBox QAbstractItemView { background: %7; color: %2;"
                          "  selection-background-color: rgba(217,119,6,0.3); }"
                          "QPushButton { padding: 8px 16px; font-weight: 700; font-size: 12px; border-radius: 2px; }"
                          "QListWidget { background: %5; border: 1px solid %6; color: %2; font-size: 12px; }"
                          "QListWidget::item { padding: 6px 8px; }"
                          "QListWidget::item:selected { background: rgba(217,119,6,0.2); color: %4; }")
                      .arg(colors::BG_SURFACE(), colors::TEXT_PRIMARY(), colors::TEXT_SECONDARY(),
                           colors::AMBER(), colors::BG_BASE(), colors::BORDER_MED(), colors::BG_RAISED()));
    setup_ui();
    refresh_account_list();

    // Live-update the account list when a background validation sweep changes a
    // connection state (e.g. Connecting → Connected/TokenExpired) so an expired
    // token never lingers as green while the dialog is open.
    //
    // CRITICAL: update ONLY the affected row's dot + colour IN PLACE. Do NOT
    // clear()/rebuild the list and do NOT touch selection — a background sweep
    // (or a broker feed reconnecting) can fire this signal repeatedly, and a
    // full rebuild + force-reselect would fight the user's clicks and make it
    // impossible to switch to another broker's card.
    connect(&AccountManager::instance(), &AccountManager::connection_state_changed, this,
            [this](const QString& account_id, ConnectionState state) {
                for (int i = 0; i < account_list_->count(); ++i) {
                    auto* item = account_list_->item(i);
                    if (item->data(Qt::UserRole).toString() != account_id)
                        continue;
                    const auto account = AccountManager::instance().get_account(account_id);
                    auto* broker = BrokerRegistry::instance().get(account.broker_id);
                    const QString broker_name = broker ? broker->profile().display_name : account.broker_id;
                    const QString icon = (state == ConnectionState::Connected || state == ConnectionState::Error)
                                             ? QString::fromUtf8("\xe2\x97\x8f")  // ●
                                             : QString::fromUtf8("\xe2\x97\x8b"); // ○
                    item->setText(QString("%1 %2 [%3]").arg(icon, account.display_name, broker_name));
                    if (state == ConnectionState::Connected)
                        item->setForeground(QColor(colors::POSITIVE()));
                    else if (state == ConnectionState::Error || state == ConnectionState::TokenExpired)
                        item->setForeground(QColor(colors::NEGATIVE()));
                    else
                        item->setForeground(QColor(colors::TEXT_PRIMARY()));
                    break;
                }
            });
}

// ── UI setup ────────────────────────────────────────────────────────────────

void AccountManagementDialog::setup_ui() {
    auto* root = new QHBoxLayout(this);
    root->setSpacing(12);
    root->setContentsMargins(12, 12, 12, 12);

    // ── Left panel: account list + add/remove ──
    auto* left = new QVBoxLayout;
    left->setSpacing(6);

    list_label_ = new QLabel(tr("ACCOUNTS"));
    list_label_->setObjectName("fieldLabel");
    left->addWidget(list_label_);

    account_list_ = new QListWidget;
    connect(account_list_, &QListWidget::currentRowChanged, this, &AccountManagementDialog::on_account_selected);
    left->addWidget(account_list_, 1);

    // Add account controls
    auto* add_row = new QHBoxLayout;
    broker_picker_ = new QComboBox;
    const auto brokers = BrokerRegistry::instance().list_brokers();
    for (const auto& bid : brokers) {
        auto* b = BrokerRegistry::instance().get(bid);
        if (b)
            broker_picker_->addItem(b->profile().display_name, bid);
    }
    add_row->addWidget(broker_picker_, 1);

    display_name_input_ = new QLineEdit;
    display_name_input_->setPlaceholderText(tr("Account name..."));
    add_row->addWidget(display_name_input_, 1);
    left->addLayout(add_row);

    auto* btn_row = new QHBoxLayout;
    add_btn_ = new QPushButton(tr("+ ADD"));
    add_btn_->setStyleSheet(QString("QPushButton { background: %1; color: %2; }")
                                .arg(colors::AMBER(), colors::BG_BASE()));
    connect(add_btn_, &QPushButton::clicked, this, &AccountManagementDialog::on_add_account);
    btn_row->addWidget(add_btn_);

    remove_btn_ = new QPushButton(tr("REMOVE"));
    remove_btn_->setStyleSheet(QString("QPushButton { background: %1; color: %2; }")
                                   .arg(colors::NEGATIVE(), colors::TEXT_PRIMARY()));
    remove_btn_->setEnabled(false);
    connect(remove_btn_, &QPushButton::clicked, this, &AccountManagementDialog::on_remove_account);
    btn_row->addWidget(remove_btn_);
    left->addLayout(btn_row);

    root->addLayout(left, 1);

    // ── Right panel: credential form (stacked) ──
    right_stack_ = new QStackedWidget;

    // Empty page (no selection)
    empty_page_ = new QWidget(this);
    auto* empty_layout = new QVBoxLayout(empty_page_);
    empty_label_ = new QLabel(tr("Select an account to configure credentials"));
    empty_label_->setAlignment(Qt::AlignCenter);
    empty_label_->setStyleSheet(QString("color: %1; font-size: 12px;").arg(colors::TEXT_TERTIARY()));
    empty_layout->addWidget(empty_label_);
    right_stack_->addWidget(empty_page_);

    // Form page
    form_page_ = new QWidget(this);
    auto* form_layout = new QVBoxLayout(form_page_);
    form_layout->setSpacing(8);
    form_layout->setContentsMargins(0, 0, 0, 0);

    form_title_ = new QLabel;
    form_title_->setObjectName("titleLabel");
    form_layout->addWidget(form_title_);

    form_status_ = new QLabel;
    form_status_->setObjectName("statusLabel");
    form_layout->addWidget(form_status_);

    // Dynamic fields container
    auto* fields_widget = new QWidget(this);
    fields_layout_ = new QVBoxLayout(fields_widget);
    fields_layout_->setSpacing(4);
    fields_layout_->setContentsMargins(0, 0, 0, 0);
    form_layout->addWidget(fields_widget);

    form_layout->addStretch();

    // Action buttons
    auto* action_row = new QHBoxLayout;
    rename_btn_ = new QPushButton(tr("RENAME"));
    rename_btn_->setStyleSheet(QString("QPushButton { background: %1; color: %2; }")
                                   .arg(colors::BG_RAISED(), colors::TEXT_PRIMARY()));
    connect(rename_btn_, &QPushButton::clicked, this, &AccountManagementDialog::on_rename_account);
    action_row->addWidget(rename_btn_);

    connect_btn_ = new QPushButton(tr("CONNECT"));
    connect_btn_->setStyleSheet(QString("QPushButton { background: %1; color: %2; }")
                                    .arg(colors::AMBER(), colors::BG_BASE()));
    connect(connect_btn_, &QPushButton::clicked, this, &AccountManagementDialog::on_connect_account);
    action_row->addWidget(connect_btn_);
    form_layout->addLayout(action_row);

    right_stack_->addWidget(form_page_);

    // MT4 (MetaAPI)-specific page (populated on-demand by build_mt4_form())
    mt4_page_ = new QWidget(this);
    right_stack_->addWidget(mt4_page_);

    right_stack_->setCurrentWidget(empty_page_);

    // ── Right column: stacked pages + per-account approval footer ──
    auto* right_col = new QVBoxLayout;
    right_col->setContentsMargins(0, 0, 0, 0);
    right_col->setSpacing(8);
    right_col->addWidget(right_stack_, 1);

    auto* approval_row = new QHBoxLayout;
    approval_caption_ = new QLabel(tr("Order Approval:"));
    approval_caption_->setObjectName("fieldLabel");
    approval_row->addWidget(approval_caption_);
    approval_mode_combo_ = new QComboBox;
    approval_mode_combo_->addItem(tr("Auto — execute immediately"), "auto");
    approval_mode_combo_->addItem(tr("Semi-Auto — require approval"), "semi_auto");
    approval_mode_combo_->setEnabled(false); // enabled once an account is selected
    approval_row->addWidget(approval_mode_combo_, 1);
    right_col->addLayout(approval_row);

    // Persist immediately on change (independent of the async CONNECT flow).
    connect(approval_mode_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (selected_account_id_.isEmpty())
            return;
        ActionCenter::instance().set_order_mode(
            selected_account_id_, parse_order_mode(approval_mode_combo_->currentData().toString()));
    });

    root->addLayout(right_col, 2);
}

void AccountManagementDialog::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QDialog::changeEvent(event);
}

void AccountManagementDialog::retranslateUi() {
    setWindowTitle(tr("Manage Broker Accounts"));

    // Left panel chrome
    if (list_label_)          list_label_->setText(tr("ACCOUNTS"));
    if (display_name_input_)  display_name_input_->setPlaceholderText(tr("Account name..."));
    if (add_btn_)             add_btn_->setText(tr("+ ADD"));
    if (remove_btn_)          remove_btn_->setText(tr("REMOVE"));

    // Right panel — generic form chrome
    if (empty_label_)         empty_label_->setText(tr("Select an account to configure credentials"));
    if (rename_btn_)          rename_btn_->setText(tr("RENAME"));
    if (connect_btn_)         connect_btn_->setText(tr("CONNECT"));
    if (approval_caption_)    approval_caption_->setText(tr("Order Approval:"));
    if (approval_mode_combo_ && approval_mode_combo_->count() >= 2) {
        approval_mode_combo_->setItemText(0, tr("Auto — execute immediately"));
        approval_mode_combo_->setItemText(1, tr("Semi-Auto — require approval"));
    }

    // Per-broker form bottom buttons (built lazily — guarded). Their dynamic
    // status text and the account-driven titles are reapplied by
    // on_account_selected when the user re-selects an account, so we only
    // re-translate the persistent action buttons here.
    if (mt4_rename_btn_)      mt4_rename_btn_->setText(tr("RENAME"));
    if (mt4_connect_btn_)     mt4_connect_btn_->setText(tr("CONNECT MT4 ACCOUNT"));
}

// ── Account list ────────────────────────────────────────────────────────────

void AccountManagementDialog::refresh_account_list() {
    account_list_->clear();
    const auto accounts = AccountManager::instance().list_accounts();
    for (const auto& account : accounts) {
        auto* broker = BrokerRegistry::instance().get(account.broker_id);
        const QString broker_name = broker ? broker->profile().display_name : account.broker_id;
        const QString state_icon = account.state == ConnectionState::Connected ? QString::fromUtf8("\xe2\x97\x8f") // ●
                                 : account.state == ConnectionState::Error     ? QString::fromUtf8("\xe2\x97\x8f") // ●
                                                                               : QString::fromUtf8("\xe2\x97\x8b"); // ○
        const QString text = QString("%1 %2 [%3]").arg(state_icon, account.display_name, broker_name);
        auto* item = new QListWidgetItem(text);
        item->setData(Qt::UserRole, account.account_id);
        if (account.state == ConnectionState::Connected)
            item->setForeground(QColor(colors::POSITIVE()));
        else if (account.state == ConnectionState::Error || account.state == ConnectionState::TokenExpired)
            item->setForeground(QColor(colors::NEGATIVE()));
        account_list_->addItem(item);
    }
    remove_btn_->setEnabled(false);
}

void AccountManagementDialog::on_account_selected(int row) {
    if (row < 0) {
        right_stack_->setCurrentWidget(empty_page_);
        remove_btn_->setEnabled(false);
        if (approval_mode_combo_)
            approval_mode_combo_->setEnabled(false);
        selected_account_id_.clear();
        return;
    }

    auto* item = account_list_->item(row);
    selected_account_id_ = item->data(Qt::UserRole).toString();
    remove_btn_->setEnabled(true);

    // Reflect this account's persisted approval mode (applies to every broker).
    if (approval_mode_combo_) {
        const auto mode = ActionCenter::instance().get_order_mode(selected_account_id_);
        QSignalBlocker blk(approval_mode_combo_);
        approval_mode_combo_->setCurrentIndex(mode == OrderMode::SemiAuto ? 1 : 0);
        approval_mode_combo_->setEnabled(true);
    }

    auto account = AccountManager::instance().get_account(selected_account_id_);
    auto* broker = BrokerRegistry::instance().get(account.broker_id);
    if (!broker) {
        right_stack_->setCurrentWidget(empty_page_);
        return;
    }

    const auto prof = broker->profile();


    if (account.broker_id == QStringLiteral("metatrader4")) {
        build_mt4_form();
        const QString state_str = connection_state_str(account.state);
        if (mt4_title_)
            mt4_title_->setText(QString("%1 — %2 | %3").arg(account.display_name, prof.region, prof.currency));
        if (mt4_status_) {
            mt4_status_->setText(tr("Status: %1").arg(state_str));
            if (account.state == ConnectionState::Connected)
                mt4_status_->setStyleSheet(QString("color: %1;").arg(colors::POSITIVE()));
            else if (account.state == ConnectionState::Error || account.state == ConnectionState::TokenExpired)
                mt4_status_->setStyleSheet(QString("color: %1;").arg(colors::NEGATIVE()));
            else
                mt4_status_->setStyleSheet(QString("color: %1;").arg(colors::TEXT_SECONDARY()));
        }
        load_saved_credentials(selected_account_id_);
        right_stack_->setCurrentWidget(mt4_page_);
        return;
    }

    form_title_->setText(QString("%1 — %2 | %3").arg(account.display_name, prof.region, prof.currency));

    // Update status
    const QString state_str = connection_state_str(account.state);
    form_status_->setText(tr("Status: %1").arg(state_str));
    if (account.state == ConnectionState::Connected)
        form_status_->setStyleSheet(QString("color: %1;").arg(colors::POSITIVE()));
    else if (account.state == ConnectionState::Error || account.state == ConnectionState::TokenExpired)
        form_status_->setStyleSheet(QString("color: %1;").arg(colors::NEGATIVE()));
    else
        form_status_->setStyleSheet(QString("color: %1;").arg(colors::TEXT_SECONDARY()));

    build_credential_form(prof);
    load_saved_credentials(selected_account_id_);

    right_stack_->setCurrentWidget(form_page_);
}

// ── Credential form ─────────────────────────────────────────────────────────

void AccountManagementDialog::build_credential_form(const trading::BrokerProfile& profile) {
    // Clear existing fields
    for (auto* f : cred_fields_)
        f->deleteLater();
    cred_fields_.clear();
    cred_field_defs_ = profile.credential_fields;
    cred_form_keys_.clear();

    // Remove old widgets from layout
    while (fields_layout_->count() > 0) {
        auto* item = fields_layout_->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }

    // Brokers that pack several secrets into one exchange_token() arg get a custom
    // multi-sub-field form — one labelled box per real secret — instead of the
    // plain profile form (which would force the user to hand-type ":::"/"|||"
    // delimiters). on_connect_account packs the values back via pack_custom_credentials.
    const auto custom = custom_cred_fields(profile.id);
    if (!custom.isEmpty()) {
        for (const auto& sf : custom) {
            fields_layout_->addWidget(make_field_label(sf.label));
            auto* field = make_field(QString(), sf.secret);
            fields_layout_->addWidget(field);
            cred_fields_.append(field);
            cred_form_keys_.append(sf.key);
        }
        return;
    }

    // Build fields from BrokerProfile::credential_fields
    for (const auto& def : profile.credential_fields) {
        if (def.field == CredentialField::Environment)
            continue; // Environment is handled by per-account trading mode, not here
        fields_layout_->addWidget(make_field_label(def.label));
        auto* field = make_field(def.placeholder, def.secret);
        fields_layout_->addWidget(field);
        cred_fields_.append(field);
    }
}

void AccountManagementDialog::load_saved_credentials(const QString& account_id) {
    auto creds = AccountManager::instance().load_credentials(account_id);

    // MT4 (MetaAPI) branch — populate the dedicated form.
    if (creds.broker_id == QStringLiteral("metatrader4") && mt4_meta_token_) {
        mt4_meta_token_->setText(creds.api_key);
        mt4_login_->setText(creds.user_id);
        const auto extra = QJsonDocument::fromJson(creds.additional_data.toUtf8()).object();
        mt4_server_->setText(extra.value("server").toString());
        const QString region = extra.value("region").toString();
        if (mt4_region_) {
            for (int i = 0; i < mt4_region_->count(); ++i) {
                if (mt4_region_->itemData(i).toString() == region) {
                    mt4_region_->setCurrentIndex(i);
                    break;
                }
            }
        }
        const QString acct_type = extra.value("account_type").toString();
        if (mt4_account_type_)
            mt4_account_type_->setCurrentIndex(acct_type == "live" ? 1 : 0);
        return;
    }

    // Custom multi-sub-field forms (delimiter brokers) can't be cleanly unpacked
    // back into individual boxes — and they're daily-expiring TOTP logins anyway —
    // so leave them blank for re-entry rather than mis-populate from packed creds.
    if (!cred_form_keys_.isEmpty())
        return;

    int idx = 0;
    for (const auto& def : cred_field_defs_) {
        if (def.field == CredentialField::Environment)
            continue;
        if (idx >= cred_fields_.size())
            break;
        switch (def.field) {
        case CredentialField::ApiKey:
            cred_fields_[idx]->setText(creds.api_key);
            break;
        case CredentialField::ApiSecret:
            cred_fields_[idx]->setText(creds.api_secret);
            break;
        case CredentialField::AuthCode:
            // Auth codes are ephemeral — don't pre-fill
            break;
        case CredentialField::ClientCode: {
            // Parse from additional_data JSON
            auto doc = QJsonDocument::fromJson(creds.additional_data.toUtf8());
            if (doc.isObject())
                cred_fields_[idx]->setText(doc.object().value("client_code").toString());
            break;
        }
        default:
            break;
        }
        ++idx;
    }
}

// ── Actions ─────────────────────────────────────────────────────────────────

void AccountManagementDialog::on_add_account() {
    const QString broker_id = broker_picker_->currentData().toString();
    QString name = display_name_input_->text().trimmed();
    if (name.isEmpty()) {
        auto* broker = BrokerRegistry::instance().get(broker_id);
        name = broker ? broker->profile().display_name : broker_id;
        // Append count if multiple accounts for same broker
        const auto existing = AccountManager::instance().list_accounts(broker_id);
        if (!existing.isEmpty())
            name += QString(" #%1").arg(existing.size() + 1);
    }

    auto account = AccountManager::instance().add_account(broker_id, name);
    if (account.account_id.isEmpty()) {
        // AccountManager returns an empty account on persistence failure (e.g. FK
        // violation, paper-portfolio creation failed). Surface this instead of
        // silently pretending the account was added.
        QMessageBox::critical(this, tr("Add Account"),
                              tr("Could not save the account to the database. "
                                 "Check the application log for details."));
        return;
    }
    display_name_input_->clear();
    refresh_account_list();

    // Select the new account
    for (int i = 0; i < account_list_->count(); ++i) {
        if (account_list_->item(i)->data(Qt::UserRole).toString() == account.account_id) {
            account_list_->setCurrentRow(i);
            break;
        }
    }

    emit account_added(account.account_id);
}

void AccountManagementDialog::on_remove_account() {
    if (selected_account_id_.isEmpty())
        return;

    auto account = AccountManager::instance().get_account(selected_account_id_);
    auto reply = QMessageBox::warning(this, tr("Remove Account"),
                                      tr("Are you sure you want to remove \"%1\"?\n\n"
                                         "This will delete all saved credentials and the linked paper portfolio.\n"
                                         "This action cannot be undone.")
                                          .arg(account.display_name),
                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    const QString removed_id = selected_account_id_;
    selected_account_id_.clear();
    AccountManager::instance().remove_account(removed_id);
    refresh_account_list();
    right_stack_->setCurrentWidget(empty_page_);

    emit account_removed(removed_id);
}

void AccountManagementDialog::on_connect_account() {
    if (selected_account_id_.isEmpty())
        return;

    auto account = AccountManager::instance().get_account(selected_account_id_);
    auto* broker = BrokerRegistry::instance().get(account.broker_id);
    if (!broker)
        return;

    // Collect one raw value per declared credential field, then assemble the exact
    // exchange_token() arguments this broker expects. assemble_exchange_args()
    // centralises per-broker packing (e.g. a ClientCode field rides in
    // additional_data as JSON) so each broker receives the args it parses.
    BrokerCredentials creds;
    creds.broker_id = account.broker_id;
    ExchangeArgs ex;
    if (!cred_form_keys_.isEmpty()) {
        // Custom multi-sub-field form: collect values by key, pack per broker.
        QMap<QString, QString> kv;
        for (int i = 0; i < cred_form_keys_.size() && i < cred_fields_.size(); ++i)
            kv.insert(cred_form_keys_[i], cred_fields_[i]->text().trimmed());
        ex = pack_custom_credentials(account.broker_id, kv);
    } else {
        QMap<int, QString> field_values;
        int idx = 0;
        for (const auto& def : cred_field_defs_) {
            if (def.field == CredentialField::Environment)
                continue;
            if (idx >= cred_fields_.size())
                break;
            field_values.insert(static_cast<int>(def.field), cred_fields_[idx]->text().trimmed());
            ++idx;
        }
        ex = assemble_exchange_args(account.broker_id, field_values);
    }
    creds.api_key = ex.api_key;
    creds.api_secret = ex.api_secret;
    creds.additional_data = ex.additional_data;
    QString auth_code = ex.auth_code;

    // Attempt token exchange asynchronously — exchange_token() makes a blocking
    // HTTP call (BrokerHttp::execute uses QEventLoop), so running it on the UI
    // thread freezes the entire terminal until the request completes. Offload
    // to a worker thread and post results back via QMetaObject::invokeMethod.
    form_status_->setText(tr("Connecting..."));
    form_status_->setStyleSheet(QString("color: %1;").arg(colors::AMBER()));
    connect_btn_->setEnabled(false);

    const QString account_id = selected_account_id_;
    QPointer<AccountManagementDialog> self = this;
    const QString api_key_val = creds.api_key;
    const QString api_secret_val = creds.api_secret;
    const QString auth_code_val = auth_code;
    const QString existing_additional = creds.additional_data;

    (void)QtConcurrent::run([self, broker, account_id, api_key_val, api_secret_val, auth_code_val, existing_additional]() {
        auto result = broker->exchange_token(api_key_val, api_secret_val, auth_code_val);

        QMetaObject::invokeMethod(
            qApp,
            [self, account_id, api_key_val, api_secret_val, auth_code_val, existing_additional, result]() {
                // Always update AccountManager — it's thread-safe and survives dialog close
                if (!result.success) {
                    AccountManager::instance().set_connection_state(account_id, ConnectionState::Error, result.error);
                    if (self) {
                        self->form_status_->setText(self->tr("Error: %1").arg(result.error));
                        self->form_status_->setStyleSheet(QString("color: %1;").arg(colors::NEGATIVE()));
                        self->connect_btn_->setEnabled(true);
                        if (self->selected_account_id_ == account_id)
                            self->refresh_account_list();
                    }
                    return;
                }

                // Build merged credentials
                BrokerCredentials creds;
                creds.broker_id = AccountManager::instance().get_account(account_id).broker_id;
                creds.api_key = api_key_val;
                creds.api_secret = api_secret_val;
                creds.access_token = result.access_token;
                if (!result.refresh_token.isEmpty())
                    creds.refresh_token = result.refresh_token;
                if (!result.user_id.isEmpty())
                    creds.user_id = result.user_id;

                // Merge additional_data (keep existing like client_code, add new from token response)
                auto existing = QJsonDocument::fromJson(existing_additional.toUtf8()).object();
                auto extra = QJsonDocument::fromJson(result.additional_data.toUtf8()).object();
                for (auto it = extra.begin(); it != extra.end(); ++it)
                    existing[it.key()] = it.value();
                if (!existing.isEmpty())
                    creds.additional_data = QJsonDocument(existing).toJson(QJsonDocument::Compact);

                AccountManager::instance().save_credentials(account_id, creds);
                AccountManager::instance().set_connection_state(account_id, ConnectionState::Connected);

                // Emit signal so EquityTradingScreen starts the data stream, even if the
                // dialog has been closed in the meantime.
                if (self) {
                    emit self->credentials_saved(account_id);
                    self->form_status_->setText(self->tr("Connected as %1")
                                                     .arg(creds.user_id.isEmpty() ? creds.api_key.left(8) + "..."
                                                                                  : creds.user_id));
                    self->form_status_->setStyleSheet(QString("color: %1;").arg(colors::POSITIVE()));
                    self->connect_btn_->setEnabled(true);
                    if (self->selected_account_id_ == account_id)
                        self->refresh_account_list();
                }
            },
            Qt::QueuedConnection);
    });
}

void AccountManagementDialog::on_rename_account() {
    if (selected_account_id_.isEmpty()) {
        QMessageBox::information(this, tr("Rename Account"),
                                tr("Select an account first, then rename it."));
        return;
    }

    // Ask for the new name inline, seeded with the current one. The RENAME
    // buttons live on the right-hand broker forms — far from the left-panel
    // "Account name..." field, which is shared with + ADD. The old handler read
    // that field, so RENAME silently did nothing whenever it was empty (the
    // common case), making the button look dead. Prompting here makes every
    // RENAME button self-contained.
    const QString acct_id = selected_account_id_;
    const auto account = AccountManager::instance().get_account(acct_id);
    bool ok = false;
    const QString new_name =
        QInputDialog::getText(this, tr("Rename Account"), tr("New account name:"),
                              QLineEdit::Normal, account.display_name, &ok)
            .trimmed();
    if (!ok || new_name.isEmpty() || new_name == account.display_name)
        return;

    AccountManager::instance().update_display_name(acct_id, new_name);

    // refresh_account_list() clears the list, which emits currentRowChanged(-1)
    // and wipes selected_account_id_ — so re-select via the captured acct_id,
    // not the member. Re-selecting also refreshes the broker form's title with
    // the new name.
    refresh_account_list();
    for (int i = 0; i < account_list_->count(); ++i) {
        if (account_list_->item(i)->data(Qt::UserRole).toString() == acct_id) {
            account_list_->setCurrentRow(i);
            break;
        }
    }
}

// ── MT4 (MetaAPI)-specific form ──────────────────────────────────────────────

void AccountManagementDialog::build_mt4_form() {
    if (mt4_meta_token_ != nullptr)
        return;

    auto* outer = new QVBoxLayout(mt4_page_);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(mt4_page_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* container = new QWidget;
    scroll->setWidget(container);
    auto* v = new QVBoxLayout(container);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(10);

    mt4_title_ = new QLabel(QStringLiteral("MetaTrader 4"));
    mt4_title_->setObjectName("titleLabel");
    v->addWidget(mt4_title_);
    mt4_status_ = new QLabel(" ");
    mt4_status_->setObjectName("statusLabel");
    v->addWidget(mt4_status_);

    // Collapsible setup instructions
    mt4_setup_toggle_ = new QPushButton(
        QString::fromUtf8("\xe2\x96\xb8") + tr(" First-time setup (3 steps)"));
    mt4_setup_toggle_->setStyleSheet(QString("text-align:left;background:transparent;color:%1;"
                                             "border:none;padding:4px 0;font-weight:700;")
                                         .arg(colors::AMBER()));
    v->addWidget(mt4_setup_toggle_);

    mt4_setup_panel_ = new QWidget;
    auto* setup = new QVBoxLayout(mt4_setup_panel_);
    setup->setContentsMargins(12, 4, 12, 12);
    setup->setSpacing(4);
    auto add_step = [&](const QString& text) {
        auto* l = new QLabel(text);
        l->setWordWrap(true);
        l->setTextFormat(Qt::RichText);
        l->setOpenExternalLinks(true);
        l->setStyleSheet(QString("color:%1;font-size:11px;").arg(colors::TEXT_SECONDARY()));
        setup->addWidget(l);
    };
    add_step(tr("1. Create a free account at <a href='https://app.metaapi.cloud'>app.metaapi.cloud</a>."));
    add_step(tr("2. Generate an auth token from the API Access section."));
    add_step(tr("3. Enter your MT4 broker account credentials (login, password, server name)."));
    mt4_setup_panel_->setVisible(false);
    v->addWidget(mt4_setup_panel_);

    QPointer<AccountManagementDialog> self = this;
    connect(mt4_setup_toggle_, &QPushButton::clicked, this, [self]() {
        if (!self) return;
        const bool vis = !self->mt4_setup_panel_->isVisible();
        self->mt4_setup_panel_->setVisible(vis);
        self->mt4_setup_toggle_->setText(
            (vis ? QString::fromUtf8("\xe2\x96\xbe") : QString::fromUtf8("\xe2\x96\xb8")) +
            self->tr(" First-time setup (3 steps)"));
    });

    auto add_labeled_field = [&](QBoxLayout* lay, const QString& label, QLineEdit*& out,
                                 const QString& placeholder, const QString& hint, bool password) {
        auto* hdr = make_field_label(label);
        lay->addWidget(hdr);
        out = make_field(placeholder, password);
        lay->addWidget(out);
        if (!hint.isEmpty()) {
            auto* h = new QLabel(hint);
            h->setStyleSheet(QString("color:%1;font-size:10px;margin-bottom:4px;")
                                 .arg(colors::TEXT_SECONDARY()));
            lay->addWidget(h);
        }
    };

    // MetaAPI Token + "Get Token" link
    add_labeled_field(v, tr("METAAPI TOKEN"), mt4_meta_token_, tr("Enter MetaAPI auth token..."), QString(), true);
    auto* token_link = new QLabel(
        tr("<a href='https://app.metaapi.cloud/api-access/generate-token' "
           "style='color:%1;font-size:10px;'>Get Token \xe2\x86\x92</a>")
            .arg(colors::AMBER()));
    token_link->setTextFormat(Qt::RichText);
    token_link->setOpenExternalLinks(true);
    v->addWidget(token_link);

    // MT4 credentials
    add_labeled_field(v, tr("MT4 LOGIN"), mt4_login_, tr("Account number e.g. 12345678"),
                      tr("your MT4 broker account number"), false);
    add_labeled_field(v, tr("MT4 PASSWORD"), mt4_password_, tr("Trading password"),
                      tr("trading password (not investor password)"), true);
    add_labeled_field(v, tr("SERVER NAME"), mt4_server_, tr("e.g. ICMarkets-Demo03"),
                      tr("exact server name from your MT4 broker"), false);

    // Region combo
    v->addWidget(make_field_label(tr("REGION")));
    mt4_region_ = new QComboBox;
    mt4_region_->addItem(tr("New York"), "new-york-server");
    mt4_region_->addItem(tr("London"), "london-server");
    mt4_region_->addItem(tr("Singapore"), "singapore-server");
    v->addWidget(mt4_region_);

    // Account type combo
    v->addWidget(make_field_label(tr("ACCOUNT TYPE")));
    mt4_account_type_ = new QComboBox;
    mt4_account_type_->addItem(tr("Demo"), "demo");
    mt4_account_type_->addItem(tr("Live"), "live");
    v->addWidget(mt4_account_type_);

    // Connect button
    mt4_connect_btn_ = new QPushButton(tr("CONNECT MT4 ACCOUNT"));
    mt4_connect_btn_->setStyleSheet(
        QString("QPushButton { background: %1; color: %2; padding: 10px; font-weight: 700; }")
            .arg(colors::AMBER(), colors::BG_BASE()));
    v->addWidget(mt4_connect_btn_);

    v->addStretch();

    // Bottom action row
    auto* btn_row = new QHBoxLayout;
    mt4_rename_btn_ = new QPushButton(tr("RENAME"));
    mt4_rename_btn_->setStyleSheet(QString("QPushButton { background: %1; color: %2; }")
                                       .arg(colors::BG_RAISED(), colors::TEXT_PRIMARY()));
    btn_row->addWidget(mt4_rename_btn_);
    btn_row->addStretch();
    v->addLayout(btn_row);

    // Wire signals
    connect(mt4_connect_btn_, &QPushButton::clicked, this, &AccountManagementDialog::on_connect_mt4);
    connect(mt4_rename_btn_, &QPushButton::clicked, this, &AccountManagementDialog::on_rename_account);
}

void AccountManagementDialog::on_connect_mt4() {
    if (selected_account_id_.isEmpty())
        return;

    const QString meta_token = mt4_meta_token_->text().trimmed();
    const QString mt4_login = mt4_login_->text().trimmed();
    const QString mt4_password = mt4_password_->text();
    const QString mt4_server = mt4_server_->text().trimmed();
    const QString region = mt4_region_->currentData().toString();
    const QString account_type = mt4_account_type_->currentData().toString();

    if (meta_token.isEmpty() || mt4_login.isEmpty() || mt4_password.isEmpty() || mt4_server.isEmpty()) {
        mt4_status_->setText(tr("All fields are required."));
        mt4_status_->setStyleSheet(QString("color:%1;").arg(colors::NEGATIVE()));
        return;
    }

    // Persist credentials before provisioning so they survive dialog close
    BrokerCredentials creds;
    creds.broker_id = "metatrader4";
    creds.api_key = meta_token;
    creds.user_id = mt4_login;
    creds.additional_data = QString::fromUtf8(QJsonDocument(QJsonObject{
        {"region", region},
        {"server", mt4_server},
        {"platform", "mt4"},
        {"account_type", account_type},
    }).toJson(QJsonDocument::Compact));
    AccountManager::instance().save_credentials(selected_account_id_, creds);

    mt4_provision_async(meta_token, mt4_login, mt4_password, mt4_server, region);
}

void AccountManagementDialog::mt4_provision_async(const QString& meta_token, const QString& mt4_login,
                                                   const QString& mt4_password, const QString& mt4_server,
                                                   const QString& region) {
    mt4_connect_btn_->setEnabled(false);
    mt4_status_->setText(tr("Creating MT4 bridge..."));
    mt4_status_->setStyleSheet(QString("color:%1;").arg(colors::AMBER()));

    const QString acct = selected_account_id_;
    QPointer<AccountManagementDialog> self = this;

    const QString auth_code_json = QString::fromUtf8(QJsonDocument(QJsonObject{
        {"login", mt4_login},
        {"password", mt4_password},
        {"server", mt4_server},
        {"region", region},
    }).toJson(QJsonDocument::Compact));

    (void)QtConcurrent::run([self, acct, meta_token, auth_code_json, region]() {
        trading::MetaApiBroker broker;
        auto result = broker.exchange_token(meta_token, QString(), auth_code_json);

        QMetaObject::invokeMethod(qApp, [self, acct, meta_token, result, region]() {
            if (!self) return;
            auto& am = AccountManager::instance();

            if (result.success) {
                auto creds = am.load_credentials(acct);
                creds.access_token = result.access_token;
                creds.user_id = result.user_id;
                if (!result.additional_data.isEmpty())
                    creds.additional_data = result.additional_data;
                am.save_credentials(acct, creds);
                am.set_connection_state(acct, ConnectionState::Connected);
                self->mt4_status_->setText(self->tr("Connected"));
                self->mt4_status_->setStyleSheet(QString("color:%1;").arg(colors::POSITIVE()));
                emit self->credentials_saved(acct);
            } else {
                am.set_connection_state(acct, ConnectionState::Error, result.error);
                self->mt4_status_->setText(result.error);
                self->mt4_status_->setStyleSheet(QString("color:%1;").arg(colors::NEGATIVE()));
            }
            self->mt4_connect_btn_->setEnabled(true);
            self->refresh_account_list();
        }, Qt::QueuedConnection);
    });
}

} // namespace openmarketterminal::screens::equity
