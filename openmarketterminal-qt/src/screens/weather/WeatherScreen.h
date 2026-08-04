#pragma once

#include "screens/common/IStatefulScreen.h"
#include "screens/weather/WeatherEvidenceReader.h"
#include "services/prediction/PredictionTypes.h"

#include <QHash>
#include <QShowEvent>
#include <QTableWidget>
#include <QWidget>

class QLabel;
class QTabWidget;
class QTimer;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QPushButton;
class QVBoxLayout;

namespace openmarketterminal::services::prediction {
class PredictionExchangeAdapter;
}

namespace openmarketterminal::screens::crypto {
class CryptoOrderBook;
}

namespace openmarketterminal::screens {

/// Rich weather cockpit route (`weather`). Task 2 delivers the shell
/// and a left-side bracket browser sourced straight from the
/// PredictionExchangeAdapter (same "kalshi" adapter instance KalshiScreen
/// uses), filtered server-side to Kalshi's real "Climate and Weather"
/// category (NOT "Weather" — that slug resolves to zero series on Kalshi's
/// /series?category=… endpoint) and then client-side to the six weather
/// series the bot trades (KXHIGHNY/KXHIGHCHI/KXHIGHDEN/KXHIGHTSFO/
/// KXHIGHPHIL/KXHIGHTSEA — see is_bot_weather_series() in the .cpp), so the
/// browser shows exactly the bot's cities rather than every climate market.
/// Task 3 adds a right-side detail pane: selecting a browser row fetches and
/// renders that bracket's order book (via the reused CryptoOrderBook widget — the
/// same generic bid/ask depth widget KalshiScreen embeds for its spot
/// reference DOM) and recent trades. Task 5 adds a BOT sub-tab (summary,
/// decisions, positions) ported from the retired lean predictions screen,
/// filtered to sandbox_strategy kind='kalshi_weather' / edge_decision_journal
/// source='kalshi weather-plan'. Task 7 adds a paper-only order-entry mini
/// form for the selected bracket: it builds a PredictionExchangeAdapter
/// OrderRequest for parity with KalshiScreen's ticket fields, but — unlike
/// KalshiScreen's live/demo order path — NEVER calls adapter()->place_order().
/// Placing always simulates a local fill and never reaches the exchange,
/// so it cannot submit a live order regardless of whether a Kalshi account
/// is connected.
class WeatherScreen final : public QWidget, public IStatefulScreen {
    Q_OBJECT
  public:
    explicit WeatherScreen(QWidget* parent = nullptr);

    QVariantMap save_state() const override { return {}; }
    void restore_state(const QVariantMap&) override {}
    QString state_key() const override { return QStringLiteral("weather"); }
    int state_version() const override { return 1; }

  protected:
    void showEvent(QShowEvent* event) override;

  private:
    void build_ui();
    void wire_adapter();
    void refresh();
    services::prediction::PredictionExchangeAdapter* adapter() const;
    void populate_events(const QVector<services::prediction::PredictionEvent>& events);
    void populate_markets(const QVector<services::prediction::PredictionMarket>& markets);
    void select_bracket(int row);
    void render_order_book(const services::prediction::PredictionOrderBook& book);
    void render_trades(const QVector<services::prediction::PredictionTrade>& trades);
    void load_forecasts();
    void update_forecast_panel();

    // Tier 1: real load state machine (never a permanent spinner) — a fetch
    // is "pending" from refresh() until either populate_events/populate_markets
    // or handle_fetch_error() resolves it, with load_timeout_timer_ as the
    // backstop for a request whose reply never arrives at all.
    void handle_fetch_error(const QString& context, const QString& message);
    void handle_fetch_timeout();
    void set_status(const QString& text, const QString& color);

    // Tier 3: P&L summary strip on the BRACKETS tab — populate_bot_summary()
    // is the single place that queries sandbox_position/sandbox_strategy;
    // it passes its already-computed figures here to update the strip too,
    // so the BOT tab's stat row and the BRACKETS tab's strip never drift.
    void populate_pnl_strip(int open, int resolved, int wins, double pnl);

    // Task 5: bot panel (subsumes the retired lean weather predictions screen).
    QWidget* build_bot_tab();
    void refresh_bot_panel();
    void populate_bot_summary();
    void populate_bot_decisions();
    void populate_bot_positions();

