#pragma once

#include <QJsonObject>
#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;

namespace openmarketterminal::screens::crypto {

/// Read-only operating view for the local Coinbase spot/scalp decision engine.
/// It reads the daemon's persisted state, the same state consumed by the CLI,
/// so the screen cannot imply that a live strategy is armed when it is not.
class CryptoAutomationCockpit : public QWidget {
    Q_OBJECT
  public:
    explicit CryptoAutomationCockpit(QWidget* parent = nullptr);

  signals:
    void positions_requested();
    void orders_requested();

  private slots:
    void refresh();

  private:
    static QJsonObject read_json(const QString& path);
    static QString age_text(const QString& iso_time);
    static QString money_bps(double bps);
    static void set_metric(QLabel* value, QLabel* label, const QString& text, const QString& tone = {});
    void render_sources(const QJsonObject& decision);

    QLabel* engine_value_ = nullptr;
    QLabel* engine_label_ = nullptr;
    QLabel* guard_value_ = nullptr;
    QLabel* guard_label_ = nullptr;
    QLabel* cadence_value_ = nullptr;
    QLabel* cadence_label_ = nullptr;
    QLabel* fee_value_ = nullptr;
    QLabel* fee_label_ = nullptr;

    QLabel* verdict_value_ = nullptr;
    QLabel* direction_value_ = nullptr;
    QLabel* price_value_ = nullptr;
    QLabel* hurdle_value_ = nullptr;
    QLabel* edge_value_ = nullptr;
    QLabel* blockers_value_ = nullptr;
    QLabel* sources_value_ = nullptr;
    QLabel* heartbeat_value_ = nullptr;
    QPushButton* refresh_btn_ = nullptr;
    QTimer* refresh_timer_ = nullptr;
};

} // namespace openmarketterminal::screens::crypto
