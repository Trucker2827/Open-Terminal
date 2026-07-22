// src/screens/crypto_trading/CryptoTradingScreen_Handlers.cpp
//
// User-action handlers: exchange/symbol switching, mode toggle, API config,
// order submit/cancel, order-book click, search.
//
// Part of the partial-class split of CryptoTradingScreen.cpp.

#include "screens/crypto_trading/CryptoTradingScreen.h"

#include "core/logging/Logger.h"
#include "core/session/ScreenStateManager.h"
#include "core/symbol/SymbolContext.h"
#include "screens/crypto_trading/CryptoBottomPanel.h"
#include "screens/crypto_trading/CryptoChart.h"
#include "screens/crypto_trading/CryptoCredentials.h"
#include "screens/crypto_trading/CryptoLadder.h"
#include "screens/crypto_trading/CryptoOrderBook.h"
#include "screens/crypto_trading/CryptoOrderEntry.h"
#include "screens/crypto_trading/CryptoPaperFill.h"
#include "screens/crypto_trading/CryptoSymbolUniverse.h"
#include "screens/crypto_trading/CryptoTickerBar.h"
#include "screens/crypto_trading/CryptoWatchlist.h"
#include "screens/equity_trading/AccountManagementDialog.h"
#include "trading/ExchangeService.h"
#include "trading/ExchangeSession.h"
#include "trading/ExchangeSessionManager.h"
#include "trading/OrderMatcher.h"
#include "trading/PaperTrading.h"
#include "ui/theme/StyleSheets.h"
#include "ui/theme/Theme.h"

#include <QCompleter>

#include <limits>
#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QSplitter>
#include <QStringListModel>
#include <QStyle>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <cmath>
#include <optional>

