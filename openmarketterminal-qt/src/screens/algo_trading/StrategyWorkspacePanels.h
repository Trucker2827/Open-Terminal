#pragma once

#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;
class QTabWidget;

namespace openmarketterminal::screens {

class StrategyAutomationPanel;

/// Paper handler controls backed by the real strategy runner and append-only AI ledger.
class StrategyHandlersPanel final : public QWidget {
    Q_OBJECT
  public:
    explicit StrategyHandlersPanel(QWidget* parent = nullptr);

  public slots:
    void refresh();

  private:
    void build_ui();
    void run_once();
    void run_cli(const QStringList& args);
    QString cli_path() const;
    void set_activity(const QString& text, const QString& color);

    QComboBox* handler_combo_ = nullptr;
    QLineEdit* symbols_edit_ = nullptr;
    QSpinBox* interval_spin_ = nullptr;
    QDoubleSpinBox* aggregate_cap_spin_ = nullptr;
    QCheckBox* floor_check_ = nullptr;
    QPushButton* run_button_ = nullptr;
    QLabel* activity_label_ = nullptr;
    QLabel* handler_count_ = nullptr;
    QLabel* fill_count_ = nullptr;
    QLabel* open_count_ = nullptr;
    QLabel* realized_total_ = nullptr;
    QTableWidget* table_ = nullptr;
};

/// Human-owned risk constitution plus read-only decision blockers and daemon health.
class StrategyRiskPanel final : public QWidget {
    Q_OBJECT
  public:
    explicit StrategyRiskPanel(QWidget* parent = nullptr);

  public slots:
    void refresh();
    void focus_decision_envelopes();

  private:
    void build_ui();
    void refresh_daemon();
    void restart_daemon();
    void engage_kill_switch();
    void set_gate(QLabel* value, const QString& text, const QString& color);
    QString cli_path() const;

    QLabel* kill_value_ = nullptr;
    QLabel* paper_value_ = nullptr;
    QLabel* live_value_ = nullptr;
    QLabel* fast_value_ = nullptr;
    QLabel* account_value_ = nullptr;
    QLabel* venues_value_ = nullptr;
    QLabel* daemon_value_ = nullptr;
    QLabel* daemon_detail_ = nullptr;
    QLabel* status_label_ = nullptr;
    QTableWidget* blockers_table_ = nullptr;
    QPushButton* restart_button_ = nullptr;
};

/// Append-only fill audit and the daemon workflows that generated strategy evidence.
class StrategyRunHistoryPanel final : public QWidget {
    Q_OBJECT
  public:
    explicit StrategyRunHistoryPanel(QWidget* parent = nullptr);

  public slots:
    void refresh();
    void focus_outcomes();

  private:
    void build_ui();
    void refresh_fills();
    void refresh_outcomes();

    QTabWidget* tabs_ = nullptr;
    QTableWidget* fills_table_ = nullptr;
    QLabel* fill_count_ = nullptr;
    QLabel* handler_count_ = nullptr;
    QLabel* open_count_ = nullptr;
    QLabel* realized_total_ = nullptr;
    QLabel* detail_label_ = nullptr;
    QTableWidget* outcomes_table_ = nullptr;
    QLabel* outcomes_summary_ = nullptr;
    StrategyAutomationPanel* automation_ = nullptr;
    QTimer refresh_timer_;
};

} // namespace openmarketterminal::screens
