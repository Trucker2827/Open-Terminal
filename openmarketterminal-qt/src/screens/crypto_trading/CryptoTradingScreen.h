#pragma once
// Crypto Trading Screen — coordinator

#include "core/symbol/IGroupLinked.h"
#include "core/symbol/SymbolGroup.h"
#include "screens/common/IStatefulScreen.h"
#include "screens/crypto_trading/CryptoLiveOverlay.h"
#include "screens/crypto_trading/CryptoTypes.h"
#include "trading/TradingTypes.h"

#include <QEvent>
#include <QHideEvent>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QShowEvent>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <deque>

namespace openmarketterminal::screens::crypto {
class CryptoTickerBar;
class CryptoWatchlist;
class CryptoChart;
class CryptoOrderEntry;
class CryptoOrderBook;
class CryptoBottomPanel;
class CryptoLadder;
} // namespace openmarketterminal::screens::crypto

namespace openmarketterminal::screens {

class CryptoTradingScreen : public QWidget, public IStatefulScreen, public IGroupLinked {
    Q_OBJECT
    Q_INTERFACES(openmarketterminal::IGroupLinked)
  public:
    enum class Focus { MultiAsset, Bitcoin };

    explicit CryptoTradingScreen(QWidget* parent = nullptr);
    explicit CryptoTradingScreen(Focus focus, QWidget* parent = nullptr);
    ~CryptoTradingScreen();

    void restore_state(const QVariantMap& state) override;
    QVariantMap save_state() const override;
    QString state_key() const override { return bitcoin_focus_ ? "bitcoin" : "crypto_trading"; }
    int state_version() const override { return 1; }

    // IGroupLinked — Phase 7: link crypto pairs across groups so a "BTC/USDT"
    // selection in one panel propagates to a chart panel in the same group.
    void set_group(SymbolGroup g) override { link_group_ = g; }
    SymbolGroup group() const override { return link_group_; }
    void on_group_symbol_changed(const SymbolRef& ref) override;
    SymbolRef current_symbol() const override;

  signals:
    /// User clicked a crypto prediction market — host should open the
    /// Predictions ("polymarket") screen so they can place the Kalshi bet.
    void open_predictions_requested();

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;

  private slots:
    void on_exchange_changed(const QString& exchange);
    void on_symbol_selected(const QString& symbol);
    void on_mode_toggled();
    void on_api_clicked();
    void on_accounts_clicked();
    void on_order_submitted(const QString& side, const QString& order_type, double qty, double price, double stop_price,
                            double sl, double tp, bool post_only);
    void on_cancel_order(const QString& order_id);
    void on_cancel_all_orders();                     // CANCEL ALL (live + paper)
    void on_close_all_positions();                   // SQUARE OFF ALL (live + paper)
    void on_close_position(const QString& symbol);   // close a single position
    void on_ob_price_clicked(double price);
    void on_search_requested(const QString& filter);

    void refresh_ticker();
    void refresh_orderbook();
    void refresh_portfolio();
    void refresh_watchlist();
    void refresh_market_info();
    void refresh_candles();
    void refresh_live_data();
    void update_clock();

  private:
    void setup_ui();
    void setup_timers();
    void retranslateUi();
    void init_exchange();
    void load_portfolio();
    void switch_symbol(const QString& symbol);
    QString default_symbol() const;
    QString normalized_symbol_for_focus(const QString& symbol) const;

    // Perp detection + futures-control visibility. Leverage / margin-mode /
    // reduce-only controls only make sense on derivatives, so they are shown
    // only when the active market is a perp (Hyperliquid, or a settled pair).
    bool is_perp_market() const;
    void update_futures_visibility();

    void async_fetch_candles(const QString& symbol, const QString& timeframe);
    void async_fetch_live_positions();
    void async_fetch_live_orders();
    void async_fetch_live_balance();
    void async_fetch_my_trades();
    void async_fetch_trading_fees();
    void async_fetch_mark_price();
    void async_set_leverage(int leverage);
    void async_set_margin_mode(const QString& mode);

