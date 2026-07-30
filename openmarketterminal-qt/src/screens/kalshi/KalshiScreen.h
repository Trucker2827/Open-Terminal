#pragma once

#include "screens/kalshi/AutoCockpitPresentation.h"
#include "services/edge_radar/KalshiAutoEngine.h"
#include "services/prediction/kalshi/Kalshi15mCaptureController.h"

#include "services/prediction/PredictionTypes.h"
#include "trading/TradingTypes.h"

#include <QList>
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QWidget>

#include <atomic>
#include <functional>

class QComboBox;
class QDialog;
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
class QTabWidget;
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
    /// Re-fetch the market list. `background` marks a timer-driven refresh:
    /// list only, no badge flash, and the operator's selection survives it.
    void refresh(bool background = false);
    /// Timer tick. Asks decide_market_refresh whether a list fetch is due —
    /// periodic cadence, or immediately once the selected contract expires.
    void refresh_market_list_if_due();
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
    void update_calibrator_readout();
    void update_market_health();
    void update_strike_overlay();
    void refresh_flow_meter();
    /// Repaints the AUTO COCKPIT header from `present_auto_cockpit`. Driven by
    /// the 1s clock tick and NOT by the ladder engine, so a cockpit whose
    /// engine has stopped ages into STALE instead of freezing at its last-good
    /// text. Owns the ladder table whenever the inputs are not live: the rows
    /// are replaced with the stated reason rather than left showing prices
    /// derived from inputs the header has just called untrustworthy — and that
    /// includes the plan-summary line, which the engine pass writes only on the
    /// live path. `now_ms` is passed in so a single engine pass classifies its
    /// inputs against one clock reading: the header and the gate at the end of
    /// record_ladder_evidence must never straddle a staleness boundary and
    /// disagree about the same pass.
    void refresh_auto_cockpit_header(qint64 now_ms);
    /// The cockpit's inputs as they stand right now: live selection and market
    /// list state, plus the ladder-leg freshness carried in cockpit_ladder_.
    AutoCockpitInputs auto_cockpit_inputs() const;
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
    void refresh_arena_context_status();
    void refresh_bot_panel();
    /// Throws or clears the bot's kill switch by writing/removing
    /// kalshi-bot-stop.json — the same file `kalshi bot stop` writes, written
    /// through the same shared helper, in-process (never by shelling out, so
    /// the switch does not depend on finding openterminalcli).
    void toggle_bot_kill_switch();
    /// Opens the BOT COCKPIT — the decision-rain scene over the bot's own
    /// evidence. Read-only, like the BOT tab it opens from: the scene has no
    /// control of any kind on it.
    void open_bot_cockpit();
    void refresh_daemon_status();
    void restart_daemon();
    void run_live_cli(const QStringList& args, const std::function<void(const QJsonObject&, const QString&)>& done);
    QString cli_path() const;
    QString evidence_path(const QString& filename) const;
    services::prediction::PredictionExchangeAdapter* adapter() const;

    Kalshi15mCaptureController* capture_15m_ = nullptr;
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
    QLabel* calibrator_readout_ = nullptr;
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
    // AUTO COCKPIT header: what this surface is, and whether the inputs it
    // prices from are fresh. Repainted every clock tick.
    QLabel* cockpit_state_ = nullptr;
    QLabel* cockpit_markets_ = nullptr;
    QLabel* cockpit_books_ = nullptr;
    QLabel* cockpit_roles_ = nullptr;
    QPushButton* cockpit_open_bot_ = nullptr;
    QLabel* flow_status_ = nullptr;
    QLabel* flow_detail_ = nullptr;
    QTableWidget* ladder_table_ = nullptr;
    QLabel* live_automation_status_ = nullptr;
    QLabel* advisor_separation_status_ = nullptr;
    QLabel* legacy_live_badge_ = nullptr;
    QLabel* canary_badge_ = nullptr;
    // The concluded duel's record, on the ARENA tab. Always present: the card
    // states ARCHIVED / LIVE AGAIN / NO ADVISOR EVIDENCE rather than hiding.
    QLabel* concluded_duel_state_ = nullptr;
    QLabel* concluded_duel_record_ = nullptr;
    QLabel* advisor_system_status_ = nullptr;
    QLabel* advisor_qualification_status_ = nullptr;
    QLabel* advisor_safety_status_ = nullptr;
    QLabel* advisor_activity_status_ = nullptr;
    QLabel* arena_context_status_ = nullptr;
    QPushButton* arena_open_button_ = nullptr;
    QLabel* bot_status_ = nullptr;
    QLabel* bot_armed_ = nullptr;
    QLabel* bot_signal_ = nullptr;
    QLabel* bot_scoreboard_ = nullptr;
    // The conversion funnel and the pace to the sealed gate (issue #153) —
    // the scoreboard's missing denominator.
    QLabel* bot_funnel_ = nullptr;
    QLabel* bot_gate_ = nullptr;
    // WHAT THE RECORD TEACHES (issue #174) — the edge autopsy's standing
    // conclusions, one line per question with its sample size. Display only.
    QLabel* bot_lessons_ = nullptr;
    // The BOT panel's one control: the kill switch. It can only stop the bot
    // or clear that stop — it cannot arm, size, price, or place anything.
    QPushButton* bot_stop_button_ = nullptr;
    // Opens the cockpit scene. Its label changes to a suggestion while the
    // loop is running — the scene model decides that, not this widget.
    QPushButton* bot_cockpit_button_ = nullptr;
    QPointer<QDialog> bot_cockpit_dialog_;
    QListWidget* bot_decisions_ = nullptr;
    QLabel* live_positions_summary_ = nullptr;
    QTableWidget* active_positions_table_ = nullptr;
    QLabel* pnl_summary_ = nullptr;
    QLabel* pnl_scoreboard_ = nullptr;
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
    // The centre tab bar, kept so the cockpit can hand the operator over to the
    // BOT tab. The index is captured at addTab time — never searched by label,
    // which a later rename would break silently.
    QTabWidget* center_tabs_ = nullptr;
    int bot_tab_index_ = -1;
    // The ARENA tab's page, and the retired ADVISOR & CANARY page which is
    // built but NOT in the tab row. It is inserted after ARENA only while the
    // advisor loop is demonstrably writing again (the resurrection guard), and
    // removed when it falls silent — so the row holds only living surfaces.
    QWidget* arena_tab_page_ = nullptr;
    QWidget* advisor_panel_page_ = nullptr;
    // When populate_markets last replaced all_markets_. 0 = never listed.
    qint64 markets_listed_at_ms_ = 0;
    // Ladder-leg freshness carried over from the last surface the engine built
    // (KalshiSurfacePoint::quote_observed_at_ms), so the 1s header and the 5s
    // ladder can never quote different book ages. Cleared when the selection
    // changes: facts about the previous event are not facts about this one.
    AutoCockpitInputs cockpit_ladder_;
    QString pending_order_kind_;
    QJsonObject pending_manual_order_;
    QSet<QString> recorded_fill_ids_;
    QSet<QString> recorded_auto_shadow_keys_;
    bool position_snapshot_pending_ = false;
    bool trade_ledger_loaded_ = false;
    bool daemon_status_fetching_ = false;
    bool daemon_restarting_ = false;
    // Market-list rollover state (see MarketRollPresentation.h). The in-flight
    // pair is the no-storm guard: one list fetch at a time, released when the
    // payload lands or when it is presumed lost.
    bool market_list_fetch_in_flight_ = false;
    qint64 market_list_fetch_started_ms_ = 0;
    qint64 market_list_last_fetch_ms_ = 0;
    bool preserve_selection_on_populate_ = false;
    // Last refresh verdict written to the log, so an unchanged one stays quiet.
    QString market_list_logged_reason_;
    QHash<QString, qint64> series_detail_fetched_ms_;
    QTimer* market_list_timer_ = nullptr;
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
    // Coinbase spot-DOM endpoint choice: Advanced Trade WS first; flips to the
    // legacy Exchange feed on a coinbase connection that never produced a book
    // (fallback while the sunset feed still answers).
    bool reference_dom_use_legacy_coinbase_ = false;
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
    QJsonObject calibrator_report_;
    qint64 calibrator_report_read_ms_ = 0;
    // advisor_competition_report.json is ~750KB and frozen once the duel ends,
    // so it is read at most once per session. Not gated on the verdict: the
    // ARENA card shows the record whether or not the loop is writing again.
    QJsonObject advisor_duel_report_;
    bool advisor_duel_report_read_ = false;
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
