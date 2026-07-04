#pragma once

#include "services/crypto_latency/CryptoLatencyService.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace openmarketterminal::services::edge_radar {

struct CryptoImpulseOptions {
    int stale_after_ms = 3000;
    double weak_move_pct = 0.05;
    double strong_move_pct = 0.20;
    double minimum_confidence = 0.35;
};

struct CryptoImpulseWindow {
    int seconds = 0;
    double start_price = 0.0;
    double end_price = 0.0;
    double move_pct = 0.0;
    double velocity_pct_per_sec = 0.0;
    qint64 start_ts_ms = 0;
    qint64 end_ts_ms = 0;
    bool available = false;
};

struct CryptoImpulseSignal {
    QString symbol;
    QString direction = QStringLiteral("flat");
    QString strength = QStringLiteral("none");
    QString recommendation = QStringLiteral("no-trade");
    QString gate = QStringLiteral("reject");
    QString rationale;
    QString rejection_reasons;
    double confidence = 0.0;
    qint64 latest_tick_age_ms = -1;
    QString freshest_source;
    QVector<CryptoImpulseWindow> windows;
};

class CryptoImpulseModel {
  public:
    explicit CryptoImpulseModel(CryptoImpulseOptions options = {});

    void clear();
    void add_tick(const openmarketterminal::services::crypto_latency::CryptoLatencyTick& tick);
    CryptoImpulseSignal signal(int primary_window_seconds = 15) const;

    static double anchor_probability(const CryptoImpulseSignal& signal);
    static QJsonObject to_json(const CryptoImpulseSignal& signal);

  private:
    CryptoImpulseWindow window(int seconds) const;

    CryptoImpulseOptions options_;
    QVector<openmarketterminal::services::crypto_latency::CryptoLatencyTick> ticks_;
    qint64 latest_direct_tick_ms_ = 0;
};

} // namespace openmarketterminal::services::edge_radar