    // ── DataHub subscription lifecycle (the only data path since Phase 6) ─
    // Only exchanges registered as DataHub producers via
    // ExchangeSessionManager::topic_patterns() can appear in the dropdown.
    void hub_subscribe_topics();
    void hub_unsubscribe_topics();

    // ── Command bar widgets ──
    QPushButton* exchange_btn_ = nullptr;
    QMenu* exchange_menu_ = nullptr;
    QLineEdit* symbol_input_ = nullptr;
    QPushButton* mode_btn_ = nullptr;
    QPushButton* api_btn_ = nullptr;
    QPushButton* accounts_btn_ = nullptr;
    QLabel* ws_status_ = nullptr;
    QLabel* ws_transport_ = nullptr;  // tiny hint: "NATIVE" for Kraken, "DAEMON" for ccxt
    QLabel* impulse_label_ = nullptr;
    QLabel* clock_label_ = nullptr;

    /// Context object that owns all direct connections to the native Kraken
    /// WS client. Destroyed (and recreated) on symbol/exchange swap so every
    /// connection is auto-disconnected in one move.
    QObject* ws_subscription_owner_ = nullptr;

    // ── Sub-widgets ──
    crypto::CryptoTickerBar* ticker_bar_ = nullptr;
    crypto::CryptoWatchlist* watchlist_ = nullptr;
    crypto::CryptoChart* chart_ = nullptr;
    crypto::CryptoOrderEntry* order_entry_ = nullptr;
    crypto::CryptoOrderBook* orderbook_ = nullptr;
    crypto::CryptoBottomPanel* bottom_panel_ = nullptr;
    crypto::CryptoLadder* ladder_ = nullptr;  // read-only DOM ladder (depth + VAP), fed alongside orderbook_

    // ── Timers ──
    QTimer* ticker_timer_ = nullptr;
    QTimer* ob_timer_ = nullptr;
    QTimer* portfolio_timer_ = nullptr;
    QTimer* watchlist_timer_ = nullptr;
    QTimer* market_info_timer_ = nullptr;
    QTimer* live_data_timer_ = nullptr;
    QTimer* clock_timer_ = nullptr;

    // ── State ──
    bool bitcoin_focus_ = false;
    QString exchange_id_ = "coinbase";
    // Set in the ctor from CryptoSymbolUniverse (exchange-native quote).
    QString selected_symbol_;
    crypto::TradingMode trading_mode_ = crypto::TradingMode::Paper;

    // Paper trading
    QString portfolio_id_;
    trading::PtPortfolio portfolio_;


    // Async fetch guards
    std::atomic<bool> candles_fetching_{false};
    std::atomic<int> live_inflight_{0};  // counts async_fetch_live_* tasks still running
    std::atomic<bool> paper_bookkeeping_in_flight_{false};

    // Startup gate — ensures daemon-dependent fetches fire exactly once,
    // either via daemon_ready signal or via the 8s safety fallback timeout.
    bool startup_fetches_done_ = false;

    // WS/REST mode edge detection — when WS transitions online we stop REST
    // polling; when it drops we restart it. tri-state: -1 = unknown.
    int last_ws_state_ = -1;
    int last_ws_status_label_state_ = -1;  // separate from last_ws_state_ since update_clock is 1Hz
    void apply_feed_mode(bool ws_connected);
    // Reflect whether the last authenticated daemon call (live balance) succeeded
    // on the API/DAEMON status chrome. Truthful "account reachable" signal —
    // distinct from the public WS feed pill (ws_status_). -1 = unknown/neutral.
    void set_live_auth_indicator(bool ok);
    int last_auth_state_ = -1;
    // DAEMON label = ccxt subprocess liveness (dead/rest/live), edge-detected.
    // Independent of the auth indicator above — see CryptoChromeState.h.
    void update_daemon_chrome();
    int last_daemon_chrome_ = -1;

