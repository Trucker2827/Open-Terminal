#pragma once

#include "screens/crypto_trading/CryptoCockpitPresentation.h"

#include <QJsonObject>
#include <QWidget>

class QLabel;
class QTimer;
class QListWidget;

namespace openmarketterminal::screens::crypto {

/// Read-only operational view of the selected crypto venue plus the local
/// spot/scalp decision engine. Binds a pure `CryptoCockpitScene`. Trading
/// authority remains in order entry + Profile canary + Settings Security.
class CryptoAutomationCockpit : public QWidget {
    Q_OBJECT
  public:
    explicit CryptoAutomationCockpit(QWidget* parent = nullptr);

    void set_exchange_context(const QString& exchange_id, bool is_paper);
    void set_active_symbol(const QString& symbol);

  signals:
    void positions_requested();
    void orders_requested();

  private slots:
    void refresh();

  public: // UI construction helper; contains widget pointers, no trading authority.
    struct LaneCard {
        QLabel* eyebrow = nullptr;
        QLabel* title = nullptr;
        QLabel* state = nullptr;
        QLabel* authority = nullptr;
        QLabel* decision = nullptr;
        QLabel* edge = nullptr;
        QLabel* sample = nullptr;
        QLabel* ledger = nullptr;
        QLabel* detail = nullptr;
    };
  private:
    struct ProofRow {
        QLabel* scope = nullptr;
        QLabel* verdict = nullptr;
        QLabel* sample = nullptr;
        QLabel* mean_net = nullptr;
        QLabel* win_rate = nullptr;
        QLabel* coverage = nullptr;
    };

    static QJsonObject read_json(const QString& path);
    static qint64 file_age_ms(const QString& path, qint64 now_ms);
    CryptoCockpitInputs build_inputs(qint64 now_ms) const;
    void bind_scene(const CryptoCockpitScene& scene);
    void set_metric(QLabel* value, QLabel* caption, const QString& text, const QString& color);
    ProofRow make_proof_row(class QGridLayout* grid, int grid_row);
    void render_proof_row(ProofRow& row, const CryptoCockpitProofRow& proof);
    void render_lane(LaneCard& card, const CryptoCockpitLaneScene& lane);

    QLabel* mood_value_ = nullptr;
    QLabel* mood_detail_ = nullptr;
    LaneCard spot_lane_;
    LaneCard scalp_lane_;
    QLabel* venue_value_ = nullptr;
    QLabel* venue_caption_ = nullptr;
    QLabel* engine_value_ = nullptr;
    QLabel* engine_caption_ = nullptr;
    QLabel* guard_value_ = nullptr;
    QLabel* guard_caption_ = nullptr;
    QLabel* hurdle_value_ = nullptr;
    QLabel* hurdle_caption_ = nullptr;
    QLabel* heartbeat_value_ = nullptr;
    QLabel* decide_title_ = nullptr;
    QLabel* symbol_value_ = nullptr;
    QLabel* verdict_value_ = nullptr;
    QLabel* direction_value_ = nullptr;
    QLabel* liquidity_value_ = nullptr;
    QLabel* price_value_ = nullptr;
    QLabel* required_value_ = nullptr;
    QLabel* edge_value_ = nullptr;
    QLabel* blockers_value_ = nullptr;
    QLabel* tape_census_ = nullptr;
    QListWidget* tape_list_ = nullptr;
    QLabel* sources_value_ = nullptr;
    QLabel* qualification_value_ = nullptr;
    QLabel* qualification_detail_ = nullptr;
    ProofRow proof_symbol_row_;
    ProofRow proof_all_row_;
    QLabel* proof_status_ = nullptr;
    QTimer* refresh_timer_ = nullptr;
    QString exchange_id_ = QStringLiteral("coinbase");
    QString active_symbol_ = QStringLiteral("BTC-USD");
    bool is_paper_ = true;
};

} // namespace openmarketterminal::screens::crypto
