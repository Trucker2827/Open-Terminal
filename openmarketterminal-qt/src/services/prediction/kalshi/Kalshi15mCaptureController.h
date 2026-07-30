#pragma once
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <memory>
#include "services/prediction/PredictionTypes.h"

namespace openmarketterminal::services::prediction { class PredictionExchangeAdapter; }
namespace openmarketterminal::services::prediction::kalshi_ns { class KalshiRestClient; }

/// Always-on: discovers open KXBTC15M markets via REST and keeps the app's
/// Kalshi WS subscribed to their ticker channel, independent of UI selection.
/// Recording is the existing ws_ticker_event -> kalshi-tickers.jsonl sink.
class Kalshi15mCaptureController : public QObject {
    Q_OBJECT
public:
    explicit Kalshi15mCaptureController(
        openmarketterminal::services::prediction::PredictionExchangeAdapter* adapter,
        QObject* parent = nullptr);
    ~Kalshi15mCaptureController() override;
    void start();   // begins the discovery timer + first immediate poll
    void stop();

private slots:
    void poll();
    void on_markets_ready(
        const QVector<openmarketterminal::services::prediction::PredictionMarket>& markets,
        const QString& next_cursor);

private:
    void reconcile_and_apply();
    openmarketterminal::services::prediction::PredictionExchangeAdapter* adapter_;
    std::unique_ptr<openmarketterminal::services::prediction::kalshi_ns::KalshiRestClient> rest_;
    QTimer poll_timer_;
    QStringList families_{QStringLiteral("KXBTC15M")};
    int cap_ = 200;
    int poll_interval_ms_ = 30000;
    QVector<openmarketterminal::services::prediction::PredictionMarket> page_accum_;
    QSet<QString> held_;
};