    // Task 7: paper-only order entry for the selected bracket.
    void build_order_entry_panel(QVBoxLayout* into);
    void preview_paper_order();
    void place_paper_order();

    QLabel* status_label_ = nullptr;
    QLabel* count_label_ = nullptr;
    QPushButton* refresh_button_ = nullptr;
    QTableWidget* bracket_table_ = nullptr;
    bool first_show_ = true;

    // Tier 1: fetch state machine. fetch_pending_ is true from refresh()
    // until the corresponding events_ready/markets_ready or error_occurred
    // lands; load_timeout_timer_ is a backstop single-shot so a reply that
    // never arrives (adapter hangs, dropped connection) still resolves to a
    // visible "Fetch failed" state instead of leaving the browser stuck on
    // "Loading weather brackets…" forever.
    bool fetch_pending_ = false;
    QTimer* load_timeout_timer_ = nullptr;

    // Tier 1: rows in bracket_table_ interleave city group-header rows with
    // data rows (Tier 3 grouping); this parallel vector maps a table row to
    // its index into markets_, or -1 for a group-header row that carries no
    // market of its own.
    QVector<int> row_market_index_;

    // Task 3: bracket detail pane (order book + recent trades).
    QLabel* detail_title_ = nullptr;
    crypto::CryptoOrderBook* order_book_widget_ = nullptr;
    QTableWidget* trades_table_ = nullptr;
    QVector<services::prediction::PredictionMarket> markets_;
    services::prediction::PredictionMarket selected_;
    bool has_selection_ = false;

    // Task 4: forecast-vs-market panel, read from the producer's evidence
    // JSON (kalshi-weather-plan.json) via WeatherEvidenceReader and joined
    // to markets by ticker (PredictionMarket::key.market_id, which
    // KalshiRestClient populates from the raw Kalshi ticker — the same
    // string weather_producer.py's bracket_record() writes).
    QHash<QString, BracketForecast> forecasts_;
    QLabel* forecast_stats_label_ = nullptr;
    QLabel* forecast_edge_label_ = nullptr;
    QLabel* forecast_window_badge_ = nullptr;
    QLabel* forecast_threshold_label_ = nullptr;
    // Small forecast-vs-threshold number line (defined in WeatherScreen.cpp;
    // stored as a plain QWidget* here since only that translation unit needs
    // the concrete type).
    QWidget* forecast_line_widget_ = nullptr;

    // Tier 3: P&L summary strip on the BRACKETS tab — "positions · settled ·
    // net P&L · win%", the same figures the BOT tab's stat row shows, kept
    // in sync by populate_bot_summary() (single source of the SQL).
    QLabel* pnl_strip_positions_ = nullptr;
    QLabel* pnl_strip_settled_ = nullptr;
    QLabel* pnl_strip_net_ = nullptr;
    QLabel* pnl_strip_winrate_ = nullptr;

    // Task 5: bot panel — summary (open/closed, realized PnL, edge-status
    // badge), decisions table, positions table. Queries ported verbatim
    // from the retired lean weather predictions screen.
    QTabWidget* main_tabs_ = nullptr;
    QTimer* bot_timer_ = nullptr;
    QLabel* bot_status_label_ = nullptr;
    QLabel* bot_open_count_ = nullptr;
    QLabel* bot_resolved_count_ = nullptr;
    QLabel* bot_win_count_ = nullptr;
    QLabel* bot_net_pnl_ = nullptr;
    QTableWidget* bot_decisions_table_ = nullptr;
    QTableWidget* bot_positions_table_ = nullptr;

    // Task 7: paper-only order-entry mini form for the selected bracket.
    // PAPER ONLY: place_paper_order() never calls
    // PredictionExchangeAdapter::place_order() — it simulates the fill
    // locally, so no code path here can reach the live/demo exchange.
    QComboBox* order_side_combo_ = nullptr;
    QDoubleSpinBox* order_price_spin_ = nullptr;
    QSpinBox* order_qty_spin_ = nullptr;
    QPushButton* order_preview_button_ = nullptr;
    QPushButton* order_place_button_ = nullptr;
    QLabel* order_confirm_label_ = nullptr;
    QTableWidget* manual_orders_table_ = nullptr;
};

} // namespace openmarketterminal::screens
