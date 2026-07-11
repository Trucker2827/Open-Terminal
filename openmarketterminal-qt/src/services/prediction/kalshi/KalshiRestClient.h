#pragma once

#include "services/prediction/PredictionTypes.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QJsonDocument;
class QJsonObject;
class QNetworkAccessManager;

namespace openmarketterminal::services::prediction::kalshi_ns {

/// Public (unsigned) REST client for the Kalshi v2 API.
///
///   Base URL : https://external-api.kalshi.com/trade-api/v2
///   Demo URL : https://external-api.demo.kalshi.co/trade-api/v2
///
/// This client owns one QNetworkAccessManager for all public calls. Each
/// method emits its own `*_ready` signal on success and `request_error`
/// on failure. Authenticated endpoints (orders, positions, fills, balance)
/// are NOT implemented here — Phase 7 adds them via a Python bridge that
/// signs requests with the user's RSA private key.
class KalshiRestClient : public QObject {
    Q_OBJECT
  public:
    explicit KalshiRestClient(QObject* parent = nullptr);
    ~KalshiRestClient() override;

    /// Swap base URL between production and demo environments.
    void set_demo_mode(bool demo);
    bool demo_mode() const { return demo_; }
    QString base_url() const;

    // ── Public endpoints ─────────────────────────────────────────────────

    /// GET /markets — paginated; pass empty cursor on first call.
    void fetch_markets(const QString& status = QStringLiteral("open"),
                       const QString& event_ticker = QString(),
                       const QString& series_ticker = QString(),
                       const QString& tickers = QString(),
                       int limit = 100,
                       const QString& cursor = QString());
    /// GET /markets/{ticker} — single market detail.
    void fetch_market(const QString& ticker);
    /// GET /events — paginated; event_list with optional status filter.
    void fetch_events(const QString& status = QStringLiteral("open"),
                      const QString& series_ticker = QString(),
                      bool with_nested_markets = true,
                      int limit = 100,
                      const QString& cursor = QString());
    /// GET /events/{event_ticker} — single event + nested markets.
    void fetch_event(const QString& event_ticker);
    /// GET /series — emits the ~18 distinct human categories via tags_ready.
    void fetch_series(const QString& status = QStringLiteral("open"));
    /// Browse one human category. Kalshi's broad list endpoints ignore category,
    /// so this resolves /series?category=… then fans out per-series requests and
    /// aggregates, emitting events_ready (as_events) or markets_ready.
    /// `frequencies` (e.g. {"fifteen_min","hourly"}) further restricts to series
    /// of those cadences; empty = all cadences. `series_keywords` is used by
    /// the Kalshi-native crypto browser to narrow Crypto to BTC/ETH/SOL/etc.
    void fetch_category(const QString& category, const QStringList& frequencies,
                        const QStringList& series_keywords,
                        bool as_events, int limit);
    /// GET /markets/{ticker}/orderbook — depth 0 = all levels.
    void fetch_order_book(const QString& ticker, int depth = 20);
    /// GET /markets/orderbooks — full REST snapshots for up to 100 tickers.
    void fetch_batch_order_books(const QStringList& tickers);
    /// GET /markets/{ticker}/candlesticks — period_interval in minutes (1, 60, 1440).
    void fetch_candlesticks(const QString& series_ticker, const QString& ticker,
                            int period_interval_min, qint64 start_ts, qint64 end_ts);
    /// GET /markets/{ticker}/trades — recent trades on a single market.
    void fetch_market_trades(const QString& ticker, int limit = 100,
                             const QString& cursor = QString());

    // ── Exchange metadata ────────────────────────────────────────────────

    /// GET /exchange/status — is the exchange accepting trades right now.
    void fetch_exchange_status();
    /// GET /exchange/schedule — trading calendar.
    void fetch_exchange_schedule();

    // ── Series metadata ──────────────────────────────────────────────────

    /// GET /series/{series_ticker} — single series detail (fees, frequency).
    void fetch_series_detail(const QString& series_ticker);
    /// GET /series/fee_changes — upcoming fee adjustments.
    void fetch_series_fee_changes();

    // ── Batch market data ────────────────────────────────────────────────

