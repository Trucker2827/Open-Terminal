#pragma once
// CryptoMicrostructureWidget - compact BTC tape and bid/ask pressure radar.
//
// Config (persisted):
//   { "symbol": "BTC-USD", "sources": ["coinbase", "kraken", "bitcointicker"] }

#include "screens/dashboard/widgets/BaseWidget.h"
#include "services/crypto_latency/CryptoLatencyService.h"
#include "services/edge_radar/CryptoMicrostructureRadar.h"

#include <QHash>
#include <QJsonArray>
#include <QString>
#include <QStringList>

class QLabel;
class QTableWidget;
class QTimer;

namespace openmarketterminal::screens::widgets {

class CryptoMicrostructureWidget : public BaseWidget {
    Q_OBJECT
  public:
    explicit CryptoMicrostructureWidget(const QJsonObject& cfg = {}, QWidget* parent = nullptr);
    ~CryptoMicrostructureWidget() override;

    QJsonObject config() const override;
    void apply_config(const QJsonObject& cfg) override;

  protected:
    void on_theme_changed() override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void retranslateUi() override;
    QDialog* make_config_dialog(QWidget* parent) override;

  private:
    void apply_styles();
    void start_feed();
    void stop_feed();
    void render();
    void render_sources(const openmarketterminal::services::edge_radar::CryptoMicrostructureSnapshot& snap);
    void render_windows(const openmarketterminal::services::edge_radar::CryptoMicrostructureSnapshot& snap);
    void on_tick(const openmarketterminal::services::crypto_latency::CryptoLatencyTick& tick);
    static QString fmt_price(double value);
    static QString fmt_age(qint64 age_ms);
    static QStringList normalize_sources(const QJsonArray& arr);

    QString symbol_ = QStringLiteral("BTC-USD");
    QStringList sources_;
    bool feed_active_ = false;

    openmarketterminal::services::crypto_latency::CryptoLatencyService feed_;
    openmarketterminal::services::edge_radar::CryptoMicrostructureRadar radar_;

    QTimer* render_timer_ = nullptr;
    QLabel* call_label_ = nullptr;
    QLabel* detail_label_ = nullptr;
    QLabel* price_label_ = nullptr;
    QLabel* book_label_ = nullptr;
    QLabel* pressure_label_ = nullptr;
    QLabel* rationale_label_ = nullptr;
    QTableWidget* windows_table_ = nullptr;
    QTableWidget* sources_table_ = nullptr;
};

} // namespace openmarketterminal::screens::widgets
