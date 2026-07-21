#pragma once

#include "services/edge_radar/KalshiAutoEngine.h"

#include "services/prediction/PredictionTypes.h"
#include "trading/TradingTypes.h"

#include <QList>
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QWidget>

#include <atomic>
#include <functional>

class QComboBox;
class QDoubleSpinBox;
class QJsonArray;
class QLabel;
class QLineEdit;
class QListWidget;
class QObject;
class QPushButton;
class QResizeEvent;
class QSpinBox;
class QStackedWidget;
class QSplitter;
class QTableWidget;
class QTableWidgetItem;
class QTextEdit;
class QTimer;
class QWebSocket;

namespace openmarketterminal::trading { struct OrderBookData; }
namespace openmarketterminal::services::prediction {
class PredictionExchangeAdapter;
}
namespace openmarketterminal::screens::crypto { class CryptoOrderBook; }

namespace openmarketterminal::screens::kalshi {

class KalshiSimpleChart;

class KalshiScreen final : public QWidget {
    Q_OBJECT
  public:
    explicit KalshiScreen(QWidget* parent = nullptr);
    ~KalshiScreen() override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  signals:
    void venue_switch_requested(const QString& venue);

  protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void build_ui();
    void ensure_workspace_panes_visible();
    void wire_adapter();
    void refresh();
    void set_family(const QString& family);
    void set_asset(const QString& asset);
    void set_cadence(const QString& cadence);
    QString category_slug() const;
    void populate_events(const QVector<services::prediction::PredictionEvent>& events);
    void populate_markets(const QVector<services::prediction::PredictionMarket>& markets);
    void select_market(int row);
    void render_market();
    void apply_live_market_prices(const QString& ticker, const QJsonObject& payload);
    void render_order_book(const services::prediction::PredictionOrderBook& book);
    void render_trades(const QVector<services::prediction::PredictionTrade>& trades);
    void preview_order();
    void toggle_shadow_collector();
    void observe_shadow_book(const services::prediction::PredictionOrderBook& book);
    void append_shadow_event(const QString& event, const QString& asset_id,
                             double quote_price, double queue, const QString& confirmation = QString());
    void refresh_spot_dom();
    void start_spot_dom_stream();
    void render_spot_book(const openmarketterminal::trading::OrderBookData& book, const QString& source);
    void handle_reference_dom_message(const QString& message);
    void subscribe_reference_dom();
    void set_reference_dom_venue(const QString& venue);
    void refresh_venue_consensus();
    void refresh_reference_chart();
    void refresh_spot_history();
    void set_chart_timeframe(const QString& timeframe);
    void set_spot_symbol(const QString& asset);
    void update_observation_strip();
    void update_market_health();
    void update_strike_overlay();
    void refresh_flow_meter();
    void record_ladder_evidence();
    void refresh_volatility_estimate(const QString& symbol, qint64 decision_ts_ms);
    void render_ladder_surface(
        const QVector<services::edge_radar::KalshiSurfacePoint>& surface,
        const services::edge_radar::KalshiPortfolioPlan& plan,
        const QJsonArray& diagnostics);
    void record_kalshi_trade(const services::prediction::PredictionTrade& trade);
    bool record_account_fills(const QVariantList& activities);
    void reconcile_settlement(const services::prediction::PredictionMarket& market);
    void refresh_account_status();
    void show_account_dialog();
    void update_position_panel();
    void update_live_positions_summary();
    void render_closed_bets(const QJsonArray& settlements);
    void show_contract_details(QTableWidgetItem* item);
    void cash_out_selected_position();
    void place_live_order();
    void show_live_automation_dialog();
    void kill_live_automation();
    void refresh_live_automation_status();
    void refresh_advisor_canary_status();
    void refresh_daemon_status();
    void restart_daemon();
    void run_live_cli(const QStringList& args, const std::function<void(const QJsonObject&, const QString&)>& done);
    QString cli_path() const;
    QString evidence_path(const QString& filename) const;
    services::prediction::PredictionExchangeAdapter* adapter() const;

