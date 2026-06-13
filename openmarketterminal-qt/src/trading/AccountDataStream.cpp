#include "trading/AccountDataStream.h"

#include "core/logging/Logger.h"
#include "trading/AccountManager.h"
#include "trading/BrokerRegistry.h"
#include "trading/HistoricalDataService.h"
#include "trading/OrderMatcher.h"
#include "trading/PaperTrading.h"
#include "trading/instruments/InstrumentService.h"
#include "trading/brokers/alpaca/AlpacaWebSocket.h"

#include <QDate>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QtConcurrent>

#include <algorithm>

namespace openmarketterminal::trading {

namespace {
constexpr const char* ADS_TAG = "AccountDataStream";
constexpr int ADS_QUOTE_POLL_MS = 300000;
constexpr int ADS_PORTFOLIO_POLL_MS = 300000;
constexpr int ADS_WATCHLIST_POLL_MS = 300000;
constexpr int ADS_ACTIVE_FEED_POLL_MS = 3000; // fast poll for algo/active-feed symbols (non-WS brokers)
} // namespace

// ── Construction / Destruction ──────────────────────────────────────────────

AccountDataStream::AccountDataStream(const QString& account_id, QObject* parent)
    : QObject(parent), account_id_(account_id) {
    auto account = AccountManager::instance().get_account(account_id);
    broker_id_ = account.broker_id;

    // Create timers (P3: don't start in constructor, intervals only)
    quote_timer_ = new QTimer(this);
    quote_timer_->setInterval(ADS_QUOTE_POLL_MS);
    connect(quote_timer_, &QTimer::timeout, this, &AccountDataStream::on_quote_timer);

    portfolio_timer_ = new QTimer(this);
    portfolio_timer_->setInterval(ADS_PORTFOLIO_POLL_MS);
    connect(portfolio_timer_, &QTimer::timeout, this, &AccountDataStream::on_portfolio_timer);

    watchlist_timer_ = new QTimer(this);
    watchlist_timer_->setInterval(ADS_WATCHLIST_POLL_MS);
    connect(watchlist_timer_, &QTimer::timeout, this, &AccountDataStream::on_watchlist_timer);

    active_feed_timer_ = new QTimer(this);
    active_feed_timer_->setInterval(ADS_ACTIVE_FEED_POLL_MS);
    connect(active_feed_timer_, &QTimer::timeout, this, &AccountDataStream::on_active_feed_timer);
}

AccountDataStream::~AccountDataStream() {
    stop();
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

void AccountDataStream::start() {
    if (running_)
        return;
    running_ = true;

    // Try to init WebSocket if broker supports it
    ws_init();

    // Start polling (suppressed per-timer if WS active)
    if (!ws_active()) {
        quote_timer_->start();
        watchlist_timer_->start();
    }
    portfolio_timer_->start();
    if (!active_feed_symbol_union().isEmpty())
        active_feed_timer_->start();

    // Initial data fetch
    async_fetch_quote();
    async_fetch_watchlist_quotes();
    async_fetch_positions();
    async_fetch_holdings();
    async_fetch_orders();
    async_fetch_funds();

    LOG_INFO(ADS_TAG, QString("Started stream for account %1 (%2)").arg(account_id_, broker_id_));
}

void AccountDataStream::stop() {
    if (!running_)
        return;
    running_ = false;

    quote_timer_->stop();
    portfolio_timer_->stop();
    watchlist_timer_->stop();
    active_feed_timer_->stop();
    ws_teardown();

    LOG_INFO(ADS_TAG, QString("Stopped stream for account %1").arg(account_id_));
}

void AccountDataStream::pause() {
    quote_timer_->stop();
    portfolio_timer_->stop();
    watchlist_timer_->stop();
    active_feed_timer_->stop();
    // Keep WS alive in background
}

void AccountDataStream::resume() {
    if (!running_)
        return;
    if (!ws_active()) {
        quote_timer_->start();
        watchlist_timer_->start();
    }
    portfolio_timer_->start();
    if (!active_feed_symbol_union().isEmpty())
        active_feed_timer_->start();
}

void AccountDataStream::refresh_portfolio_now() {
    if (!running_)
        return;
    async_fetch_positions();
    async_fetch_holdings();
    async_fetch_orders();
    async_fetch_funds();
}

// ── Symbol management ───────────────────────────────────────────────────────

void AccountDataStream::subscribe_symbols(const QString& consumer_id, const QStringList& symbols) {
    LOG_INFO("subdbg", QString("subscribe_symbols consumer='%1' (%2 syms): [%3] ws_connected=%4")
                           .arg(consumer_id).arg(symbols.size())
                           .arg(symbols.join(','), ws_connected() ? "Y" : "N"));
    if (symbols.isEmpty())
        consumer_symbols_.remove(consumer_id);
    else
        consumer_symbols_[consumer_id] = symbols;
    // Resubscribe whenever the socket is connected — NOT gated on ws_active(),
    // which also requires ticks. On an account switch-back the socket is
    // connected but may have 0 ticks (it was subscribed to 0 symbols while
    // unfocused), so an ws_active() gate would never re-push the watchlist.
    if (ws_connected())
        ws_resubscribe();
}

void AccountDataStream::unsubscribe_consumer(const QString& consumer_id) {
    const bool had = consumer_symbols_.remove(consumer_id) > 0;
    active_feed_symbols_.remove(consumer_id);
    if (active_feed_timer_ && active_feed_symbol_union().isEmpty())
        active_feed_timer_->stop();
    if (had && ws_connected())
        ws_resubscribe();
}

void AccountDataStream::set_active_feed(const QString& consumer_id, const QStringList& symbols) {
    if (symbols.isEmpty())
        active_feed_symbols_.remove(consumer_id);
    else
        active_feed_symbols_[consumer_id] = symbols;
    if (!active_feed_timer_)
        return;
    const bool any = !active_feed_symbol_union().isEmpty();
    if (any && running_) {
        if (!active_feed_timer_->isActive())
            active_feed_timer_->start();
        if (!ws_connected())
            async_fetch_active_feed_quotes(); // non-WS only; WS brokers wait for the pushed tick
    } else if (!any && active_feed_timer_->isActive()) {
        active_feed_timer_->stop();
    }
}

QStringList AccountDataStream::subscribed_symbols() const {
    QStringList out;
    for (auto it = consumer_symbols_.constBegin(); it != consumer_symbols_.constEnd(); ++it)
        for (const QString& s : it.value())
            if (!out.contains(s))
                out.append(s);
    return out;
}

QStringList AccountDataStream::active_feed_symbol_union() const {
    QStringList out;
    for (auto it = active_feed_symbols_.constBegin(); it != active_feed_symbols_.constEnd(); ++it)
        for (const QString& s : it.value())
            if (!out.contains(s))
                out.append(s);
    return out;
}

void AccountDataStream::set_selected_symbol(const QString& symbol, const QString& exchange) {
    selected_symbol_ = symbol;
    selected_exchange_ = exchange;
    if (ws_connected())
        ws_resubscribe();
}

// ── Cached data access ──────────────────────────────────────────────────────

QVector<BrokerPosition> AccountDataStream::cached_positions() const { return positions_; }
QVector<BrokerHolding> AccountDataStream::cached_holdings() const { return holdings_; }
QVector<BrokerOrderInfo> AccountDataStream::cached_orders() const { return orders_; }
BrokerFunds AccountDataStream::cached_funds() const { return funds_; }

BrokerQuote AccountDataStream::cached_quote(const QString& symbol) const {
    auto it = quote_cache_.find(symbol);
    return it != quote_cache_.end() ? it.value() : BrokerQuote{};
}

// ── Timer callbacks ─────────────────────────────────────────────────────────

// ── Token expiry check ─────────────────────────────────────────────────────

bool AccountDataStream::is_token_expired() const {
    auto creds = AccountManager::instance().load_credentials(account_id_);
    if (creds.additional_data.isEmpty())
        return false;
    auto doc = QJsonDocument::fromJson(creds.additional_data.toUtf8());
    qint64 expires_at = static_cast<qint64>(doc.object().value("token_expires_at").toDouble(0));
    return expires_at > 0 && expires_at <= QDateTime::currentSecsSinceEpoch();
}

bool AccountDataStream::check_token_expiry(const QString& error) {
    // All brokers prefix expired-session errors with "[TOKEN_EXPIRED]"
    // (originated by ZerodhaBroker; generalized here so every broker benefits).
    if (!error.startsWith("[TOKEN_EXPIRED]"))
        return false;

    // May be called from a QtConcurrent worker — AccountManager mutates state
    // and we emit a signal, so marshal to the thread that owns this object.
    QPointer<AccountDataStream> self = this;
    QMetaObject::invokeMethod(this, [self]() {
        if (!self)
            return;
        AccountManager::instance().set_connection_state(
            self->account_id_, ConnectionState::TokenExpired,
            QStringLiteral("Broker session token expired"));
        LOG_WARN(ADS_TAG, QString("Token expired for account %1 (%2)")
                              .arg(self->account_id_, self->broker_id_));
        emit self->token_expired(self->account_id_);
    }, Qt::QueuedConnection);
    return true;
}

void AccountDataStream::on_quote_timer() {
    if (is_token_expired()) {
        emit token_expired(account_id_);
        return;
    }
    async_fetch_quote();
}

void AccountDataStream::on_portfolio_timer() {
    if (is_token_expired()) {
        emit token_expired(account_id_);
        return;
    }
    if (portfolio_fetching_.exchange(true))
        return;
    async_fetch_positions();
    async_fetch_holdings();
    async_fetch_orders();
    async_fetch_funds();
    portfolio_fetching_ = false;
}

void AccountDataStream::on_watchlist_timer() {
    if (is_token_expired()) {
        emit token_expired(account_id_);
        return;
    }
    async_fetch_watchlist_quotes();
}

// ── Async fetchers ──────────────────────────────────────────────────────────
// All follow P8: capture account_id by value, QPointer guard, QueuedConnection

void AccountDataStream::async_fetch_quote() {
    if (quote_fetching_.exchange(true))
        return;
    if (selected_symbol_.isEmpty()) {
        quote_fetching_ = false;
        return;
    }

    const QString acct_id = account_id_;
    const QString bid = broker_id_;
    const QString symbol = selected_symbol_;
    QPointer<AccountDataStream> self = this;

    // Load credentials on the calling (main) thread. QSqlDatabase is bound
    // to the thread that opened it; calling SecureStorage::retrieve from a
    // QtConcurrent worker corrupts SQLite memory and crashes.
    auto creds = AccountManager::instance().load_credentials(acct_id);
    if (creds.api_key.isEmpty()) {
        quote_fetching_ = false;
        return;
    }

    (void)QtConcurrent::run([self, acct_id, bid, symbol, creds]() {
        auto* broker = BrokerRegistry::instance().get(bid);
        if (!broker) {
            if (self) self->quote_fetching_ = false;
            return;
        }
        auto result = broker->get_quotes(creds, {symbol});
        if (self) self->quote_fetching_ = false;
        if (!result.success || !result.data || result.data->isEmpty()) {
            LOG_WARN(ADS_TAG, QString("async_fetch_quote failed for %1/%2 [%3]: %4")
                                  .arg(bid, acct_id, symbol, result.error));
            if (self)
                self->check_token_expiry(result.error);
            return;
        }
        const auto quote = result.data->first();
        QMetaObject::invokeMethod(self, [self, acct_id, symbol, quote]() {
            if (!self) return;
            self->quote_cache_[symbol] = quote;
            emit self->quote_updated(acct_id, symbol, quote);
        }, Qt::QueuedConnection);
    });
}

void AccountDataStream::async_fetch_positions() {
    const QString acct_id = account_id_;
    const QString bid = broker_id_;
    QPointer<AccountDataStream> self = this;

    auto creds = AccountManager::instance().load_credentials(acct_id);
    if (creds.api_key.isEmpty()) return;

    (void)QtConcurrent::run([self, acct_id, bid, creds]() {
        auto* broker = BrokerRegistry::instance().get(bid);
        if (!broker) return;
        auto result = broker->get_positions(creds);
        if (!result.success || !result.data) {
            LOG_WARN(ADS_TAG, QString("async_fetch_positions failed for %1/%2: %3")
                                  .arg(bid, acct_id, result.error));
            if (self)
                self->check_token_expiry(result.error);
            return;
        }
        // Success — log the count so an empty live blotter can be told apart from a
        // token/error (which logs WARN above): 0 rows here = genuinely no positions.
        LOG_INFO(ADS_TAG, QString("get_positions %1/%2 → %3 row(s)").arg(bid, acct_id).arg(result.data->size()));
        QMetaObject::invokeMethod(self, [self, acct_id, data = *result.data]() {
            if (!self) return;
            self->positions_ = data;
            emit self->positions_updated(acct_id, data);
        }, Qt::QueuedConnection);
    });
}

void AccountDataStream::async_fetch_holdings() {
    const QString acct_id = account_id_;
    const QString bid = broker_id_;
    QPointer<AccountDataStream> self = this;

    auto creds = AccountManager::instance().load_credentials(acct_id);
    if (creds.api_key.isEmpty()) return;

    (void)QtConcurrent::run([self, acct_id, bid, creds]() {
        auto* broker = BrokerRegistry::instance().get(bid);
        if (!broker) return;
        auto result = broker->get_holdings(creds);
        if (!result.success || !result.data) {
            LOG_WARN(ADS_TAG, QString("async_fetch_holdings failed for %1/%2: %3")
                                  .arg(bid, acct_id, result.error));
            if (self)
                self->check_token_expiry(result.error);
            return;
        }
        LOG_INFO(ADS_TAG, QString("get_holdings %1/%2 → %3 row(s)").arg(bid, acct_id).arg(result.data->size()));
        QMetaObject::invokeMethod(self, [self, acct_id, data = *result.data]() {
            if (!self) return;
            self->holdings_ = data;
            emit self->holdings_updated(acct_id, data);
        }, Qt::QueuedConnection);
    });
}

void AccountDataStream::async_fetch_orders() {
    const QString acct_id = account_id_;
    const QString bid = broker_id_;
    QPointer<AccountDataStream> self = this;

    auto creds = AccountManager::instance().load_credentials(acct_id);
    if (creds.api_key.isEmpty()) return;

    (void)QtConcurrent::run([self, acct_id, bid, creds]() {
        auto* broker = BrokerRegistry::instance().get(bid);
        if (!broker) return;
        auto result = broker->get_orders(creds);
        if (!result.success || !result.data) {
            LOG_WARN(ADS_TAG, QString("async_fetch_orders failed for %1/%2: %3")
                                  .arg(bid, acct_id, result.error));
            if (self)
                self->check_token_expiry(result.error);
            return;
        }
        LOG_INFO(ADS_TAG, QString("get_orders %1/%2 → %3 row(s)").arg(bid, acct_id).arg(result.data->size()));
        QMetaObject::invokeMethod(self, [self, acct_id, data = *result.data]() {
            if (!self) return;
            self->orders_ = data;
            emit self->orders_updated(acct_id, data);
        }, Qt::QueuedConnection);
    });
}

void AccountDataStream::async_fetch_funds() {
    const QString acct_id = account_id_;
    const QString bid = broker_id_;
    QPointer<AccountDataStream> self = this;

    auto creds = AccountManager::instance().load_credentials(acct_id);
    if (creds.api_key.isEmpty()) return;

    (void)QtConcurrent::run([self, acct_id, bid, creds]() {
        auto* broker = BrokerRegistry::instance().get(bid);
        if (!broker) return;
        auto result = broker->get_funds(creds);
        if (!result.success || !result.data) {
            LOG_WARN(ADS_TAG, QString("async_fetch_funds failed for %1/%2: %3")
                                  .arg(bid, acct_id, result.error));
            if (self)
                self->check_token_expiry(result.error);
            return;
        }
        QMetaObject::invokeMethod(self, [self, acct_id, data = *result.data]() {
            if (!self) return;
            self->funds_ = data;
            emit self->funds_updated(acct_id, data);
        }, Qt::QueuedConnection);
    });
}

void AccountDataStream::async_fetch_watchlist_quotes() {
    const QStringList symbols = subscribed_symbols();
    if (symbols.isEmpty())
        return;
    const QString acct_id = account_id_;
    const QString bid = broker_id_;
    QPointer<AccountDataStream> self = this;

    auto creds = AccountManager::instance().load_credentials(acct_id);
    if (creds.api_key.isEmpty()) return;

    (void)QtConcurrent::run([self, acct_id, bid, symbols, creds]() {
        auto* broker = BrokerRegistry::instance().get(bid);
        if (!broker) return;
        auto result = broker->get_quotes(creds, symbols.toVector());
        if (!result.success || !result.data) {
            LOG_WARN(ADS_TAG, QString("async_fetch_watchlist_quotes failed for %1/%2 (%3 syms): %4")
                                  .arg(bid, acct_id).arg(symbols.size()).arg(result.error));
            if (self)
                self->check_token_expiry(result.error);
            return;
        }
        QMetaObject::invokeMethod(self, [self, acct_id, data = *result.data]() {
            if (!self) return;
            for (const auto& q : data)
                self->quote_cache_[q.symbol] = q;
            emit self->watchlist_updated(acct_id, data);
        }, Qt::QueuedConnection);
    });
}

// ── On-demand fetches ───────────────────────────────────────────────────────

void AccountDataStream::on_active_feed_timer() {
    if (!running_ || ws_connected())
        return; // WS is connected and pushes ticks — poll ONLY for non-WS brokers / WS outage
    if (is_token_expired()) {
        emit token_expired(account_id_);
        return;
    }
    async_fetch_active_feed_quotes();
}

void AccountDataStream::async_fetch_active_feed_quotes() {
    const QStringList symbols = active_feed_symbol_union();
    if (symbols.isEmpty())
        return;
    const QString acct_id = account_id_;
    const QString bid = broker_id_;
    QPointer<AccountDataStream> self = this;

    auto creds = AccountManager::instance().load_credentials(acct_id);
    if (creds.api_key.isEmpty())
        return;

    (void)QtConcurrent::run([self, acct_id, bid, symbols, creds]() {
        auto* broker = BrokerRegistry::instance().get(bid);
        if (!broker)
            return;
        auto result = broker->get_quotes(creds, symbols.toVector());
        if (!result.success || !result.data) {
            if (self)
                self->check_token_expiry(result.error);
            return;
        }
        // Emit PER-SYMBOL quote_updated (not watchlist_updated) so DataStreamManager
        // publishes each to broker:<id>:<acct>:quote:<symbol> (the topic algo feeds read).
        QMetaObject::invokeMethod(self, [self, acct_id, data = *result.data]() {
            if (!self) return;
            for (const auto& q : data) {
                self->quote_cache_[q.symbol] = q;
                emit self->quote_updated(acct_id, q.symbol, q);
            }
        }, Qt::QueuedConnection);
    });
}

void AccountDataStream::fetch_candles(const QString& symbol, const QString& timeframe) {
    if (candles_fetching_.exchange(true))
        return;
    const QString acct_id = account_id_;
    QPointer<AccountDataStream> self = this;

    // Map the chart timeframe to a look-back window (preserves the prior per-tf
    // windows), then delegate to the shared HistoricalDataService (broker fetch +
    // symbol resolution + 60s cache). Yahoo fallback is algo-only, not used here.
    auto lookback_for = [](const QString& tf) -> int {
        if (tf == "1d" || tf == "D" || tf == "1w" || tf == "W") return 3 * 365;
        if (tf == "1h" || tf == "60") return 60;
        if (tf == "30m") return 45;
        if (tf == "15m") return 30;
        if (tf == "5m") return 15;
        return 7;
    };

    HistoricalDataService::instance().fetch(
        symbol, timeframe, lookback_for(timeframe), broker_id_, acct_id,
        [self, acct_id](bool ok, const QVector<BrokerCandle>& candles, const QString& err) {
            if (!self)
                return;
            self->candles_fetching_ = false;
            if (ok) {
                emit self->candles_fetched(acct_id, candles);
            } else {
                self->check_token_expiry(err);
                LOG_WARN(ADS_TAG, QString("fetch_candles failed for %1/%2: %3")
                                      .arg(self->broker_id_, acct_id, err));
            }
        });
}

void AccountDataStream::fetch_orderbook(const QString& symbol) {
    const QString acct_id = account_id_;
    const QString bid = broker_id_;
    QPointer<AccountDataStream> self = this;

    auto creds = AccountManager::instance().load_credentials(acct_id);
    if (creds.api_key.isEmpty()) {
        LOG_WARN(ADS_TAG, QString("fetch_orderbook: no credentials for %1").arg(acct_id));
        return;
    }

    (void)QtConcurrent::run([self, acct_id, bid, symbol, creds]() {
        auto* broker = BrokerRegistry::instance().get(bid);
        if (!broker) return;

        const QString today = QDate::currentDate().toString("yyyy-MM-dd");
        auto result = broker->get_historical_quotes_single(creds, symbol, today + "T00:00:00Z", "", 1000);

        if (!result.success || !result.data || result.data->isEmpty()) {
            LOG_WARN(ADS_TAG, QString("fetch_orderbook: L2 depth failed for %1 (%2), trying snapshot")
                                  .arg(symbol, result.error));
            auto snap = broker->get_quotes(creds, {symbol});
            if (!snap.success || !snap.data || snap.data->isEmpty()) {
                LOG_WARN(ADS_TAG, QString("fetch_orderbook: snapshot fallback also failed for %1").arg(symbol));
                return;
            }
            const auto& q = snap.data->first();
            const double b = q.bid > 0 ? q.bid : (q.ltp > 0 ? q.ltp * 0.9995 : 0);
            const double a = q.ask > 0 ? q.ask : (q.ltp > 0 ? q.ltp * 1.0005 : 0);
            if (b <= 0 || a <= 0) return;
            QVector<QPair<double, double>> bids{{b, q.bid_size > 0 ? q.bid_size : 100.0}};
            QVector<QPair<double, double>> asks{{a, q.ask_size > 0 ? q.ask_size : 100.0}};
            const double spread = a - b;
            const double spread_pct = b > 0 ? (spread / b) * 100.0 : 0.0;
            QMetaObject::invokeMethod(self, [self, acct_id, bids, asks, spread, spread_pct]() {
                if (!self) return;
                emit self->orderbook_fetched(acct_id, bids, asks, spread, spread_pct, {}, {});
            }, Qt::QueuedConnection);
            return;
        }

        // Aggregate L2 — track orders count per level
        QMap<double, double> bid_map, ask_map;
        QMap<double, int> bid_ord_map, ask_ord_map;
        for (const auto& q : *result.data) {
            if (q.bid > 0) {
                bid_map[q.bid] += q.bid_size > 0 ? q.bid_size : 1.0;
                if (q.oi > 0) bid_ord_map[q.bid] += static_cast<int>(q.oi);
            }
            if (q.ask > 0) {
                ask_map[q.ask] += q.ask_size > 0 ? q.ask_size : 1.0;
                if (q.oi > 0) ask_ord_map[q.ask] += static_cast<int>(q.oi);
            }
        }
        QVector<QPair<double, double>> bids, asks;
        QVector<int> bid_orders, ask_orders;
        auto bid_keys = bid_map.keys();
        std::sort(bid_keys.begin(), bid_keys.end(), std::greater<double>());
        for (const double p : bid_keys.mid(0, 10)) {
            bids.append({p, bid_map[p]});
            bid_orders.append(bid_ord_map.value(p, 0));
        }
        auto ask_keys = ask_map.keys();
        std::sort(ask_keys.begin(), ask_keys.end());
        for (const double p : ask_keys.mid(0, 10)) {
            asks.append({p, ask_map[p]});
            ask_orders.append(ask_ord_map.value(p, 0));
        }
        if (bids.isEmpty() || asks.isEmpty()) return;

        LOG_INFO(ADS_TAG, QString("fetch_orderbook: %1 bids, %2 asks for %3")
                              .arg(bids.size()).arg(asks.size()).arg(symbol));

        const double best_bid = bids.first().first;
        const double best_ask = asks.first().first;
        const double spread = best_ask - best_bid;
        const double spread_pct = best_bid > 0 ? (spread / best_bid) * 100.0 : 0.0;
        QMetaObject::invokeMethod(self, [self, acct_id, bids, asks, spread, spread_pct,
                                         bid_orders, ask_orders]() {
            if (!self) return;
            emit self->orderbook_fetched(acct_id, bids, asks, spread, spread_pct,
                                         bid_orders, ask_orders);
        }, Qt::QueuedConnection);
    });
}

void AccountDataStream::fetch_time_sales(const QString& symbol) {
    const QString acct_id = account_id_;
    const QString bid = broker_id_;
    QPointer<AccountDataStream> self = this;

    auto creds = AccountManager::instance().load_credentials(acct_id);
    if (creds.api_key.isEmpty()) return;

    (void)QtConcurrent::run([self, acct_id, bid, symbol, creds]() {
        auto* broker = BrokerRegistry::instance().get(bid);
        if (!broker) return;
        const QString today = QDate::currentDate().toString("yyyy-MM-dd");
        auto result = broker->get_historical_trades_single(creds, symbol, today + "T00:00:00Z", "", 500);
        if (!result.success || !result.data) return;
        QMetaObject::invokeMethod(self, [self, acct_id, data = *result.data]() {
            if (!self) return;
            emit self->time_sales_fetched(acct_id, data);
        }, Qt::QueuedConnection);
    });
}

void AccountDataStream::fetch_latest_trade(const QString& symbol) {
    const QString acct_id = account_id_;
    const QString bid = broker_id_;
    QPointer<AccountDataStream> self = this;

    auto creds = AccountManager::instance().load_credentials(acct_id);
    if (creds.api_key.isEmpty()) return;

    (void)QtConcurrent::run([self, acct_id, bid, symbol, creds]() {
        auto* broker = BrokerRegistry::instance().get(bid);
        if (!broker) return;
        auto result = broker->get_latest_trade(creds, symbol);
        if (!result.success || !result.data) return;
        QMetaObject::invokeMethod(self, [self, acct_id, data = *result.data]() {
            if (!self) return;
            emit self->latest_trade_fetched(acct_id, data);
        }, Qt::QueuedConnection);
    });
}

void AccountDataStream::fetch_calendar() {
    const QString acct_id = account_id_;
    const QString bid = broker_id_;
    QPointer<AccountDataStream> self = this;

    auto creds = AccountManager::instance().load_credentials(acct_id);

    (void)QtConcurrent::run([self, acct_id, bid, creds]() {
        auto* broker = BrokerRegistry::instance().get(bid);
        if (!broker) return;
        const QString start = QDate::currentDate().addDays(-5).toString("yyyy-MM-dd");
        const QString end = QDate::currentDate().addDays(30).toString("yyyy-MM-dd");
        auto result = broker->get_calendar(creds, start, end);
        if (!result.success || !result.data) return;
        QMetaObject::invokeMethod(self, [self, acct_id, data = *result.data]() {
            if (!self) return;
            emit self->calendar_fetched(acct_id, data);
        }, Qt::QueuedConnection);
    });
}

void AccountDataStream::fetch_clock() {
    const QString acct_id = account_id_;
    const QString bid = broker_id_;
    QPointer<AccountDataStream> self = this;

    auto creds = AccountManager::instance().load_credentials(acct_id);

    (void)QtConcurrent::run([self, acct_id, bid, creds]() {
        auto* broker = BrokerRegistry::instance().get(bid);
        if (!broker) return;
        auto result = broker->get_clock(creds);
        if (!result.success || !result.data) return;
        QMetaObject::invokeMethod(self, [self, acct_id, data = *result.data]() {
            if (!self) return;
            emit self->clock_fetched(acct_id, data);
        }, Qt::QueuedConnection);
    });
}

// ── WebSocket ───────────────────────────────────────────────────────────────

void AccountDataStream::ws_init() {
    if (broker_id_ == "alpaca") {
        auto creds = AccountManager::instance().load_credentials(account_id_);
        if (creds.api_key.isEmpty() || creds.api_secret.isEmpty())
            return;

        auto* aws = new AlpacaWebSocket(creds.api_key, creds.api_secret, this);
        ws_ = aws;

        connect(aws, &AlpacaWebSocket::tick_received, this, [this](const BrokerQuote& q) {
            ++ws_tick_count_;
            quote_cache_[q.symbol] = q;
            emit quote_updated(account_id_, q.symbol, q);
        });

        connect(aws, &AlpacaWebSocket::trade_received, this, [this](const BrokerTrade& trade) {
            auto it = quote_cache_.find(trade.symbol);
            if (it != quote_cache_.end()) {
                it->ltp = trade.price;
                const auto dt = QDateTime::fromString(trade.timestamp, Qt::ISODateWithMs);
                it->timestamp = dt.isValid() ? dt.toMSecsSinceEpoch() : 0;
                emit quote_updated(account_id_, trade.symbol, it.value());
            }
        });

        connect(aws, &AlpacaWebSocket::bar_received, this,
                [this](const QString& symbol, const BrokerCandle& bar) {
            auto it = quote_cache_.find(symbol);
            if (it != quote_cache_.end()) {
                it->open = bar.open;
                it->high = bar.high;
                it->low = bar.low;
                it->close = bar.close;
                it->volume = bar.volume;
                it->change = bar.close - bar.open;
                it->change_pct = bar.open > 0 ? ((bar.close - bar.open) / bar.open) * 100.0 : 0;
                emit quote_updated(account_id_, symbol, it.value());
            }
        });

        connect(aws, &AlpacaWebSocket::connected, this, [this]() {
            LOG_INFO(ADS_TAG, QString("Alpaca WS connected for %1").arg(account_id_));
            emit connection_state_changed(account_id_, ConnectionState::Connected);
            QStringList symbols;
            if (!selected_symbol_.isEmpty())
                symbols.append(selected_symbol_);
            for (const QString& s : subscribed_symbols()) {
                if (!symbols.contains(s))
                    symbols.append(s);
            }
            if (auto* a = qobject_cast<AlpacaWebSocket*>(ws_))
                a->subscribe(symbols);
        });

        connect(aws, &AlpacaWebSocket::disconnected, this, [this]() {
            LOG_WARN(ADS_TAG, QString("Alpaca WS disconnected for %1").arg(account_id_));
            emit connection_state_changed(account_id_, ConnectionState::Disconnected);
        });

        connect(aws, &AlpacaWebSocket::error_occurred, this, [this](const QString& err) {
            LOG_ERROR(ADS_TAG, QString("Alpaca WS error: %1").arg(err));
            check_token_expiry(err);
        });

        connect(aws, &AlpacaWebSocket::market_closed, this, [this]() {
            LOG_INFO(ADS_TAG, QString("Alpaca market closed for %1 — falling back to polling").arg(account_id_));
        });

        aws->open();
        return;
    }
}

void AccountDataStream::ws_teardown() {
    if (!ws_)
        return;
    // The WS object will be an AlpacaWebSocket — call close and deleteLater.
    QMetaObject::invokeMethod(ws_, "close");
    ws_->deleteLater();
    ws_ = nullptr;
}

bool AccountDataStream::ws_active() const {
    if (!ws_)
        return false;
    if (auto* aws = qobject_cast<AlpacaWebSocket*>(ws_))
        return aws->is_connected() && ws_tick_count_ > 0;
    return false;
}

bool AccountDataStream::ws_connected() const {
    // Like ws_active() but without the tick-count requirement — used to decide
    // whether a (re)subscribe can be sent right now.
    if (!ws_)
        return false;
    if (auto* aws = qobject_cast<AlpacaWebSocket*>(ws_))
        return aws->is_connected();
    return false;
}

void AccountDataStream::ws_resubscribe() {
    if (auto* aws = qobject_cast<AlpacaWebSocket*>(ws_)) {
        QStringList symbols;
        if (!selected_symbol_.isEmpty())
            symbols.append(selected_symbol_);
        for (const QString& s : subscribed_symbols()) {
            if (!symbols.contains(s))
                symbols.append(s);
        }
        aws->set_subscriptions(symbols);
    }
}

} // namespace openmarketterminal::trading
