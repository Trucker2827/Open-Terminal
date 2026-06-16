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
                if (result.contains("positions"))
                    self->bottom_panel_->set_live_positions(result.value("positions").toArray());
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
                if (result.contains("orders"))
                    self->bottom_panel_->set_live_orders(result.value("orders").toArray());
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
                const QJsonObject balances = result.value("balances").toObject();
                // Pick the cash/quote currency to display. The displayed pair's
                // quote (e.g. USDT) may be remapped on the exchange — Coinbase
                // settles in USD — so try the pair quote first, then common
                // fiat/stablecoins, then fall back to the single largest holding.
                const QString pair_quote = self->selected_symbol_.section('/', 1, 1).toUpper();
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
                self->bottom_panel_->set_live_balance(free, total, used);
                self->order_entry_->set_balance(free, ccy);
                self->live_inflight_.fetch_sub(1);
            },
            Qt::QueuedConnection);
    });
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
