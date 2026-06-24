// src/screens/equity_trading/EquityTradingScreen_Streams.cpp
//
// DataHub subscription wiring (D4 migration) + on-demand legacy signal slots.
//
// Streaming data (quotes, positions, holdings, orders, funds) is consumed
// via DataHub::subscribe() instead of direct DataStreamManager signals.
// On-demand / one-shot data (candles, orderbook, time/sales, calendar, clock)
// remains wired as legacy signals — no hub topic exists for those.
//
// Part of the partial-class split of EquityTradingScreen.cpp.

#include "screens/equity_trading/EquityTradingScreen.h"

#include "core/logging/Logger.h"
#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "services/markets/MarketDataService.h"
#include "screens/equity_trading/EquityBottomPanel.h"
#include "screens/equity_trading/EquityChartPanel.h"
#include "screens/equity_trading/EquityOrderBook.h"
#include "screens/equity_trading/EquityOrderEntry.h"
#include "screens/equity_trading/EquityTickerBar.h"
#include "screens/equity_trading/EquityWatchlist.h"
#include "trading/AccountManager.h"
#include "trading/BrokerRegistry.h"
#include "trading/BrokerTopic.h"
#include "trading/DataStreamManager.h"
#include "trading/OrderMatcher.h"
#include "trading/PaperTrading.h"

#include <QDateTime>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QtConcurrent>