    // Set in the ctor (and on exchange change) from CryptoSymbolUniverse —
    // always quoted in the active exchange's native currency.
    QStringList watchlist_symbols_;

    bool initialized_ = false;

    // Phase 7: symbol group link — SymbolGroup::None when unlinked.
    // Crypto Trading publishes selected_symbol_ (a "BASE/QUOTE" pair) into
    // the linked group with asset_class="crypto"; consumes inbound symbols
    // tagged crypto by routing them through switch_symbol().
    SymbolGroup link_group_ = SymbolGroup::None;

    // Cached market info — funding rate and open interest arrive on separate
    // workers so we merge into this cache and emit the union to the bottom
    // panel from each worker's return path (UI-thread — no lock needed).
    crypto::MarketInfoData market_info_cache_;

    // ── WS update coalescing (10fps UI flush) ──
    QTimer* ws_flush_timer_ = nullptr;
    QHash<QString, trading::TickerData> pending_tickers_; // accumulated since last flush
    trading::TickerData pending_primary_ticker_;          // latest for selected symbol
    bool has_pending_primary_ = false;

    // Orderbook coalescing — latest-wins. Raw bursts (~20 msgs/sec on busy
    // symbols) collapse to one UI refresh per flush tick (10/sec).
    trading::OrderBookData pending_orderbook_;
    bool has_pending_orderbook_ = false;

    // Latest book + receipt time — freshness evidence for honest paper-mode
    // market fills (CryptoPaperFill.h): stale/missing book → REJECT, no fill.
    trading::OrderBookData last_book_;
    qint64 last_book_ms_ = 0;

    struct ImpulsePoint {
        qint64 ts_ms = 0;
        double price = 0.0;
        double bid = 0.0;
        double ask = 0.0;
    };
    std::deque<ImpulsePoint> impulse_points_;
    void record_impulse_tick(double price, double bid, double ask);
    void update_impulse_label();

    // Candle coalescing — append closed candles in order, keep the latest
    // in-progress candle separately so we render partial bars without
    // flooding the chart on every tick.
    QVector<trading::Candle> pending_candles_;

    // Trade coalescing — batch recent trades so bottom_panel_ does one list
    // append per flush tick instead of one per WS message.
    QVector<crypto::TradeEntry> pending_trades_;

    void flush_ws_updates();

    // ── Authenticated account WS (fast path; REST stays source of truth) ──
    // Any account event stamps last_account_ws_event_ms_; the REST poll
    // cadence relaxes only while this is fresh (CryptoAccountCadence.h).
    qint64 last_account_ws_event_ms_ = 0;
    QHash<QString, QJsonObject> live_orders_by_id_;  // open orders, keyed by exchange order id
    bool account_refresh_scheduled_ = false;         // coalesces confirming REST fetches
    void on_account_order_event(const QJsonObject& order);
    void on_account_balance_event(const QJsonObject& balances);
    // Live-mode DOM overlay: own resting orders + est. avg entry (VWAP of
    // my-trades for the active pair; see CryptoLiveOverlay.h caveats).
    openmarketterminal::crypto::LiveAvgEntry live_avg_entry_;
    void refresh_live_ladder_overlay();
    // Shared AVAIL-currency fallback chain (pair quote → USD → USDC → USDT →
    // USDE → largest holding) used by both the REST and WS balance paths.
    void apply_live_balance_display(const QJsonObject& balances);

    // Ladder ORDERS/avg-entry overlay — Paper-mode only (typed PtOrder/
    // PtPosition are cleanly available here; Live-mode orders/positions
    // arrive as raw per-exchange JSON already parsed by CryptoBottomPanel,
    // so that path is left unwired — see task-5 report).
    void update_ladder_overlay(const QVector<trading::PtOrder>& orders,
                                const QVector<trading::PtPosition>& positions);
};

} // namespace openmarketterminal::screens
