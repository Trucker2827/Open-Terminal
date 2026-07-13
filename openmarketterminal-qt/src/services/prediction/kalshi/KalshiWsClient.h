#pragma once

#include "datahub/Producer.h"
#include "services/prediction/PredictionTypes.h"
#include "services/prediction/kalshi/KalshiCredentials.h"

#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace openmarketterminal {
class WebSocketClient;
}

namespace openmarketterminal::services::prediction::kalshi_ns {

/// Kalshi v2 WebSocket client.
///
///   URL : wss://external-api-ws.kalshi.com/trade-api/ws/v2
///
/// Unlike Polymarket, Kalshi's WS requires an authenticated connection
/// even for public market data (orderbook_delta, ticker, trade channels).
/// Connection is deferred until KalshiCredentials are supplied; without
/// credentials this client silently no-ops and screens fall back to REST
/// polling.
///
/// Hub producer for:
///   prediction:kalshi:price:<ticker>:<side>       (push-only)
///   prediction:kalshi:orderbook:<ticker>:<side>   (push-only)
///
/// Wire protocol (subscribe):
///   {"id":1,"cmd":"subscribe","params":{
///       "channels":["orderbook_delta","ticker"],
///       "market_tickers":["<ticker>", …]
///   }}
///
/// Delta reconciliation: Kalshi emits `orderbook_snapshot` once per
/// subscription, then incremental `orderbook_delta`. Messages include a
/// monotonic `seq`; on gap we request fresh snapshots over the socket.
class KalshiWsClient : public QObject, public openmarketterminal::datahub::Producer {
    Q_OBJECT
  public:
    explicit KalshiWsClient(QObject* parent = nullptr);
    ~KalshiWsClient() override;

    /// Update / clear credentials. Passing an invalid set disconnects.
    void set_credentials(const KalshiCredentials& creds);
    bool has_credentials() const;

    void subscribe(const QStringList& market_tickers);
    void subscribe_cf_indices(const QStringList& index_ids);
    void unsubscribe(const QStringList& market_tickers);
    void unsubscribe_all();
    void disconnect();
    /// Force a fresh authenticated socket while retaining subscriptions.
    /// Used by the daemon watchdog when the transport still reports connected
    /// but no market events have arrived within the freshness budget.
    void restart();
    bool is_connected() const { return connected_; }

    /// Register with the hub + install prediction:kalshi:* policies.
    void ensure_registered_with_hub();

    // datahub::Producer
    QStringList topic_patterns() const override;
    void refresh(const QStringList& topics) override;
    int max_requests_per_sec() const override { return 2; }

  signals:
    void price_updated(const QString& asset_id, double price);
    void orderbook_updated(const QString& asset_id,
                           const openmarketterminal::services::prediction::PredictionOrderBook& book);
    void trade_received(const openmarketterminal::services::prediction::PredictionTrade& trade);
    void trade_event(const QString& ticker, const QJsonObject& payload);
    void market_lifecycle_changed(const QString& ticker, const QString& status);
    void market_lifecycle_event(const QString& ticker, const QString& status,
                                const QJsonObject& payload);
    void ticker_event(const QString& ticker, const QJsonObject& payload);
    void orderbook_event(const QString& type, const QString& ticker, qint64 sequence,
                         const QJsonObject& payload);
    void account_event(const QString& type, const QJsonObject& payload);
    void cf_benchmark_event(const QString& index_id, double value, qint64 ts_ms,
                            const QJsonObject& payload);
    void connection_status_changed(bool connected);

  private slots:
    void on_connected();
    void on_disconnected();
    void on_message(const QString& msg);
    void on_error(const QString& err);
    void send_ping();

  private:
    void ensure_connected();
    void send_subscribe(const QStringList& tickers);
    void send_account_subscribe();
    void send_cf_subscribe();
    void request_orderbook_snapshot(const QString& ticker);
    void publish_books(const QString& ticker, qint64 ts_ms);
    void publish_price(const QString& asset_id, double price);
    void publish_orderbook(const QString& asset_id,
                           const openmarketterminal::services::prediction::PredictionOrderBook& book);

    openmarketterminal::WebSocketClient* ws_ = nullptr;
    QTimer* ping_timer_ = nullptr;

    KalshiCredentials creds_;
    QSet<QString> subscribed_tickers_;
    QSet<QString> cf_indices_;
    bool connected_ = false;
    bool restart_requested_ = false;
    bool hub_registered_ = false;
    int next_msg_id_ = 1;
    int orderbook_subscription_sid_ = 0;

    struct BookState {
        QMap<int, double> yes_bids;
        QMap<int, double> no_bids;
        bool has_snapshot = false;
    };
    QHash<QString, BookState> books_;
    qint64 orderbook_sequence_ = 0;

    static constexpr const char* kProdWs = "wss://external-api-ws.kalshi.com/trade-api/ws/v2";
    static constexpr const char* kDemoWs = "wss://external-api-ws.demo.kalshi.co/trade-api/ws/v2";
};

} // namespace openmarketterminal::services::prediction::kalshi_ns
