#pragma once

#include <QJsonObject>
#include <QWidget>

class QLabel;
class QTimer;

namespace openmarketterminal::services::edge_radar {
struct EdgeProofStats;
}

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
    void refresh_proof();

  private:
    // One rendered line of the paper-proof scoreboard (active symbol / all).
    struct ProofRow {
        QLabel* scope = nullptr;
        QLabel* verdict = nullptr;
        QLabel* sample = nullptr;
        QLabel* pnl = nullptr;
        QLabel* buy_rate = nullptr;
        QLabel* no_trade_rate = nullptr;
    };

    static QJsonObject read_json(const QString& path);
    static QString age_text(const QString& iso_time);
    static QString bps_text(double bps);
    void set_metric(QLabel* value, QLabel* caption, const QString& text, const QString& color);
    void render_sources(const QJsonObject& decision);
    ProofRow make_proof_row(class QGridLayout* grid, int grid_row);
    void clear_proof_row(ProofRow& row, const QString& scope, const QString& note);
    void render_proof_row(ProofRow& row, const QString& scope,
                          const services::edge_radar::EdgeProofStats& stats);

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
    ProofRow proof_symbol_row_;
    ProofRow proof_all_row_;
    QLabel* proof_status_ = nullptr;
    QTimer* refresh_timer_ = nullptr;
    QTimer* proof_timer_ = nullptr;
    QString exchange_id_ = QStringLiteral("coinbase");
    QString active_symbol_;
    bool is_paper_ = true;
};

} // namespace openmarketterminal::screens::crypto
