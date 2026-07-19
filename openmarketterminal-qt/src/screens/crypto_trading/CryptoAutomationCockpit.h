#pragma once

#include <QJsonObject>
#include <QWidget>

class QLabel;
class QTimer;

namespace openmarketterminal::screens::crypto {

/// Read-only operational view of the selected crypto venue plus the local
/// spot/scalp decision engine. Trading authority remains in the existing order
/// entry and global execution guard.
class CryptoAutomationCockpit : public QWidget {
    Q_OBJECT
  public:
    explicit CryptoAutomationCockpit(QWidget* parent = nullptr);

    void set_exchange_context(const QString& exchange_id, bool is_paper);

  signals:
    void positions_requested();
    void orders_requested();

  private slots:
    void refresh();

  private:
    static QJsonObject read_json(const QString& path);
    static QString age_text(const QString& iso_time);
    static QString bps_text(double bps);
    void set_metric(QLabel* value, QLabel* caption, const QString& text, const QString& color);
    void render_sources(const QJsonObject& decision);

    QLabel* venue_value_ = nullptr;
    QLabel* venue_caption_ = nullptr;
    QLabel* engine_value_ = nullptr;
    QLabel* engine_caption_ = nullptr;
    QLabel* guard_value_ = nullptr;
    QLabel* guard_caption_ = nullptr;
    QLabel* hurdle_value_ = nullptr;
    QLabel* hurdle_caption_ = nullptr;
    QLabel* heartbeat_value_ = nullptr;
    QLabel* verdict_value_ = nullptr;
    QLabel* direction_value_ = nullptr;
    QLabel* price_value_ = nullptr;
    QLabel* edge_value_ = nullptr;
    QLabel* blockers_value_ = nullptr;
    QLabel* sources_value_ = nullptr;
    QTimer* refresh_timer_ = nullptr;
    QString exchange_id_ = QStringLiteral("coinbase");
    bool is_paper_ = true;
};

} // namespace openmarketterminal::screens::crypto
