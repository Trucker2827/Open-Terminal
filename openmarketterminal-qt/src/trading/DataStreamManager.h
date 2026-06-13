#pragma once
// DataStreamManager — manages all active AccountDataStream instances.
// Singleton that creates/destroys per-account data streams and forwards
// their signals as aggregated signals for the UI to consume.

#include "trading/AccountDataStream.h"
#include "trading/BrokerAccount.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <functional>

#    include "datahub/Producer.h"

namespace openmarketterminal::trading {

class DataStreamManager : public QObject
    , public openmarketterminal::datahub::Producer
{
    Q_OBJECT
  public:
    static DataStreamManager& instance();

    // --- Stream lifecycle ---
    AccountDataStream* stream_for(const QString& account_id);
    void start_stream(const QString& account_id);
    void stop_stream(const QString& account_id);
    // Tear down and recreate a stream so AccountDataStream::ws_init() reloads the
    // latest credentials. Use after a token re-auth/refresh — plain start_stream()
    // only resume()s an existing stream and would keep the stale cached token.
    void restart_stream(const QString& account_id);
    void start_all_active();
    void stop_all();

    // --- Visibility control (P3: timers respect visibility) ---
    void pause_all();   // Screen hidden — pause all timers
    void resume_all();  // Screen shown — resume all timers

    // Force an immediate portfolio (positions/holdings/orders/funds) refetch for
    // one account, bypassing the 5-min poll. No-op if no stream exists for the
    // account. Used by UI transitions (reopen / account switch / mode toggle) so
    // the blotter shows fresh broker data right away instead of stale-or-blank.
    void refresh_portfolio(const QString& account_id);

    // --- Query ---
    bool has_stream(const QString& account_id) const;
    QStringList active_stream_ids() const;

    // --- DataHub producer wiring (Phase 7) ---
    // Registers with the hub + installs broker:* policies. Idempotent.
    void ensure_registered_with_hub();

    // --- Shared quote feed (thread-safe; callable from any thread) ---
    // Give `consumer_id` a live feed of `symbol` on `account_id`: ensures the
    // account stream is started + subscribed (so the symbol streams and is
    // published to broker:<id>:<acct>:quote:<symbol>), then forwards every quote
    // to `cb`. `cb` is invoked on `owner`'s thread (queued). `owner` is the
    // lifetime guard. All stream/DataHub mutation happens on the main thread.
    void open_quote_feed(QObject* owner, const QString& consumer_id,
                         const QString& account_id, const QString& symbol,
                         std::function<void(const BrokerQuote&)> cb);
    void close_quote_feed(const QString& consumer_id, const QString& account_id);

    // openmarketterminal::datahub::Producer
    QStringList topic_patterns() const override;
    void refresh(const QStringList& topics) override;
    int max_requests_per_sec() const override;

  signals:
    // On-demand / one-shot signals — no hub topic exists for these
    void candles_fetched(const QString& account_id, const QVector<BrokerCandle>& candles);
    void orderbook_fetched(const QString& account_id,
                           const QVector<QPair<double, double>>& bids,
                           const QVector<QPair<double, double>>& asks,
                           double spread, double spread_pct,
                           const QVector<int>& bid_orders,
                           const QVector<int>& ask_orders);
    void time_sales_fetched(const QString& account_id, const QVector<BrokerTrade>& trades);
    void latest_trade_fetched(const QString& account_id, const BrokerTrade& trade);
    void calendar_fetched(const QString& account_id, const QVector<MarketCalendarDay>& days);
    void clock_fetched(const QString& account_id, const MarketClock& clock);
    void connection_state_changed(const QString& account_id, ConnectionState state);
    void token_expired(const QString& account_id);

  private:
    DataStreamManager();
    void wire_stream_signals(AccountDataStream* stream);

    // Slot-style handlers that fan the per-account signals to hub topics.
    // Connected from wire_stream_signals() so every stream publishes.
    void on_positions_for_hub(const QString& account_id, const QVector<BrokerPosition>& positions);
    void on_holdings_for_hub(const QString& account_id, const QVector<BrokerHolding>& holdings);
    void on_orders_for_hub(const QString& account_id, const QVector<BrokerOrderInfo>& orders);
    void on_funds_for_hub(const QString& account_id, const BrokerFunds& funds);
    void on_quote_for_hub(const QString& account_id, const QString& symbol, const BrokerQuote& quote);

    bool hub_registered_ = false;

    QHash<QString, AccountDataStream*> streams_; // account_id -> stream (parent = this)

    // Open shared quote feeds (Stage 2): consumer_id -> feed handle.
    struct QuoteFeed {
        QString account_id;
        QString topic;
        QPointer<QObject> owner;
    };
    QHash<QString, QuoteFeed> quote_feeds_;
};

} // namespace openmarketterminal::trading