namespace openmarketterminal::screens {

using namespace openmarketterminal::trading;
using namespace openmarketterminal::screens::equity;

static const QString TAG = "EquityTrading";

// Paper position prices are persisted to SQLite at most once per this window per
// burst of ticks (coalesced), instead of one UPDATE per tick. The in-memory table
// and chart card still update on every tick; only the DB write is throttled.
static constexpr int kPaperPriceFlushMs = 1000;

// ============================================================================
// DataHub subscription helpers (D4)
// ============================================================================

QString EquityTradingScreen::broker_id_for_focused() const {
    if (focused_account_id_.isEmpty())
        return {};
    return AccountManager::instance().get_account(focused_account_id_).broker_id;
}

void EquityTradingScreen::hub_subscribe_streaming() {
    auto& hub = datahub::DataHub::instance();
    hub.unsubscribe(this);
    hub_active_ = false;

    // Stop any running broker quote poll so re-subscribes (focus/symbol/mode
    // change) don't stack multiple timers. Restarted below for live accounts.
    if (quote_poll_timer_)
        quote_poll_timer_->stop();

    if (focused_account_id_.isEmpty())
        return;
    const QString aid = focused_account_id_;

    // Cache the trading mode + paper portfolio id once here (read on every
    // focus/symbol/mode change; keeps the per-tick quote handler off
    // AccountManager's mutex).
    const auto focused_account = AccountManager::instance().get_account(aid);
    const bool is_paper = focused_account.trading_mode == QLatin1String("paper");
    focused_is_paper_ = is_paper;
    focused_paper_portfolio_id_ = is_paper ? focused_account.paper_portfolio_id : QString();
    pending_paper_prices_.clear();

    // Paper mode is broker-independent: quotes — and therefore paper fills, which
    // run inside the quote handler — come from the free MarketDataService feed
    // (the same yfinance source the Markets screen uses). No broker or API key
    // needed, so paper trading works out of the box.
    if (is_paper) {
        hub_subscribe_market_quotes();
        // Load chart candles here too: on_account_changed gates its candle fetch
        // behind has_creds (false for paper), so the chart would otherwise stay
        // empty. focused_is_paper_ is set above, so this routes to yfinance history.
        load_candles_for(selected_symbol_);
        hub_active_ = true;
        LOG_INFO(TAG, QString("Hub subscribed (paper / free market feed): %1").arg(aid));
        return;
    }

    // ── Live mode: requires a connected broker ──
    const QString bid = broker_id_for_focused();
    if (bid.isEmpty())
        return;

    // Live broker portfolio topics (positions/holdings/orders/balance). Paper
    // returned above — its portfolio comes from the paper engine, not these.
    {
        // ── Positions ──
        hub.subscribe(this, broker_topic(bid, aid, QStringLiteral("positions")),
                      [this](const QVariant& v) {
                          if (!v.canConvert<QVector<BrokerPosition>>())
                              return;
                          const auto positions = v.value<QVector<BrokerPosition>>();
                          bottom_panel_->set_positions(positions);
                          live_positions_ = positions; // cache for the chart overlay
                          QStringList ps;
                          for (const auto& p : positions)
                              ps << p.symbol;
                          update_position_symbols(ps); // give held symbols live WS prices
                          update_chart_position();     // refresh on-chart position card
                      });

        // ── Holdings ──
        hub.subscribe(this, broker_topic(bid, aid, QStringLiteral("holdings")),
                      [this](const QVariant& v) {
                          if (!v.canConvert<QVector<BrokerHolding>>())
                              return;
                          bottom_panel_->set_holdings(v.value<QVector<BrokerHolding>>());
                      });

        // ── Orders ──
        hub.subscribe(this, broker_topic(bid, aid, QStringLiteral("orders")),
                      [this](const QVariant& v) {
                          if (!v.canConvert<QVector<BrokerOrderInfo>>())
                              return;
                          bottom_panel_->set_orders(v.value<QVector<BrokerOrderInfo>>());
                      });

        // ── Balance / Funds ──
        // Resolve the broker's currency symbol once (₹/$/…) for the funds cards.
        const QString ccy_sym = [&]() {
            if (auto* b = BrokerRegistry::instance().get(bid))
                return currency_symbol(b->profile().currency);
            return QStringLiteral("$");
        }();
        bottom_panel_->set_currency(ccy_sym); // so the positions P&L summary formats correctly
        hub.subscribe(this, broker_topic(bid, aid, QStringLiteral("balance")),
                      [this, ccy_sym](const QVariant& v) {
                          if (!v.canConvert<BrokerFunds>())
                              return;
                          const auto funds = v.value<BrokerFunds>();
                          EquityFundsView fv;
                          fv.is_paper = false;
                          fv.currency = ccy_sym;
                          fv.available = funds.available_balance;
                          fv.used_margin = funds.used_margin;
                          fv.total_equity = funds.total_balance;
                          fv.collateral = funds.collateral;
                          const double denom = funds.used_margin + funds.available_balance;
                          fv.margin_util_pct = denom > 0.0 ? (funds.used_margin / denom) * 100.0 : 0.0;
                          bottom_panel_->set_funds_view(fv);
                          order_entry_->set_balance(funds.available_balance);
                      });
    }

    hub_active_ = true;

    // Live broker accounts: poll the broker's reliable snapshot quotes
    // (e.g. Alpaca data.alpaca.markets) for the ticker/watchlist/chart, instead
    // of the flaky free yfinance feed. Paper mode (returned above) keeps yfinance.
    // hub_active_ is set above so the immediate fire passes poll_broker_quotes()'s guard.
    if (!quote_poll_timer_) {
        quote_poll_timer_ = new QTimer(this);
        quote_poll_timer_->setInterval(4000);  // 4s; Alpaca data is fine at this cadence
        connect(quote_poll_timer_, &QTimer::timeout, this, &EquityTradingScreen::poll_broker_quotes);
    }
    poll_broker_quotes();  // fire once immediately (don't wait 4s for the first quote)
    quote_poll_timer_->start();

    LOG_INFO(TAG, QString("Hub subscribed: %1 / %2").arg(bid, aid));
}

// Per-tick UI + paper-engine update for one symbol. Shared by the free
// market-data feed used in both paper and live mode (hub_subscribe_market_quotes).
void EquityTradingScreen::apply_equity_quote(const QString& sym, const BrokerQuote& quote) {
    // Open-positions table (bottom sheet): patch LTP / P&L from the SAME quote
    // that feeds the ticker bar, so the header and the position row never show
    // different prices.
    bottom_panel_->update_quote(sym, quote);

    // Ticker bar + order entry + chart for the selected symbol.
    if (sym == selected_symbol_) {
        current_price_ = quote.ltp;
        ticker_bar_->update_quote(quote.ltp, quote.change, quote.change_pct,
                                  quote.high, quote.low, quote.volume,
                                  quote.bid, quote.ask);
        order_entry_->set_current_price(quote.ltp);
        chart_->on_quote(quote);       // roll the tick into the live forming candle
        chart_->update_pnl(quote.ltp); // live P&L on the chart's position card
    }

    // Repaint only this symbol's watchlist row (no-op if not in the active list).
    watchlist_->update_quote(quote);

    // Feed the paper trading engine (mode + portfolio cached at subscribe time).
    if (focused_is_paper_ && !focused_paper_portfolio_id_.isEmpty() && quote.ltp > 0) {
        pending_paper_prices_[sym] = quote.ltp;
        if (!paper_flush_armed_) {
            paper_flush_armed_ = true;
            QTimer::singleShot(kPaperPriceFlushMs, this, [this]() { flush_paper_prices(); });
        }
        // SL/TP + limit matching must see EVERY tick (in-memory; no DB per tick).
        PriceData pd;
        pd.last = quote.ltp;
        pd.bid = quote.bid;
        pd.ask = quote.ask;
        pd.timestamp = quote.timestamp;
        OrderMatcher::instance().check_orders(sym, pd, focused_paper_portfolio_id_);
        OrderMatcher::instance().check_sl_tp_triggers(focused_paper_portfolio_id_, sym, quote.ltp);
    }
}

// Paper mode: subscribe the watchlist/ticker symbols to the free MarketDataService
// quote feed (market:quote:<sym>, yfinance) and route each tick through the same
// apply_equity_quote() path the broker feed uses — so quotes, the chart's live
// candle, and paper fills all work with no broker connected.
void EquityTradingScreen::hub_subscribe_market_quotes() {
    auto& hub = datahub::DataHub::instance();

    QSet<QString> symbols;
    symbols.insert(selected_symbol_);
    for (const auto& s : watchlist_symbols_)
        symbols.insert(s);
    for (const auto& s : position_symbols_)
        symbols.insert(s);

    for (const auto& sym : symbols) {
        if (sym.isEmpty())
            continue;
        // Crypto pairs are stored Alpaca-style (BASE/USD) but the free yfinance
        // feed wants BASE-USD — request under the feed symbol, then map the tick
        // back to the original so it lands on the right watchlist row.
        const QString feed_sym =
            sym.contains(QLatin1Char('/')) ? QString(sym).replace(QLatin1Char('/'), QLatin1Char('-')) : sym;
        const QString topic = QStringLiteral("market:quote:") + feed_sym;
        hub.subscribe(this, topic, [this, sym](const QVariant& v) {
            if (!v.canConvert<services::QuoteData>())
                return;
            const auto q = v.value<services::QuoteData>();
            BrokerQuote bq;
            bq.symbol = sym;
            bq.ltp = q.price;
            bq.change = q.change;
            bq.change_pct = q.change_pct;
            bq.high = q.high;
            bq.low = q.low;
            bq.volume = q.volume;
            // yfinance quotes carry no bid/ask depth — leave 0 (ticker shows "—").
            bq.timestamp = QDateTime::currentMSecsSinceEpoch();
            apply_equity_quote(sym, bq);
        });
        hub.request(topic);  // kick a fetch so the card fills immediately
    }
}

// Live mode: fetch the focused broker's reliable snapshot quotes (Alpaca data,
// etc.) off the main thread and route each through apply_equity_quote() on the
// main thread — the same handler the free yfinance feed uses. Replaces the flaky
// free feed for connected brokers. Called immediately on (re)subscribe and every
// 4s by quote_poll_timer_.
void EquityTradingScreen::poll_broker_quotes() {
    if (!hub_active_ || focused_is_paper_ || focused_account_id_.isEmpty())
        return;
    const QString bid = broker_id_for_focused();
    if (bid.isEmpty())
        return;
    trading::IBroker* broker = BrokerRegistry::instance().get(bid);
    if (!broker)
        return;
    // Gather the symbols to quote: selected + watchlist + positions (dedup, skip empty).
    QSet<QString> set;
    set.insert(selected_symbol_);
    for (const auto& s : watchlist_symbols_)
        set.insert(s);
    for (const auto& s : position_symbols_)
        set.insert(s);
    QVector<QString> symbols;
    for (const auto& s : set)
        if (!s.isEmpty())
            symbols.append(s);
    if (symbols.isEmpty())
        return;
    // Load creds on the main thread (cheap, cached), pass to the worker.
    const auto creds = AccountManager::instance().load_credentials(focused_account_id_);
    QPointer<EquityTradingScreen> self = this;
    (void)QtConcurrent::run([self, broker, creds, symbols]() {
        if (!self)
            return;
        auto resp = broker->get_quotes(creds, symbols);  // blocking HTTP — off main thread
        QMetaObject::invokeMethod(self, [self, resp]() {
            if (!self)
                return;
            if (resp.success && resp.data.has_value()) {
                for (const auto& q : resp.data.value())
                    self->apply_equity_quote(q.symbol, q);
            }
        });
    });
}

// Route a candle request to the right source: paper → free MarketDataService
// history (yfinance); live → the connected broker's candle endpoint.
void EquityTradingScreen::load_candles_for(const QString& symbol) {
    if (symbol.isEmpty())
        return;
    const auto account = AccountManager::instance().get_account(focused_account_id_);
    const bool is_paper = account.account_id.isEmpty() || account.trading_mode == QLatin1String("paper");
    if (is_paper) {
        fetch_paper_candles(symbol);
        return;
    }
    if (auto* stream = DataStreamManager::instance().stream_for(focused_account_id_))
        stream->fetch_candles(symbol, chart_->current_timeframe());
}

void EquityTradingScreen::fetch_paper_candles(const QString& symbol) {
    // Map the chart timeframe to a yfinance (period, interval) pair.
    const QString tf = chart_->current_timeframe();
    QString period, interval;
    if (tf == QLatin1String("1m"))       { period = "5d";  interval = "1m"; }
    else if (tf == QLatin1String("5m"))  { period = "1mo"; interval = "5m"; }
    else if (tf == QLatin1String("15m")) { period = "1mo"; interval = "15m"; }
    else if (tf == QLatin1String("1h"))  { period = "3mo"; interval = "1h"; }
    else if (tf == QLatin1String("1d"))  { period = "2y";  interval = "1d"; }
    else if (tf == QLatin1String("1w"))  { period = "5y";  interval = "1wk"; }
    else                                 { period = "1mo"; interval = "15m"; }

    // Crypto pairs are stored Alpaca-style (BASE/USD); yfinance history wants
    // BASE-USD. Fetch under the feed symbol but key the result off the original.
    const QString feed_sym =
        symbol.contains(QLatin1Char('/')) ? QString(symbol).replace(QLatin1Char('/'), QLatin1Char('-')) : symbol;

    QPointer<EquityTradingScreen> self = this;
    services::MarketDataService::instance().fetch_history(
        feed_sym, period, interval,
        [self, symbol](bool ok, QVector<services::HistoryPoint> pts) {
            if (!self || !ok || !self->focused_is_paper_ || symbol != self->selected_symbol_)
                return;
            QVector<BrokerCandle> candles;
            candles.reserve(pts.size());
            for (const auto& p : pts) {
                BrokerCandle c;
                c.timestamp = p.timestamp * 1000;  // HistoryPoint=secs, chart wants ms
                c.open = p.open;
                c.high = p.high;
                c.low = p.low;
                c.close = p.close;
                c.volume = static_cast<double>(p.volume);
                candles.append(c);
            }
            self->chart_->set_candles(candles);
        });
}

void EquityTradingScreen::hub_unsubscribe_all() {
    if (quote_poll_timer_)
        quote_poll_timer_->stop();  // stop the live broker quote poll on hide/teardown
    if (!hub_active_)
        return;
    datahub::DataHub::instance().unsubscribe(this);
    hub_active_ = false;
    LOG_INFO(TAG, "Hub unsubscribed");
}

void EquityTradingScreen::update_chart_position() {
    if (!chart_)
        return;
    const auto account = AccountManager::instance().get_account(focused_account_id_);
    const bool is_paper = account.trading_mode == QLatin1String("paper");

    if (is_paper) {
        if (account.paper_portfolio_id.isEmpty()) {
            chart_->clear_position();
            return;
        }
        for (const auto& p : pt_get_positions(account.paper_portfolio_id)) {
            if (p.symbol == selected_symbol_ && qAbs(p.quantity) > 0.0) {
                chart_->set_position(selected_symbol_, p.side, qAbs(p.quantity), p.entry_price,
                                     selected_exchange_, QString());
                if (current_price_ > 0.0)
                    chart_->update_pnl(current_price_);
                return;
            }
        }
        chart_->clear_position();
        return;
    }

    // Live: scan the cached broker positions for the displayed symbol. P&L is in
    // the broker's own currency (intrinsic), not the global preference.
    QString ccy;
    if (auto* broker = BrokerRegistry::instance().get(account.broker_id))
        ccy = broker->profile().currency;
    for (const auto& p : live_positions_) {
        if (p.symbol == selected_symbol_ && qAbs(p.quantity) > 0.0) {
            QString side = p.side.toLower();
            if (side != QLatin1String("long") && side != QLatin1String("short"))
                side = p.quantity >= 0 ? QStringLiteral("long") : QStringLiteral("short");
            chart_->set_position(selected_symbol_, side, qAbs(p.quantity), p.avg_price,
                                 p.exchange.isEmpty() ? selected_exchange_ : p.exchange,
                                 p.product_type, ccy);
            if (current_price_ > 0.0)
                chart_->update_pnl(current_price_);
            return;
        }
    }
    chart_->clear_position();
}

// ============================================================================
// On-demand legacy signal slots (no hub topic — D4 exception)
// ============================================================================

void EquityTradingScreen::on_stream_candles_fetched(const QString& account_id,
                                                     const QVector<BrokerCandle>& candles) {
    if (account_id == focused_account_id_)
        chart_->set_candles(candles);
}

void EquityTradingScreen::on_stream_orderbook_fetched(const QString& account_id,
                                                       const QVector<QPair<double, double>>& bids,
                                                       const QVector<QPair<double, double>>& asks,
                                                       double spread, double spread_pct,
                                                       const QVector<int>& bid_orders,
                                                       const QVector<int>& ask_orders) {
    if (account_id == focused_account_id_)
        orderbook_->set_data(bids, asks, spread, spread_pct, bid_orders, ask_orders);
}

void EquityTradingScreen::on_stream_time_sales_fetched(const QString& account_id,
                                                        const QVector<BrokerTrade>& trades) {
    if (account_id == focused_account_id_)
        bottom_panel_->set_time_sales(trades);
}

void EquityTradingScreen::on_stream_latest_trade_fetched(const QString& account_id,
                                                          const BrokerTrade& trade) {
    if (account_id == focused_account_id_)
        bottom_panel_->prepend_trade(trade);
}

void EquityTradingScreen::on_stream_calendar_fetched(const QString& account_id,
                                                      const QVector<MarketCalendarDay>& days) {
    if (account_id == focused_account_id_)
        bottom_panel_->set_calendar(days);
}

void EquityTradingScreen::on_stream_clock_fetched(const QString& account_id, const MarketClock& clock) {
    if (account_id == focused_account_id_)
        bottom_panel_->set_clock(clock);
}

} // namespace openmarketterminal::screens
