// src/screens/crypto_trading/CryptoTradingScreen_AsyncFetch.cpp
//
// QtConcurrent-driven REST fetches: candles, live positions/orders/balance,
// my_trades, trading_fees, mark_price, set_leverage, set_margin_mode.
//
// Part of the partial-class split of CryptoTradingScreen.cpp.

#include "screens/crypto_trading/CryptoTradingScreen.h"

#include "core/logging/Logger.h"
#include "core/session/ScreenStateManager.h"
#include "core/symbol/SymbolContext.h"
#include "screens/crypto_trading/CryptoBottomPanel.h"
#include "screens/crypto_trading/CryptoChart.h"
#include "screens/crypto_trading/CryptoCredentials.h"
#include "screens/crypto_trading/CryptoOrderBook.h"
#include "screens/crypto_trading/CryptoOrderEntry.h"
#include "screens/crypto_trading/CryptoSymbolUniverse.h"
#include "screens/crypto_trading/CryptoTickerBar.h"
#include "screens/crypto_trading/CryptoWatchlist.h"
#include "trading/ExchangeService.h"
#include "trading/ExchangeSession.h"
#include "trading/ExchangeSessionManager.h"
#include "trading/OrderMatcher.h"
#include "trading/PaperTrading.h"
#include "ui/theme/StyleSheets.h"
#include "ui/theme/Theme.h"

#include <QCompleter>
#include <QDateTime>
#include <QHBoxLayout>
#include <QPointer>
#include <QSplitter>
#include <QStringListModel>
#include <QStyle>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