    /// GET /markets/candlesticks — batched across tickers. Returns a map
    /// of ticker → PriceHistory.
    void fetch_batch_candlesticks(const QStringList& tickers, int period_interval_min,
                                  qint64 start_ts, qint64 end_ts);

    // ── Historical data (public slice) ───────────────────────────────────

    /// GET /historical/markets — archived markets, paginated.
    void fetch_historical_markets(const QString& series_ticker = QString(),
                                  int limit = 100,
                                  const QString& cursor = QString());
    /// GET /historical/markets/{ticker}/candlesticks — archived candles.
    void fetch_historical_candlesticks(const QString& ticker, int period_interval_min,
                                       qint64 start_ts, qint64 end_ts);
    /// GET /historical/trades — archived trade tape.
    void fetch_historical_trades(const QString& ticker = QString(),
                                 int limit = 100,
                                 const QString& cursor = QString());

    // ── Search / filter metadata ─────────────────────────────────────────

    /// GET /search/tags_by_categories — tag taxonomy for browse filters.
    void fetch_search_tags_by_categories();
    /// GET /search/filters_by_sport — sport-specific filter options.
    void fetch_search_filters_by_sport(const QString& sport);

  signals:
    // Each signal carries (markets, cursor) so callers can paginate.
    void markets_ready(const QVector<openmarketterminal::services::prediction::PredictionMarket>& markets,
                       const QString& next_cursor);
    void market_detail_ready(const openmarketterminal::services::prediction::PredictionMarket& market);
    void events_ready(const QVector<openmarketterminal::services::prediction::PredictionEvent>& events,
                      const QString& next_cursor);
    void event_detail_ready(const openmarketterminal::services::prediction::PredictionEvent& event);
    void tags_ready(const QStringList& series_tickers);
    void order_book_ready(const openmarketterminal::services::prediction::PredictionOrderBook& yes_book,
                          const openmarketterminal::services::prediction::PredictionOrderBook& no_book,
                          const QString& ticker);
    void batch_order_books_ready(
        const QHash<QString, openmarketterminal::services::prediction::PredictionOrderBook>& books);
    void price_history_ready(const openmarketterminal::services::prediction::PriceHistory& yes_history,
                             const QString& ticker);
    void trades_ready(const QVector<openmarketterminal::services::prediction::PredictionTrade>& trades);

    // Exchange / series / search metadata — raw JSON pass-through since
    // these are low-use endpoints without a frontend struct yet.
    void exchange_status_ready(const QJsonObject& status);
    void exchange_schedule_ready(const QJsonObject& schedule);
    void series_detail_ready(const QJsonObject& series);
    void series_fee_changes_ready(const QJsonArray& fee_changes);
    void search_tags_ready(const QJsonObject& tags_by_categories);
    void search_filters_ready(const QJsonObject& filters);

    // Batch candlesticks: map of ticker → history.
    void batch_candlesticks_ready(
        const QHash<QString, openmarketterminal::services::prediction::PriceHistory>& histories);

    // Historical (matches the live counterparts).
    void historical_markets_ready(
        const QVector<openmarketterminal::services::prediction::PredictionMarket>& markets,
        const QString& next_cursor);
    void historical_candlesticks_ready(
        const openmarketterminal::services::prediction::PriceHistory& history, const QString& ticker);
    void historical_trades_ready(
        const QVector<openmarketterminal::services::prediction::PredictionTrade>& trades,
        const QString& next_cursor);

    void request_error(const QString& context, const QString& message);

  private:
    using JsonCallback = std::function<void(const QJsonDocument&)>;
    void get_json(const QString& path, JsonCallback on_success, const QString& error_ctx);
    QString absolute_url(const QString& path) const;
    /// Fan out one /events?series_ticker=… request per series, aggregate the
    /// nested markets, and emit once all replies complete (error-tolerant).
    void fan_out_series_events(const QStringList& series, bool as_events, int limit);
    /// Fan out one /markets?series_ticker=… request per series for the MARKETS
    /// tab. This avoids depending on nested events when the user asked for
    /// flat market rows.
    void fan_out_series_markets(const QStringList& series, const QString& category, int limit);

    QNetworkAccessManager* nam_ = nullptr;
    bool demo_ = false;
};

} // namespace openmarketterminal::services::prediction::kalshi_ns
