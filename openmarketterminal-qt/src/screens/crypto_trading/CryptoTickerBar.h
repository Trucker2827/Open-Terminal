#pragma once
// Crypto Price Ribbon — dense single-line: price, change, bid/ask, spread, high/low/volume, WS status

#include "screens/crypto_trading/CryptoTickerVolSigma.h"

#include <QLabel>
#include <QWidget>

namespace openmarketterminal::screens::crypto {

class CryptoTickerBar : public QWidget {
    Q_OBJECT
  public:
    explicit CryptoTickerBar(QWidget* parent = nullptr);

    void set_symbol(const QString& symbol);
    // live_sample=false marks a cache replay: it may update the display but
    // must not seed the 1m move window — a stale baseline would fabricate a
    // "recent" move (or a flat 0.0σ) that never happened in the last minute.
    void update_data(double price, double change_pct, double high, double low, double volume, bool ws_connected,
                     bool live_sample = true);
    void update_bid_ask(double bid, double ask, double spread);
    void update_mark_price(double mark_price, double index_price);

  private:
    QLabel* symbol_label_ = nullptr;
    QLabel* price_label_ = nullptr;
    QLabel* change_label_ = nullptr;
    QLabel* bid_label_ = nullptr;
    QLabel* ask_label_ = nullptr;
    QLabel* spread_label_ = nullptr;
    QLabel* high_label_ = nullptr;
    QLabel* low_label_ = nullptr;
    QLabel* volume_label_ = nullptr;
    QLabel* mark_price_label_ = nullptr;
    QLabel* index_price_label_ = nullptr;

    // State cache — avoid redundant setText calls at ticker rate. Values are
    // rounded to the display precision before comparing so we only repaint on
    // a pixel-visible change.
    bool last_positive_ = true;
    double last_price_display_ = -1;
    double last_change_display_ = 1e300;
    double last_high_display_ = -1;
    double last_low_display_ = -1;
    double last_volume_display_ = -1;
    int last_volume_unit_ = -1;  // 0=raw, 1=M, 2=B
    double last_bid_display_ = -1;
    double last_ask_display_ = -1;
    double last_spread_display_ = -1;

    // Ambient vol + move-in-sigmas readout (issue #97). The estimator state
    // is refreshed from the stored tick series at most once per minute; the
    // move window is fed by live ticker updates only.
    void refresh_vol_state(qint64 now_ms);
    void render_vol_sigma(qint64 now_ms);

    QLabel* vol_sigma_label_ = nullptr;
    openmarketterminal::crypto::TickerMoveWindow move_window_;
    openmarketterminal::crypto::TickerVolState vol_state_;
    QString vol_base_symbol_;
    qint64 vol_refreshed_ms_ = 0;
    QString last_vol_sigma_text_;
    QString last_vol_sigma_tooltip_;
};

} // namespace openmarketterminal::screens::crypto
