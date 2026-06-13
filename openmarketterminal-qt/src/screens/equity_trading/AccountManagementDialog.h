#pragma once
// AccountManagementDialog — multi-account management dialog for equity trading.
// Allows adding, removing, renaming, and connecting broker accounts.
// Replaces the single-account EquityCredentials dialog.

#include "trading/ActionCenter.h"
#include "trading/BrokerAccount.h"
#include "trading/BrokerInterface.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace openmarketterminal::trading::auth {
class RedirectServer;
}

namespace openmarketterminal::screens::equity {

class AccountManagementDialog : public QDialog {
    Q_OBJECT
  public:
    explicit AccountManagementDialog(QWidget* parent = nullptr);

  signals:
    void account_added(const QString& account_id);
    void account_removed(const QString& account_id);
    void credentials_saved(const QString& account_id);

  protected:
    void changeEvent(QEvent* event) override;

  private:
    void setup_ui();
    void retranslateUi();
    void refresh_account_list();
    void on_account_selected(int row);
    void on_add_account();
    void on_remove_account();
    void on_connect_account();
    void on_rename_account();
    void build_credential_form(const trading::BrokerProfile& profile);
    void load_saved_credentials(const QString& account_id);

    // MT4 (MetaAPI)-specific form + handlers
    void build_mt4_form();
    void on_connect_mt4();
    void mt4_provision_async(const QString& meta_token, const QString& mt4_login,
                              const QString& mt4_password, const QString& mt4_server,
                              const QString& region);

    // Left panel — account list
    QLabel* list_label_ = nullptr;
    QListWidget* account_list_ = nullptr;
    QPushButton* add_btn_ = nullptr;
    QPushButton* remove_btn_ = nullptr;
    QComboBox* broker_picker_ = nullptr;
    QLineEdit* display_name_input_ = nullptr;
    QLabel* empty_label_ = nullptr;

    // Right panel — credential form
    QStackedWidget* right_stack_ = nullptr;
    QWidget* empty_page_ = nullptr;
    QWidget* form_page_ = nullptr;
    QLabel* form_title_ = nullptr;
    QLabel* form_status_ = nullptr;
    QVBoxLayout* fields_layout_ = nullptr;
    QPushButton* connect_btn_ = nullptr;
    QPushButton* rename_btn_ = nullptr;

    // Per-account order-approval mode (Auto / Semi-Auto). Lives in a footer
    // below the stacked credential pages so it applies to every broker,
    // including the dedicated MT4 form.
    QLabel* approval_caption_ = nullptr;
    QComboBox* approval_mode_combo_ = nullptr;

    // Dynamic credential fields (rebuilt per broker profile)
    QVector<QLineEdit*> cred_fields_;
    QVector<trading::CredentialFieldDef> cred_field_defs_;
    // Non-empty when the focused broker uses a custom multi-sub-field form whose
    // exchange_token() packs several secrets into one arg. Holds the per-field
    // keys, row-aligned with cred_fields_, used to pack the values back together.
    // No currently-supported broker uses this (see custom_cred_fields()).
    QStringList cred_form_keys_;

    // MT4 (MetaAPI)-specific widgets
    QWidget*     mt4_page_ = nullptr;
    QLabel*      mt4_title_ = nullptr;
    QLabel*      mt4_status_ = nullptr;
    QPushButton* mt4_setup_toggle_ = nullptr;
    QWidget*     mt4_setup_panel_ = nullptr;
    QLineEdit*   mt4_meta_token_ = nullptr;
    QLineEdit*   mt4_login_ = nullptr;
    QLineEdit*   mt4_password_ = nullptr;
    QLineEdit*   mt4_server_ = nullptr;
    QComboBox*   mt4_region_ = nullptr;
    QComboBox*   mt4_account_type_ = nullptr;
    QPushButton* mt4_connect_btn_ = nullptr;
    QPushButton* mt4_rename_btn_ = nullptr;

    // Currently selected account
    QString selected_account_id_;
};

} // namespace openmarketterminal::screens::equity