namespace openmarketterminal::screens {

using namespace openmarketterminal::trading;
using namespace openmarketterminal::screens::crypto;

void CryptoTradingScreen::async_fetch_candles(const QString& symbol, const QString& timeframe) {
    if (candles_fetching_.exchange(true))
        return;
    QPointer<CryptoTradingScreen> self = this;
    (void)QtConcurrent::run([self, symbol, timeframe]() {
        auto candles = ExchangeService::instance().fetch_ohlcv(symbol, timeframe, OHLCV_FETCH_COUNT);
        if (!self)
            return;
        self->candles_fetching_ = false;
        QMetaObject::invokeMethod(
            self,
            [self, candles]() {
                if (!self)
                    return;
                self->chart_->set_candles(candles);
            },
            Qt::QueuedConnection);
    });
}

void CryptoTradingScreen::async_fetch_live_positions() {
    QPointer<CryptoTradingScreen> self = this;
    (void)QtConcurrent::run([self]() {
        if (!self) {
            // Widget destroyed before dispatch — no counter to decrement.
            return;
        }
        auto result = ExchangeService::instance().fetch_positions_live(self->selected_symbol_);
        QMetaObject::invokeMethod(
            self,
            [self, result]() {
                if (!self)
                    return;
                if (result.contains("positions")) {
                    const QJsonArray positions = result.value("positions").toArray();
                    // Spot venues return no positions; the POS tab is owned by
                    // the holdings view then (refresh_live_holdings) — don't
                    // stomp it with an empty perp-shaped array.
                    if (!positions.isEmpty() || self->is_perp_market())
                        self->bottom_panel_->set_live_positions(positions);
                }
                self->live_inflight_.fetch_sub(1);
            },
            Qt::QueuedConnection);
    });
}

void CryptoTradingScreen::async_fetch_live_orders() {
    QPointer<CryptoTradingScreen> self = this;
    (void)QtConcurrent::run([self]() {
        if (!self)
            return;
        auto result = ExchangeService::instance().fetch_open_orders_live(self->selected_symbol_);
        QMetaObject::invokeMethod(
            self,
            [self, result]() {
                if (!self)
                    return;
                if (result.contains("orders")) {
                    const QJsonArray orders = result.value("orders").toArray();
                    self->bottom_panel_->set_live_orders(orders);
                    // REST is the source of truth — rebuild the account-WS
                    // fast-path accumulator so a missed WS event can't leave
                    // a phantom open order behind.
                    self->live_orders_by_id_.clear();
                    for (const auto& v : orders) {
                        const QJsonObject o = v.toObject();
                        const QString id = o.value("id").toString();
                        if (!id.isEmpty())
                            self->live_orders_by_id_.insert(id, o);
                    }
                }
                self->live_inflight_.fetch_sub(1);
            },
            Qt::QueuedConnection);
    });
}

void CryptoTradingScreen::async_fetch_live_balance() {
    QPointer<CryptoTradingScreen> self = this;
    (void)QtConcurrent::run([self]() {
        if (!self)
            return;
        auto result = ExchangeService::instance().fetch_balance();
        // The daemon returns {"balances": {CUR: {free, used, total}}} with zero
        // balances filtered out, or {"error": ...} on a failed authenticated
        // call. A clean fetch is the truthful "live connection works" signal —
        // WS being up does not mean the account is reachable.
        const bool authed = !result.contains("error");
        if (!authed) {
            LOG_WARN("CryptoTrading", "Live balance error: " + result.value("error").toString());
        } else {
            // Keys only — never amounts or credentials. DEBUG: fires every poll.
            LOG_DEBUG("CryptoTrading", "Live balance currencies: " +
                                           QStringList(result.value("balances").toObject().keys()).join(", "));
        }
        QMetaObject::invokeMethod(
            self,
            [self, result, authed]() {
                if (!self)
                    return;
                self->set_live_auth_indicator(authed);
                self->apply_live_balance_display(result.value("balances").toObject());
                self->live_inflight_.fetch_sub(1);
            },
            Qt::QueuedConnection);
    });
}

void CryptoTradingScreen::apply_live_balance_display(const QJsonObject& balances) {
    // Pick the cash/quote currency to display. Try the displayed pair's quote
    // first, then common fiat/stablecoins, then fall back to the single
    // largest holding. Shared by the REST poll and the account-WS fast path.
    const QString pair_quote = selected_symbol_.section('/', 1, 1).toUpper();
    QString ccy;
    for (const QString& cand : {pair_quote, QStringLiteral("USD"), QStringLiteral("USDC"),
                                QStringLiteral("USDT"), QStringLiteral("USDE")}) {
        if (!cand.isEmpty() && balances.contains(cand)) {
            ccy = cand;
            break;
        }
    }
    if (ccy.isEmpty() && !balances.isEmpty())
        ccy = balances.keys().first();

    const QJsonObject b = balances.value(ccy).toObject();
    const double total = b.value("total").toDouble();
    const double free = b.value("free").toDouble();
    const double used = b.value("used").toDouble();
    bottom_panel_->set_live_balance(free, total, used);
    order_entry_->set_balance(free, ccy);

    last_balances_ = balances;
    refresh_live_holdings();
}

void CryptoTradingScreen::refresh_live_holdings() {
    // Spot venues expose no positions — the POS tab renders account holdings
    // instead (asset qty x live mark). Perp venues keep the real position feed.
    if (trading_mode_ != TradingMode::Live || is_perp_market() || !bottom_panel_)
        return;
    static const QSet<QString> kQuoteCurrencies = {
        QStringLiteral("USD"),  QStringLiteral("USDC"), QStringLiteral("USDT"),
        QStringLiteral("USDE"), QStringLiteral("EUR"),  QStringLiteral("GBP"),
    };
    const QString quote = openmarketterminal::crypto::quote_currency_for(exchange_id_);
    QJsonArray holdings;
    for (auto it = last_balances_.constBegin(); it != last_balances_.constEnd(); ++it) {
        if (kQuoteCurrencies.contains(it.key()))
            continue;
        const double qty = it.value().toObject().value(QStringLiteral("total")).toDouble();
        if (qty <= 0.0)
            continue;
        const QString symbol = it.key() + QLatin1Char('/') + quote;
        double price = 0.0;
        const auto cached = pending_tickers_.constFind(symbol);
        if (cached != pending_tickers_.constEnd() && cached->last > 0)
            price = cached->last;
        else
            price = ExchangeService::instance().get_cached_price(symbol).last;
        QJsonObject row{{QStringLiteral("symbol"), symbol},
                        {QStringLiteral("qty"), qty},
                        {QStringLiteral("price"), price}};
        // Est. avg entry / unrealized P&L exist only for the selected pair,
        // seeded from its trade history (CryptoLiveOverlay).
        if (symbol == selected_symbol_ && live_avg_entry_.avg_entry() > 0 && price > 0) {
            row.insert(QStringLiteral("avg_entry"), live_avg_entry_.avg_entry());
            row.insert(QStringLiteral("upnl_valid"), true);
            row.insert(QStringLiteral("upnl"), (price - live_avg_entry_.avg_entry()) * qty);
        }
        holdings.append(row);
    }
    bottom_panel_->set_live_holdings(holdings);
}

// ============================================================================
// Slot Handlers
// ============================================================================

void CryptoTradingScreen::async_fetch_my_trades() {
    QPointer<CryptoTradingScreen> self = this;
    (void)QtConcurrent::run([self]() {
        if (!self)
            return;
        auto result = ExchangeService::instance().fetch_my_trades(self->selected_symbol_);
        QMetaObject::invokeMethod(
            self,
            [self, result]() {
                if (!self)
                    return;
                self->bottom_panel_->update_my_trades(result);
                // Re-seed the est-avg-entry accumulator from REST history so
                // the ladder marker is right without waiting for new fills.
                if (result.contains("trades")) {
                    QVector<QJsonObject> rows;
                    for (const auto& v : result.value("trades").toArray())
                        rows.append(v.toObject());
                    std::sort(rows.begin(), rows.end(), [](const QJsonObject& a, const QJsonObject& b) {
                        return a.value("timestamp").toDouble() < b.value("timestamp").toDouble();
                    });
                    self->live_avg_entry_.reset();
                    for (const QJsonObject& t : rows) {
                        if (t.value("symbol").toString() == self->selected_symbol_ ||
                            t.value("symbol").toString().isEmpty())
                            self->live_avg_entry_.add_trade(t.value("side").toString(),
                                                            t.value("price").toDouble(),
                                                            t.value("amount").toDouble());
                    }
                    self->refresh_live_ladder_overlay();
                }
                self->live_inflight_.fetch_sub(1);
            },
            Qt::QueuedConnection);
    });
}

void CryptoTradingScreen::async_fetch_trading_fees() {
    QPointer<CryptoTradingScreen> self = this;
    (void)QtConcurrent::run([self]() {
        if (!self)
            return;
        auto result = ExchangeService::instance().fetch_trading_fees(self->selected_symbol_);
        QMetaObject::invokeMethod(
            self,
            [self, result]() {
                if (!self)
                    return;
                self->bottom_panel_->update_fees(result);
            },
            Qt::QueuedConnection);
    });
}

void CryptoTradingScreen::async_fetch_mark_price() {
    QPointer<CryptoTradingScreen> self = this;
    (void)QtConcurrent::run([self]() {
        if (!self)
            return;
        auto mp = ExchangeService::instance().fetch_mark_price(self->selected_symbol_);
        QMetaObject::invokeMethod(
            self,
            [self, mp]() {
                if (!self)
                    return;
                self->ticker_bar_->update_mark_price(mp.mark_price, mp.index_price);
            },
            Qt::QueuedConnection);
    });
}

void CryptoTradingScreen::async_set_leverage(int leverage) {
    const QString symbol = selected_symbol_;
    (void)QtConcurrent::run([symbol, leverage]() { ExchangeService::instance().set_leverage(symbol, leverage); });
}

void CryptoTradingScreen::async_set_margin_mode(const QString& mode) {
    const QString symbol = selected_symbol_;
    const QString m = mode;
    (void)QtConcurrent::run([symbol, m]() { ExchangeService::instance().set_margin_mode(symbol, m); });
}

// ── IStatefulScreen ───────────────────────────────────────────────────────────
} // namespace openmarketterminal::screens
