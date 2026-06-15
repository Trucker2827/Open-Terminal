#pragma once
// SecuritySection.h — PIN status, change PIN form, auto-lock policy,
// and security audit log display.

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QWidget>

namespace openmarketterminal::screens {

class SecuritySection : public QWidget {
    Q_OBJECT
  public:
    explicit SecuritySection(QWidget* parent = nullptr);

    /// Refresh PIN status, lockout counter, auto-lock toggles, and audit log.
    void reload();

  protected:
    void showEvent(QShowEvent* e) override;
    void changeEvent(QEvent* event) override;

  private:
    void build_ui();
    void refresh_audit_log();

    /// Re-apply tr() lookups to every widget whose text we keep a handle to.
    /// Called from changeEvent() on QEvent::LanguageChange.
    void retranslateUi();

    QLabel*      sec_pin_status_       = nullptr;
    QComboBox*   sec_lock_timeout_     = nullptr;
    QCheckBox*   sec_autolock_toggle_  = nullptr;
    QCheckBox*   sec_lock_on_minimize_ = nullptr;

    // CLI capability gates (default off; persisted as cli.allow_*).
    QCheckBox*   cli_settings_write_toggle_ = nullptr;
    QCheckBox*   cli_trading_toggle_        = nullptr;
    QCheckBox*   cli_paper_trading_toggle_  = nullptr;
    QCheckBox*   cli_live_trading_toggle_   = nullptr;
    QCheckBox*   cli_fast_live_toggle_      = nullptr;

    // AI-trading constitution (Phase B): allowed venues + per-topic exposure cap.
    QLineEdit*   cli_allowed_venues_edit_   = nullptr;
    QLineEdit*   cli_max_exposure_edit_     = nullptr;

    // AI-trading constitution (Phase C): kill switch + allowed account + daily-loss cap.
    QCheckBox*   cli_kill_switch_toggle_    = nullptr;
    QLineEdit*   cli_allowed_account_edit_  = nullptr;
    QLineEdit*   cli_max_daily_loss_edit_   = nullptr;
    QListWidget* sec_audit_list_       = nullptr;
    QLabel*      sec_lockout_status_   = nullptr;
    QPushButton* sec_change_pin_btn_   = nullptr;

    // Change PIN form (shown/hidden dynamically)
    QWidget*   sec_change_pin_form_ = nullptr;
    QLineEdit* sec_current_pin_    = nullptr;
    QLineEdit* sec_new_pin_        = nullptr;
    QLineEdit* sec_confirm_pin_    = nullptr;
    QLabel*    sec_pin_error_      = nullptr;
    QLabel*    sec_pin_success_    = nullptr;

    // Section titles, buttons, and row labels (cached for retranslateUi).
    QLabel* title_pin_    = nullptr;
    QLabel* title_change_ = nullptr;
    QLabel* title_lock_   = nullptr;
    QLabel* title_cli_    = nullptr;
    QLabel* title_audit_  = nullptr;
    QLabel* audit_note_   = nullptr;
    QPushButton* save_pin_btn_      = nullptr;
    QPushButton* save_btn_          = nullptr;
    QPushButton* refresh_audit_btn_ = nullptr;

    QLabel* row_pin_status_lbl_  = nullptr;  QLabel* row_pin_status_desc_  = nullptr;
    QLabel* row_attempts_lbl_    = nullptr;  QLabel* row_attempts_desc_    = nullptr;
    QLabel* row_current_lbl_     = nullptr;
    QLabel* row_new_lbl_         = nullptr;
    QLabel* row_confirm_lbl_     = nullptr;
    QLabel* row_autolock_lbl_    = nullptr;  QLabel* row_autolock_desc_    = nullptr;
    QLabel* row_timeout_lbl_     = nullptr;  QLabel* row_timeout_desc_     = nullptr;
    QLabel* row_minimize_lbl_    = nullptr;  QLabel* row_minimize_desc_    = nullptr;
    QLabel* row_cli_write_lbl_   = nullptr;  QLabel* row_cli_write_desc_   = nullptr;
    QLabel* row_cli_trade_lbl_   = nullptr;  QLabel* row_cli_trade_desc_   = nullptr;
    QLabel* row_cli_paper_lbl_   = nullptr;  QLabel* row_cli_paper_desc_   = nullptr;
    QLabel* row_cli_live_lbl_    = nullptr;  QLabel* row_cli_live_desc_    = nullptr;
    QLabel* row_cli_fast_lbl_    = nullptr;  QLabel* row_cli_fast_desc_    = nullptr;
    QLabel* row_cli_venues_lbl_  = nullptr;  QLabel* row_cli_venues_desc_  = nullptr;
    QLabel* row_cli_expo_lbl_    = nullptr;  QLabel* row_cli_expo_desc_    = nullptr;
    QLabel* row_cli_kill_lbl_    = nullptr;  QLabel* row_cli_kill_desc_    = nullptr;
    QLabel* row_cli_acct_lbl_    = nullptr;  QLabel* row_cli_acct_desc_    = nullptr;
    QLabel* row_cli_dloss_lbl_   = nullptr;  QLabel* row_cli_dloss_desc_   = nullptr;
};

} // namespace openmarketterminal::screens