    QList<QMetaObject::Connection> connections_;
    QVector<services::prediction::PredictionMarket> markets_;
    QVector<services::prediction::PredictionMarket> all_markets_;
    QHash<QString, services::prediction::PredictionOrderBook> kalshi_books_;
    QHash<QString, QJsonArray> orderbook_event_buffer_;
    QStringList subscribed_ladder_assets_;
    QSet<QString> reconciled_settlements_;
    QSet<QString> backfilled_series_;
    QSet<QString> recorded_account_settlements_;
    bool settlement_index_loaded_ = false;
    services::prediction::PredictionMarket selected_;
    bool has_selection_ = false;
    bool first_show_ = true;
    QString family_ = QStringLiteral("Crypto");
    QString asset_ = QStringLiteral("BTC");
    QString cadence_ = QStringLiteral("hourly");
    QString side_ = QStringLiteral("YES");

    QPushButton* polymarket_button_ = nullptr;
    QComboBox* family_combo_ = nullptr;
    QLineEdit* search_ = nullptr;
    QLabel* connection_badge_ = nullptr;
    QPushButton* account_button_ = nullptr;
    QLabel* account_badge_ = nullptr;
    QLabel* daemon_badge_ = nullptr;
    QPushButton* daemon_restart_button_ = nullptr;
    QLabel* count_label_ = nullptr;
    QWidget* asset_bar_ = nullptr;
    QWidget* cadence_bar_ = nullptr;
    QSplitter* workspace_splitter_ = nullptr;
    QWidget* dom_panel_ = nullptr;
    QListWidget* market_list_ = nullptr;
    QLabel* market_title_ = nullptr;
    QLabel* market_meta_ = nullptr;
    QLabel* yes_quote_ = nullptr;
    QLabel* no_quote_ = nullptr;
    QLabel* close_countdown_ = nullptr;
    QLabel* contract_strip_ = nullptr;
    QLabel* venue_consensus_ = nullptr;
    QTextEdit* rules_ = nullptr;
    QTableWidget* book_table_ = nullptr;
    QTableWidget* trades_table_ = nullptr;
    QPushButton* yes_button_ = nullptr;
    QPushButton* no_button_ = nullptr;
    QDoubleSpinBox* price_ = nullptr;
    QSpinBox* contracts_ = nullptr;
    QLabel* cost_label_ = nullptr;
    QLabel* fee_label_ = nullptr;
    QLabel* payout_label_ = nullptr;
    QLabel* quote_health_ = nullptr;
    QLabel* recorder_health_ = nullptr;
    QLabel* gate_label_ = nullptr;
    QLabel* shadow_status_ = nullptr;
    QLabel* ladder_status_ = nullptr;
    QLabel* flow_status_ = nullptr;
    QLabel* flow_detail_ = nullptr;
    QTableWidget* ladder_table_ = nullptr;
    QLabel* live_automation_status_ = nullptr;
    QLabel* advisor_separation_status_ = nullptr;
    QLabel* legacy_live_badge_ = nullptr;
    QLabel* canary_badge_ = nullptr;
    QLabel* advisor_system_status_ = nullptr;
    QLabel* advisor_qualification_status_ = nullptr;
    QLabel* advisor_safety_status_ = nullptr;
    QLabel* advisor_activity_status_ = nullptr;
    QLabel* live_positions_summary_ = nullptr;
    QTableWidget* active_positions_table_ = nullptr;
    QLabel* pnl_summary_ = nullptr;
    QTableWidget* pnl_table_ = nullptr;
    QPushButton* live_automation_button_ = nullptr;
    QPushButton* kill_live_button_ = nullptr;
    QPushButton* shadow_button_ = nullptr;
    QLabel* dom_title_ = nullptr;
    QLabel* dom_status_ = nullptr;
    QPushButton* kraken_dom_button_ = nullptr;
    QPushButton* coinbase_dom_button_ = nullptr;
    crypto::CryptoOrderBook* spot_dom_ = nullptr;
    KalshiSimpleChart* reference_chart_ = nullptr;
    KalshiSimpleChart* contract_chart_ = nullptr;
    QLabel* chart_status_ = nullptr;
    QLabel* account_balance_label_ = nullptr;
    QLabel* position_label_ = nullptr;
    QLabel* cashout_label_ = nullptr;
    QPushButton* cashout_button_ = nullptr;
    QPushButton* live_order_button_ = nullptr;
    QString pending_order_kind_;
    QJsonObject pending_manual_order_;
    QSet<QString> recorded_fill_ids_;
    QSet<QString> recorded_auto_shadow_keys_;
    bool position_snapshot_pending_ = false;
    bool trade_ledger_loaded_ = false;
    bool daemon_status_fetching_ = false;
    bool daemon_restarting_ = false;
    QTimer* dom_timer_ = nullptr;
    QTimer* spot_dom_timer_ = nullptr;
    QTimer* reference_dom_reconnect_timer_ = nullptr;
    QTimer* evidence_timer_ = nullptr;
    QTimer* clock_timer_ = nullptr;
    QWebSocket* reference_dom_socket_ = nullptr;
    QMap<double, double> reference_dom_bids_;
    QMap<double, double> reference_dom_asks_;
    QString reference_dom_symbol_;
    QString reference_dom_venue_ = QStringLiteral("kraken");
    qint64 reference_dom_last_update_ms_ = 0;
    qint64 reference_spot_last_update_ms_ = 0;
    QString reference_spot_source_;
    qint64 last_kalshi_ticker_ms_ = 0;
    qint64 last_kalshi_trade_ms_ = 0;
    qint64 last_kalshi_book_ms_ = 0;
    qint64 recorder_started_ms_ = 0;
    qint64 ticker_events_recorded_ = 0;
    qint64 trade_events_recorded_ = 0;
    qint64 book_batches_recorded_ = 0;
    qint64 ladder_snapshots_recorded_ = 0;
    QString spot_symbol_ = QStringLiteral("BTC/USD");
    std::atomic<bool> dom_fetching_{false};
    std::atomic<bool> consensus_fetching_{false};
    std::atomic<bool> chart_fetching_{false};
    std::atomic<bool> spot_chart_fetching_{false};
    std::atomic<bool> volatility_fetching_{false};
    QHash<QString, services::edge_radar::KalshiVolatilityEstimate> volatility_cache_;
    QHash<QString, qint64> volatility_cache_refreshed_ms_;
    QString chart_timeframe_ = QStringLiteral("live");
    QString chart_asset_id_;
    qint64 last_chart_fetch_ms_ = 0;
    bool live_chart_seeded_ = false;
    double reference_spot_ = 0.0;
    double official_settlement_reference_ = 0.0;
    qint64 official_settlement_reference_ms_ = 0;
    QString official_settlement_index_;
    QVector<openmarketterminal::trading::Candle> reference_spot_history_;
    double trend_anchor_spot_ = 0.0;
    qint64 trend_anchor_ms_ = 0;
    qint64 last_consensus_snapshot_ms_ = 0;
    qint64 last_ladder_snapshot_ms_ = 0;
    qint64 last_forward_reconcile_ms_ = 0;
    qint64 last_account_activity_fetch_ms_ = 0;
    qint64 last_live_status_fetch_ms_ = 0;
    bool live_status_fetching_ = false;
    QJsonObject latest_legacy_live_status_;
    bool shadow_enabled_ = true;
    QVector<services::prediction::PredictionPosition> positions_;
    int shadow_candidates_ = 0;
    int shadow_confirmed_ = 0;
    int auto_shadow_records_ = 0;
    struct ShadowQuote {
        double price = 0.0;
        double initial_queue = 0.0;
        double smallest_queue = 0.0;
    };
    QHash<QString, ShadowQuote> shadow_quotes_;
};

} // namespace openmarketterminal::screens::kalshi
