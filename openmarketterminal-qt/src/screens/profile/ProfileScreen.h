#pragma once
#include <QComboBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>

namespace openmarketterminal::screens {

/// Profile & Account — local identity and AI provider access.
class ProfileScreen : public QWidget {
    Q_OBJECT
  public:
    explicit ProfileScreen(QWidget* parent = nullptr);

  protected:
    void changeEvent(QEvent* event) override;

  private:
    QStackedWidget* sections_ = nullptr;
    QList<QPushButton*> nav_buttons_;
    /// Source-language keys for each nav button, aligned by index with
    /// nav_buttons_. Used by retranslateUi() to reapply tr() lookups.
    QList<QString>      nav_source_keys_;
    QLabel*             header_title_ = nullptr;

    /// Re-translate widgets we keep handles to, and rebuild the section
    /// widgets so tr() calls inside their build_* helpers re-run against the
    /// new translator.
    void retranslateUi();
    void rebuild_sections();

    // Header labels
    QLabel* username_header_ = nullptr;

    // Local profile
    QLineEdit* profile_name_ = nullptr;
    QLineEdit* profile_nickname_ = nullptr;
    QLineEdit* profile_email_ = nullptr;
    QLineEdit* profile_phone_ = nullptr;
    QLineEdit* profile_country_ = nullptr;
    QLabel* profile_status_ = nullptr;

    // AI accounts
    QLineEdit* ai_openai_key_ = nullptr;
    QComboBox* ai_openai_model_ = nullptr;
    QLineEdit* ai_openai_base_url_ = nullptr;
    QLabel* ai_openai_status_ = nullptr;
    QLineEdit* ai_anthropic_key_ = nullptr;
    QComboBox* ai_anthropic_model_ = nullptr;
    QLineEdit* ai_anthropic_base_url_ = nullptr;
    QLabel* ai_anthropic_status_ = nullptr;
    QLineEdit* ai_ollama_base_url_ = nullptr;
    QComboBox* ai_ollama_model_ = nullptr;
    QLabel* ai_ollama_status_ = nullptr;

    // Local daemon / automation
    QLabel* daemon_status_ = nullptr;
    QLabel* daemon_installed_ = nullptr;
    QLabel* daemon_owner_ = nullptr;
    QLabel* daemon_running_ = nullptr;
    QLabel* daemon_jobs_summary_ = nullptr;
    QLabel* daemon_collectors_summary_ = nullptr;
    QLabel* daemon_collectors_status_ = nullptr;
    QLabel* daemon_simple_automation_status_ = nullptr;
    QLabel* daemon_live_bot_status_ = nullptr;
    QLabel* daemon_audit_status_ = nullptr;
    QLabel* daemon_action_status_ = nullptr;
    QPlainTextEdit* daemon_logs_ = nullptr;
    QPlainTextEdit* daemon_job_detail_ = nullptr;
    QTableWidget* daemon_jobs_table_ = nullptr;
    QTableWidget* daemon_collectors_table_ = nullptr;
    QComboBox* daemon_job_kind_ = nullptr;
    QLineEdit* daemon_job_target_ = nullptr;
    QSpinBox* daemon_job_interval_ = nullptr;

    void build_header(QVBoxLayout* root);
    void build_tab_nav(QVBoxLayout* root);
    QWidget* build_overview();
    QWidget* build_ai_accounts();
    QWidget* build_security();
    QWidget* build_automation();

    void refresh_all();
    void refresh_ai_accounts();
    void refresh_daemon();

    // Helpers
    QWidget* make_panel(const QString& title);
    QWidget* make_data_row(const QString& label, QLabel*& value_out);
    QWidget* make_stat_box(const QString& label, QLabel*& value_out, const QString& color);
    QString daemon_cli_path() const;
    void run_cli_command(const QStringList& args,
                         std::function<void(const QJsonObject&, const QString&)> on_success,
                         std::function<void(const QString&)> on_error = {});
    void run_daemon_cli(const QStringList& daemon_args,
                        std::function<void(const QJsonObject&, const QString&)> on_success,
                        std::function<void(const QString&)> on_error = {});
    void run_daemon_action(const QString& action, const QStringList& extra_args = {});
    void populate_daemon_health(const QJsonObject& health);
    void populate_daemon_jobs(const QJsonArray& jobs);
    void populate_daemon_collectors(const QJsonObject& collectors);
    void populate_daemon_simple_automation(const QJsonObject& automation);
    void populate_live_bot_status(const QJsonObject& status);
    void populate_daemon_audit(const QJsonObject& audit);
    void populate_daemon_logs(const QJsonObject& logs);
    void populate_daemon_job_detail(const QJsonObject& job);
    void refresh_selected_daemon_job_detail();
    QString selected_daemon_job_id() const;
    void run_selected_daemon_job_action(const QString& action);
    void apply_daemon_job_preset(const QString& kind, const QString& target, int interval_sec);

    void save_local_profile();
    void save_ai_provider(const QString& provider);
    void add_daemon_job();

  private slots:
    void on_section_changed(int index);
};

} // namespace openmarketterminal::screens