namespace openmarketterminal::screens {

using namespace openmarketterminal::trading;
using namespace openmarketterminal::screens::crypto;

static const QString TAG = "CryptoTrading";

namespace {
QString exchange_display_name(const QString& exchange_id) {
    if (exchange_id.compare(QStringLiteral("coinbase"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("COINBASE ADVANCED");
    return exchange_id.toUpper();
}

QString coinbase_order_symbol(QString symbol) {
    symbol = symbol.trimmed().toUpper();
    // The exchange-native universe (CryptoSymbolUniverse) makes display==wire
    // on Coinbase, so this is normally identity. Defensively migrate only a
    // stale persisted /USDT suffix; an explicit /USDC pair routes to its own
    // real Coinbase book instead of being silently forced onto /USD.
    return openmarketterminal::crypto::migrate_symbol(QStringLiteral("coinbase"), symbol);
}

bool is_cash_quote(const QString& currency) {
    const QString c = currency.trimmed().toUpper();
    return c == QLatin1String("USD") || c == QLatin1String("USDT") || c == QLatin1String("USDC") ||
           c == QLatin1String("EUR") || c == QLatin1String("GBP") || c == QLatin1String("CAD") ||
           c == QLatin1String("AUD") || c == QLatin1String("CHF") || c == QLatin1String("JPY");
}

QString remap_pair_quote(QString symbol, QString quote) {
    symbol = symbol.trimmed().toUpper();
    quote = quote.trimmed().toUpper();
    if (!symbol.contains(QLatin1Char('/')) || symbol.contains(QLatin1Char(':')) || !is_cash_quote(quote))
        return symbol;
    return symbol.section(QLatin1Char('/'), 0, 0) + QStringLiteral("/") + quote;
}

QString kraken_order_symbol(QString symbol, const QString& funding_currency) {
    symbol = symbol.trimmed().toUpper();
    if (!symbol.contains(QLatin1Char('/')) || symbol.contains(QLatin1Char(':')))
        return symbol;

    const QString display_quote = symbol.section(QLatin1Char('/'), 1, 1);
    if (!is_cash_quote(display_quote))
        return symbol;

    // Kraken distinguishes USD and USDT spot books. The UI may display a USDT
    // pair while the account cash balance is USD; route live orders to the book
    // that matches the cash currency shown on the ticket.
    return is_cash_quote(funding_currency) ? remap_pair_quote(symbol, funding_currency) : symbol;
}

QString order_symbol_for_exchange(const QString& exchange_id, const QString& display_symbol,
                                  const QString& funding_currency) {
    if (exchange_id.compare(QStringLiteral("coinbase"), Qt::CaseInsensitive) == 0)
        return coinbase_order_symbol(display_symbol);
    if (exchange_id.compare(QStringLiteral("kraken"), Qt::CaseInsensitive) == 0)
        return kraken_order_symbol(display_symbol, funding_currency);
    return display_symbol.trimmed().toUpper();
}

QString order_response_message(const QJsonObject& response) {
    const QString error = response.value(QStringLiteral("error")).toString().trimmed();
    if (!error.isEmpty())
        return error;

    const QString message = response.value(QStringLiteral("message")).toString().trimmed();
    if (!message.isEmpty())
        return message;

    const QString id = response.value(QStringLiteral("id")).toString().trimmed();
    if (!id.isEmpty())
        return QStringLiteral("Submitted order %1").arg(id);

    if (!response.isEmpty())
        return QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact));

    return {};
}
} // namespace


void CryptoTradingScreen::on_exchange_changed(const QString& exchange) {
    if (exchange == exchange_id_)
        return;
    LOG_INFO(TAG, QString("Exchange changed: %1 → %2").arg(exchange_id_, exchange));
    exchange_id_ = exchange;
    exchange_btn_->setText(exchange_display_name(exchange));
    if (order_entry_)
        order_entry_->set_exchange_id(exchange_id_);
    if (bottom_panel_)
        bottom_panel_->set_exchange_context(exchange_id_, trading_mode_ != TradingMode::Live);

    if (ws_transport_) {
        ws_transport_->setText(tr("DAEMON"));
        ws_transport_->setToolTip(tr("ws_stream.py via ccxt.pro — Python subprocess"));
        last_daemon_chrome_ = -1;  // force re-apply on the new exchange's next flush
    }

    auto& es = ExchangeService::instance();

    // Phase 3: sessions stay warm for the app lifetime. Do NOT stop the old
    // exchange's WS — `ExchangeSessionManager` keeps it running so switching
    // back is instant. Dropping our hub subscriptions is enough to stop the
    // old exchange's ticks landing in our pending_* buffers; the session
    // keeps updating its cache and publishing to the hub in the background.
    hub_unsubscribe_topics();

    // Clear accumulated buffers — stale data from the old exchange is useless.
    live_orders_by_id_.clear();
    live_avg_entry_.reset();
    last_account_ws_event_ms_ = 0;  // cadence snaps to baseline on the new venue
    pending_tickers_.clear();
    pending_orderbook_ = {};
    has_pending_orderbook_ = false;
    last_book_ = {};
    last_book_ms_ = 0;
    pending_primary_ticker_ = {};
    has_pending_primary_ = false;
    pending_candles_.clear();
    pending_trades_.clear();
    impulse_points_.clear();
    update_impulse_label();
    last_ws_state_ = -1;  // re-evaluate feed mode after new stream connects

    // Reset fetch guards — a prior in-flight fetch from the old exchange must
    // not suppress the first fetch against the new exchange.
    candles_fetching_.store(false);
    live_inflight_.store(0);

    // Exchange-native symbol universe: re-derive the watchlist and migrate the
    // current selection BEFORE init_exchange re-subscribes, so the display
    // pair IS the wire pair on the new venue.
    watchlist_symbols_ = openmarketterminal::crypto::default_watchlist_for(exchange_id_, bitcoin_focus_);
    if (watchlist_)
        watchlist_->set_symbols(watchlist_symbols_);
    const QString migrated = openmarketterminal::crypto::migrate_symbol(exchange_id_, selected_symbol_);
    if (migrated != selected_symbol_) {
        selected_symbol_ = migrated;
        symbol_input_->setText(migrated);
        ticker_bar_->set_symbol(migrated);
        order_entry_->set_symbol(migrated);
        watchlist_->set_active_symbol(migrated);
    }

    es.set_exchange(exchange_id_);

    // Re-initialize — re-registers the four WS callbacks on the NEW session.
    // If that session's WS is already warm (we visited it earlier), init_exchange
    // skips the start_ws_stream() call, so there's no second handshake.
    initialized_ = false;
    init_exchange();

    load_portfolio();

    // Chart history (left of "now") still needs one REST fetch — Kraken WS
    // only streams the current bar. Everything else (ticker, orderbook,
    // watchlist) repopulates from the new session's WS subscriptions within
    // ~1s of the handshake. Funding/OI is perp-only and stays on its 30s
    // market_info_timer_ — those endpoints aren't on the public WS feed.
    async_fetch_candles(selected_symbol_, chart_->current_timeframe());
    refresh_market_info();
    update_futures_visibility(); // leverage/margin/reduce-only depend on perp-ness
    if (trading_mode_ == TradingMode::Live)
        async_fetch_trading_fees(); // fees are per-exchange

    ScreenStateManager::instance().notify_changed(this);
}

void CryptoTradingScreen::on_symbol_selected(const QString& symbol) {
    const QString normalized = normalized_symbol_for_focus(symbol);
    if (normalized.isEmpty() || normalized == selected_symbol_)
        return;
    switch_symbol(normalized);
    ScreenStateManager::instance().notify_changed(this);

    // Phase 7: publish to the linked group so other panels in the same
    // group switch to this pair. Source = `this` so on_group_symbol_changed
    // can suppress its own re-publish below.
    if (link_group_ != SymbolGroup::None) {
        SymbolRef ref;
        ref.symbol = normalized;
        ref.asset_class = QStringLiteral("crypto");
        ref.exchange = exchange_id_;
        SymbolContext::instance().set_group_symbol(link_group_, ref, this);
    }
}

void CryptoTradingScreen::on_group_symbol_changed(const SymbolRef& ref) {
    // Phase 7: subscribe-side. Only react to crypto symbols — an inbound
    // equity ticker has nothing meaningful to do here. Empty asset_class
    // is treated as "unknown" and ignored to avoid surprising the user
    // with a stale cross-asset propagation.
    if (!ref.is_valid())
        return;
    if (ref.asset_class != QStringLiteral("crypto"))
        return;
    const QString normalized = normalized_symbol_for_focus(ref.symbol);
    if (normalized == selected_symbol_)
        return; // already showing this pair
    // Reuse the existing publish-suppressing path: switch_symbol mutates
    // selected_symbol_ but doesn't re-emit because we don't go through
    // on_symbol_selected.
    switch_symbol(normalized);
}

SymbolRef CryptoTradingScreen::current_symbol() const {
    if (selected_symbol_.isEmpty())
        return {};
    SymbolRef r;
    r.symbol = selected_symbol_;
    r.asset_class = QStringLiteral("crypto");
    r.exchange = exchange_id_;
    return r;
}

void CryptoTradingScreen::switch_symbol(const QString& symbol) {
    const QString normalized = normalized_symbol_for_focus(symbol);
    if (normalized == selected_symbol_)
        return;
    LOG_INFO(TAG, QString("Symbol changed: %1 → %2").arg(selected_symbol_, normalized));
    auto& es = ExchangeService::instance();
    es.unwatch_symbol(selected_symbol_, portfolio_id_);
    selected_symbol_ = normalized;
    symbol_input_->setText(normalized);
    ticker_bar_->set_symbol(normalized);
    order_entry_->set_symbol(normalized);
    order_entry_->set_orderbook_quote(0, 0);
    watchlist_->set_active_symbol(normalized);

    // Drop per-symbol buffers tied to the old symbol to prevent cross-symbol leakage.
    has_pending_primary_ = false;
    pending_primary_ticker_ = {};
    has_pending_orderbook_ = false;
    pending_orderbook_ = {};
    last_book_ = {};
    last_book_ms_ = 0;  // the old symbol's book must never fill the new symbol's paper order
    pending_candles_.clear();
    pending_trades_.clear();
    impulse_points_.clear();
    // Per-symbol overlay state: the next my-trades REST fetch re-seeds it.
    live_avg_entry_.reset();
    if (trading_mode_ == TradingMode::Live && ladder_) {
        ladder_->set_my_orders({});
        ladder_->set_avg_entry(0);
    }
    update_impulse_label();
    market_info_cache_ = {};

    es.watch_symbol(selected_symbol_, portfolio_id_);
    es.set_ws_primary_symbol(normalized);

    // Three of the five subscriptions (ticker/orderbook/trades + ohlc
    // pattern) are primary-symbol-specific and need to be re-bound to the
    // new symbol. The watchlist ticker pattern doesn't change. Simpler to
    // resubscribe wholesale than diff.
    hub_subscribe_topics();

    // WS-only mode: ticker + orderbook reflow naturally as the new symbol's
    // WS subscriptions kick in. Only history needs a REST fetch.
    async_fetch_candles(selected_symbol_, chart_->current_timeframe());
    update_futures_visibility(); // a perp↔spot symbol switch toggles the controls
}

void CryptoTradingScreen::on_mode_toggled() {
    const bool is_live = mode_btn_->isChecked();
    trading_mode_ = is_live ? TradingMode::Live : TradingMode::Paper;
    mode_btn_->setText(is_live ? tr("LIVE") : tr("PAPER"));
    mode_btn_->setProperty("mode", is_live ? "live" : "paper");
    mode_btn_->style()->unpolish(mode_btn_);
    mode_btn_->style()->polish(mode_btn_);
    order_entry_->set_mode(!is_live);
    bottom_panel_->set_mode(!is_live);
    bottom_panel_->set_exchange_context(exchange_id_, !is_live);

    if (is_live) {
        live_data_timer_->start(5000);
        refresh_live_data();
        async_fetch_trading_fees(); // populate the Fees tab once on entering live
        // Clear the Paper overlay on mode entry; the Live overlay repopulates
        // from the account stream + REST seed (refresh_live_ladder_overlay /
        // CryptoLiveOverlay.h) within the first refresh cycle.
        ladder_->set_my_orders({});
        ladder_->set_avg_entry(0);
    } else {
        live_data_timer_->stop();
        live_orders_by_id_.clear();  // account-WS fast path is live-mode only
        // Leaving live mode → the auth indicator is no longer meaningful; reset
        // the API button to neutral. The DAEMON label is liveness-driven
        // (update_daemon_chrome) and is deliberately NOT touched here.
        last_auth_state_ = -1;
        if (api_btn_) {
            api_btn_->setProperty("authed", QVariant());
            api_btn_->style()->unpolish(api_btn_);
            api_btn_->style()->polish(api_btn_);
        }
        refresh_portfolio();
    }
}

void CryptoTradingScreen::on_api_clicked() {
    auto* dlg = new CryptoCredentials(exchange_id_, this);

    // If credentials are already stored for this exchange, open the dialog in
    // "connected" mode (pre-filled non-secret fields + status) rather than a
    // blank form — otherwise a connected user sees an empty entry screen and
    // can't tell they're already set up.
    const ExchangeCredentials existing = ExchangeService::instance().get_credentials();
    const bool have_existing = !existing.api_key.isEmpty() || !existing.wallet_address.isEmpty();
    if (have_existing)
        dlg->mark_connected(existing.api_key, existing.password, existing.wallet_address);

    connect(dlg, &CryptoCredentials::credentials_saved, this,
            [this, existing](const QString& key, const QString& secret, const QString& pw, const QString& wallet,
                             const QString& pk) {
                ExchangeCredentials creds;
                creds.api_key = key;
                // Secret/private-key left blank in connected mode → keep the
                // stored value (it is never shown in the dialog).
                creds.secret = secret.isEmpty() ? existing.secret : secret;
                creds.password = pw;
                creds.wallet_address = wallet;
                creds.private_key = pk.isEmpty() ? existing.private_key : pk;
                ExchangeService::instance().set_credentials(creds);
                LOG_INFO(TAG, "Credentials saved for " + exchange_id_);
            });
    dlg->exec();
    dlg->deleteLater();
}

void CryptoTradingScreen::on_accounts_clicked() {
    auto* dlg = new equity::AccountManagementDialog(this);
    dlg->setWindowTitle(tr("Manage Trading Accounts"));

    connect(dlg, &equity::AccountManagementDialog::credentials_saved, this, [](const QString& account_id) {
        Q_UNUSED(account_id);
        LOG_INFO(TAG, "Broker account credentials saved from crypto workspace");
    });

    dlg->exec();
    dlg->deleteLater();
}

void CryptoTradingScreen::on_order_submitted(const QString& side, const QString& order_type, double qty, double price,
                                             double stop_price, double sl, double tp, bool post_only) {
    LOG_INFO(TAG, QString("Order submit: mode=%1 %2 %3 qty=%4 px=%5 stop=%6 sl=%7 tp=%8 post_only=%9 sym=%10")
                      .arg(trading_mode_ == TradingMode::Paper ? "PAPER" : "LIVE", side, order_type)
                      .arg(qty).arg(price).arg(stop_price).arg(sl).arg(tp).arg(post_only ? "yes" : "no").arg(selected_symbol_));
    try {
        if (trading_mode_ == TradingMode::Paper) {
            if (order_type == "market") {
                // Honest market fill: walk the freshest live book; a stale or
                // empty book REJECTS instead of filling against dead data.
                // (Replaces the old ticker.last / literal-1000.0 fill.)
                const qint64 age_ms = last_book_ms_ > 0
                    ? QDateTime::currentMSecsSinceEpoch() - last_book_ms_
                    : std::numeric_limits<qint64>::max();
                const auto verdict = openmarketterminal::crypto::paper_market_fill(
                    side, qty, last_book_, age_ms, portfolio_.fee_rate);
                if (!verdict.ok) {
                    order_entry_->show_order_result(false, tr("PAPER reject: %1").arg(verdict.reason));
                    return;
                }
                auto order = pt_place_order(portfolio_id_, selected_symbol_, side, order_type,
                                            verdict.filled_qty, verdict.fill_price, std::nullopt);
                pt_fill_order(order.id, verdict.fill_price);
                order_entry_->show_order_result(
                    true, tr("PAPER filled %1 @ %2 (fee ≈ %3)%4")
                              .arg(verdict.filled_qty)
                              .arg(verdict.fill_price)
                              .arg(verdict.fee_paid)
                              .arg(verdict.filled_qty < qty ? tr(" — PARTIAL: visible depth") : QString()));
            } else {
                std::optional<double> price_opt;
                if (price > 0)
                    price_opt = price;
                std::optional<double> stop_opt;
                if (stop_price > 0)
                    stop_opt = stop_price;
                auto order =
                    pt_place_order(portfolio_id_, selected_symbol_, side, order_type, qty, price_opt, stop_opt);
                OrderMatcher::instance().add_order(order);
                if (sl > 0 || tp > 0)
                    OrderMatcher::instance().set_sl_tp(portfolio_id_, selected_symbol_, order.id, sl, tp);
            }
            refresh_portfolio();
        } else {
            // In-flight guard: place_exchange_order is dispatched to a worker and
            // we return to the GUI thread immediately, so the submit button must
            // be disabled for the lifetime of the POST — otherwise a double-click
            // or held Ctrl+Enter fires a second real exchange order (CR-07).
            order_entry_->set_submit_busy(true);

            // Read the reduce-only flag on the UI thread before dispatching.
            const bool reduce_only = order_entry_->reduce_only();
            const QString funding_currency = order_entry_ ? order_entry_->funding_currency() : QString();
            const QString execution_symbol = order_symbol_for_exchange(exchange_id_, selected_symbol_, funding_currency);
            LOG_INFO(TAG, QString("Live execution symbol: display=%1 exchange=%2 execution=%3")
                              .arg(selected_symbol_, exchange_id_, execution_symbol));
            QPointer<CryptoTradingScreen> self = this;
            QPointer<crypto::CryptoOrderEntry> oe = order_entry_;
            (void)QtConcurrent::run([self, oe, execution_symbol, side, order_type, qty, price, stop_price, sl, tp,
                                     reduce_only, post_only]() {
                // stop_price drives Stop / Stop-Limit triggers; sl/tp attach native
                // bracket legs; reduce_only is honoured on perps. The daemon maps
                // these to ccxt unified params (triggerPrice / stopLoss / takeProfit).
                // Re-enable the button on completion regardless of success/throw so
                // the user is never stuck on a permanently disabled SENDING… button.
                QJsonObject response;
                bool ok = false;
                QString message;
                try {
                    if (self) {
                        response = ExchangeService::instance().place_exchange_order(execution_symbol, side, order_type,
                                                                                   qty, price, stop_price, sl, tp,
                                                                                   reduce_only, post_only);
                        ok = !response.contains(QStringLiteral("error")) &&
                             response.value(QStringLiteral("success")).toBool(true);
                        message = order_response_message(response);
                        if (message.isEmpty())
                            message = ok ? QStringLiteral("Submitted %1 order").arg(execution_symbol)
                                         : QStringLiteral("Order rejected");
                    }
                } catch (const std::exception& e) {
                    message = QString::fromUtf8(e.what());
                    LOG_ERROR(TAG, QString("Live order failed: %1").arg(message));
                } catch (...) {
                    message = QStringLiteral("Unknown live order failure");
                    LOG_ERROR(TAG, "Live order failed: unknown exception");
                }
                // order_entry_ is a child of the screen, so if self is alive the
                // order-entry widget is too. Posting onto self is sufficient.
                if (!self)
                    return;
                QMetaObject::invokeMethod(
                    self,
                    [self, oe, ok, message, execution_symbol]() {
                        if (oe) {
                            oe->set_submit_busy(false);
                            oe->show_order_result(ok, message);
                        }
                        if (ok) {
                            QMessageBox::information(self, CryptoTradingScreen::tr("Coinbase Order Submitted"),
                                                     CryptoTradingScreen::tr("%1\n\nSymbol: %2")
                                                         .arg(message, execution_symbol));
                        } else {
                            QMessageBox::warning(self, CryptoTradingScreen::tr("Coinbase Order Rejected"),
                                                 CryptoTradingScreen::tr("%1\n\nSymbol sent: %2")
                                                     .arg(message, execution_symbol));
                        }
                        if (self)
                            self->refresh_live_data();
                    },
                    Qt::QueuedConnection);
            });
        }
    } catch (const std::exception& e) {
        LOG_ERROR(TAG, QString("Order failed: %1").arg(e.what()));
    }
}

void CryptoTradingScreen::on_cancel_order(const QString& order_id) {
    LOG_INFO(TAG, QString("Cancel order: %1 (%2)").arg(order_id, trading_mode_ == TradingMode::Paper ? "paper" : "live"));
    if (trading_mode_ == TradingMode::Paper) {
        try {
            pt_cancel_order(order_id);
            OrderMatcher::instance().remove_order(order_id);
            refresh_portfolio();
        } catch (const std::exception& e) {
            LOG_ERROR(TAG, QString("Cancel failed: %1").arg(e.what()));
        }
    } else {
        QPointer<CryptoTradingScreen> self = this;
        (void)QtConcurrent::run([self, order_id]() {
            if (!self)
                return;
            ExchangeService::instance().cancel_exchange_order(order_id, self->selected_symbol_);
            QMetaObject::invokeMethod(
                self,
                [self]() {
                    if (!self)
                        return;
                    self->refresh_live_data();
                },
                Qt::QueuedConnection);
        });
    }
}

void CryptoTradingScreen::on_ob_price_clicked(double price) {
    order_entry_->set_current_price(price);
}

void CryptoTradingScreen::on_search_requested(const QString& filter) {
    QPointer<CryptoTradingScreen> self = this;
    QString filter_copy = filter;
    (void)QtConcurrent::run([self, filter_copy]() {
        if (!self)
            return;
        auto markets = ExchangeService::instance().fetch_markets("spot", filter_copy);
        QMetaObject::invokeMethod(
            self,
            [self, markets]() {
                if (!self)
                    return;
                self->watchlist_->set_search_results(markets);
            },
            Qt::QueuedConnection);
    });
}

bool CryptoTradingScreen::is_perp_market() const {
    // Hyperliquid is a perps-only DEX; on other venues a settled pair
    // (e.g. "BTC/USDC:USDC") denotes a swap/perp market.
    return exchange_id_ == QLatin1String("hyperliquid") || selected_symbol_.contains(QLatin1Char(':'));
}

void CryptoTradingScreen::update_futures_visibility() {
    if (order_entry_)
        order_entry_->set_futures_mode(is_perp_market());
}

void CryptoTradingScreen::on_cancel_all_orders() {
    if (trading_mode_ == TradingMode::Paper) {
        if (portfolio_id_.isEmpty())
            return;
        try {
            for (const auto& o : pt_get_orders(portfolio_id_, "pending")) {
                pt_cancel_order(o.id);
                OrderMatcher::instance().remove_order(o.id);
            }
        } catch (const std::exception& e) {
            LOG_ERROR(TAG, QString("Paper cancel-all failed: %1").arg(e.what()));
        }
        refresh_portfolio();
        return;
    }
    // Live — fetch open orders then cancel each on a worker thread.
    QPointer<CryptoTradingScreen> self = this;
    (void)QtConcurrent::run([self]() {
        if (!self)
            return;
        auto result = ExchangeService::instance().fetch_open_orders_live();
        const auto orders = result.value("orders").toArray();
        int cancelled = 0;
        for (const auto& v : orders) {
            const auto o = v.toObject();
            const QString id = o.value("id").toString();
            if (id.isEmpty())
                continue;
            ExchangeService::instance().cancel_exchange_order(id, o.value("symbol").toString());
            ++cancelled;
        }
        QMetaObject::invokeMethod(
            self,
            [self, cancelled]() {
                if (!self)
                    return;
                LOG_INFO(TAG, QString("Cancelled %1 live order(s)").arg(cancelled));
                self->refresh_live_data();
            },
            Qt::QueuedConnection);
    });
}

void CryptoTradingScreen::on_close_all_positions() {
    if (trading_mode_ == TradingMode::Paper) {
        if (portfolio_id_.isEmpty())
            return;
        try {
            for (const auto& p : pt_get_positions(portfolio_id_)) {
                if (p.quantity == 0)
                    continue;
                auto ticker = ExchangeService::instance().get_cached_price(p.symbol);
                const double fill = ticker.last > 0 ? ticker.last : p.current_price;
                const QString side = p.quantity > 0 ? "sell" : "buy";
                auto order = pt_place_order(portfolio_id_, p.symbol, side, "market", std::abs(p.quantity),
                                            std::optional<double>(fill), std::nullopt);
                pt_fill_order(order.id, fill);
            }
        } catch (const std::exception& e) {
            LOG_ERROR(TAG, QString("Paper close-all failed: %1").arg(e.what()));
        }
        refresh_portfolio();
        return;
    }
    // Live — counter each open position with a reduce-only market order.
    QPointer<CryptoTradingScreen> self = this;
    (void)QtConcurrent::run([self]() {
        if (!self)
            return;
        auto result = ExchangeService::instance().fetch_positions_live();
        const auto positions = result.value("positions").toArray();
        int closed = 0;
        for (const auto& v : positions) {
            const auto p = v.toObject();
            const QString sym = p.value("symbol").toString();
            const double contracts = p.value("contracts").toDouble();
            if (sym.isEmpty() || contracts == 0)
                continue;
            const bool is_long = p.value("side").toString() == QLatin1String("long") || contracts > 0;
            ExchangeService::instance().place_exchange_order(sym, is_long ? "sell" : "buy", "market",
                                                             std::abs(contracts), 0.0, 0.0, 0.0, 0.0, /*reduce_only=*/true);
            ++closed;
        }
        QMetaObject::invokeMethod(
            self,
            [self, closed]() {
                if (!self)
                    return;
                LOG_INFO(TAG, QString("Closed %1 live position(s)").arg(closed));
                self->refresh_live_data();
            },
            Qt::QueuedConnection);
    });
}

void CryptoTradingScreen::on_close_position(const QString& symbol) {
    if (symbol.isEmpty())
        return;
    if (trading_mode_ == TradingMode::Paper) {
        if (portfolio_id_.isEmpty())
            return;
        try {
            for (const auto& p : pt_get_positions(portfolio_id_)) {
                if (p.symbol != symbol || p.quantity == 0)
                    continue;
                auto ticker = ExchangeService::instance().get_cached_price(symbol);
                const double fill = ticker.last > 0 ? ticker.last : p.current_price;
                const QString side = p.quantity > 0 ? "sell" : "buy";
                auto order = pt_place_order(portfolio_id_, symbol, side, "market", std::abs(p.quantity),
                                            std::optional<double>(fill), std::nullopt);
                pt_fill_order(order.id, fill);
            }
        } catch (const std::exception& e) {
            LOG_ERROR(TAG, QString("Paper close failed: %1").arg(e.what()));
        }
        refresh_portfolio();
        return;
    }
    // Live — reduce-only counter order for the one symbol.
    QPointer<CryptoTradingScreen> self = this;
    const QString sym = symbol;
    (void)QtConcurrent::run([self, sym]() {
        if (!self)
            return;
        auto result = ExchangeService::instance().fetch_positions_live(sym);
        const auto positions = result.value("positions").toArray();
        for (const auto& v : positions) {
            const auto p = v.toObject();
            if (p.value("symbol").toString() != sym)
                continue;
            const double contracts = p.value("contracts").toDouble();
            if (contracts == 0)
                continue;
            const bool is_long = p.value("side").toString() == QLatin1String("long") || contracts > 0;
            ExchangeService::instance().place_exchange_order(sym, is_long ? "sell" : "buy", "market",
                                                             std::abs(contracts), 0.0, 0.0, 0.0, 0.0, /*reduce_only=*/true);
        }
        QMetaObject::invokeMethod(
            self,
            [self]() {
                if (!self)
                    return;
                self->refresh_live_data();
            },
            Qt::QueuedConnection);
    });
}

// ============================================================================
// Refresh Functions
// ============================================================================

// ============================================================================
// WS Update Coalescing — flush accumulated data to UI at 10fps
// ============================================================================

} // namespace openmarketterminal::screens
