#pragma once

#include "services/crypto_latency/CryptoLatencyService.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace openmarketterminal::services::edge_radar {

struct CryptoMicrostructureSource {
    QString source;
    QString status;
    QString message;
    double price = 0.0;
    double best_bid = 0.0;
    double best_ask = 0.0;
    double spread_bps = 0.0;
    qint64 age_ms = -1;
    int ticks = 0;
};

struct CryptoMicrostructureWindow {
    int seconds = 0;
    int upticks = 0;
    int downticks = 0;
    int flat_ticks = 0;
    double start_price = 0.0;
    double end_price = 0.0;
    double move_pct = 0.0;
    double tape_pressure = 0.0;
    bool available = false;
};

struct CryptoMicrostructureSnapshot {
    QString symbol = QStringLiteral("BTC-USD");
    QString call = QStringLiteral("NO TRADE");
    QString direction = QStringLiteral("flat");
    QString rationale;
    QString freshest_source;
    qint64 freshest_age_ms = -1;
    double reference_price = 0.0;
    double cross_source_spread_bps = 0.0;
    double book_pressure = 0.0;
    double tape_pressure = 0.0;
    double confidence = 0.0;
    int live_sources = 0;
    int tick_count = 0;
    QVector<CryptoMicrostructureSource> sources;
    QVector<CryptoMicrostructureWindow> windows;
};

class CryptoMicrostructureRadar {
  public:
    void clear();
    void add_tick(const openmarketterminal::services::crypto_latency::CryptoLatencyTick& tick);
    CryptoMicrostructureSnapshot snapshot(
        const openmarketterminal::services::crypto_latency::CryptoLatencySnapshot& latency_snapshot) const;

    static QJsonObject to_json(const CryptoMicrostructureSnapshot& snapshot);

  private:
    CryptoMicrostructureWindow window(int seconds) const;
    QVector<openmarketterminal::services::crypto_latency::CryptoLatencyTick> ticks_;
};

} // namespace openmarketterminal::services::edge_radar
