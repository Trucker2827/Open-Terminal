#pragma once

#include "screens/common/IStatefulScreen.h"
#include "screens/manual_trading/ManualTradingPnl.h"
#include "services/prediction/PredictionTypes.h"

#include <QDateTime>
#include <QHash>
#include <QShowEvent>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QHideEvent;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;
class QVBoxLayout;

namespace openmarketterminal::services::prediction {
class PredictionExchangeAdapter;
}

namespace openmarketterminal::screens {

/// Manual Trading (paper) — a teaching surface embedded in the Kalshi
/// predictions window (KalshiScreen's category_stack_, the "Manual" family).
///
/// Two side-by-side paper accounts, "KALSHI" and "COINBASE (powered by
/// Kalshi)", both fed by the SAME live Kalshi BTC price feed (they are the
/// same order book — Coinbase has no API of its own, so both panels read the
/// Kalshi adapter). The two accounts are kept as SEPARATE paper ledgers so a
/// user can hold YES on one and NO on the other at the same time WITHOUT the
/// positions netting. A combined scoreboard marks both ledgers to the current
/// mid and shows total staked, fees, net P&L, and open positions — which
/// teaches, safely, that opposite YES/NO across the two panels on the same
/// book nets to roughly zero minus fees.
///
/// PAPER ONLY: this screen NEVER calls PredictionExchangeAdapter::place_order()
/// or any live/exchange path. A placed order simply appends an in-memory
/// PaperPosition; the fill is simulated locally, exactly like WeatherScreen's
/// order-entry mini form. There is no code path here that can reach a live or
/// demo exchange, regardless of whether a Kalshi account is connected.
class ManualTradingScreen final : public QWidget, public IStatefulScreen {
    Q_OBJECT
  public:
    explicit ManualTradingScreen(QWidget* parent = nullptr);

    QVariantMap save_state() const override { return {}; }
    void restore_state(const QVariantMap&) override {}
    QString state_key() const override { return QStringLiteral("manual_trading"); }
    int state_version() const override { return 1; }

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    // A single paper "position": {account, market, side, price, qty, fee}.
    // Purely in-memory and session-local — never persisted, never sent to any
    // exchange. Net P&L is computed by marking `entry_price` to the market's
    // current mid (see mark_price()); the fee is a sunk cost already paid.
    struct PaperPosition {
        QString account;       // "KALSHI" | "COINBASE"
        QString market_id;     // Kalshi market ticker (key.market_id)
        QString market_label;  // human-readable contract label at fill time
        QString side;          // "YES" | "NO"
        double entry_price = 0.0;  // probability 0..1 (= $ per contract)
        int qty = 0;
        double fee = 0.0;
        QDateTime placed_at;
    };

    // Per-account panel widgets. Index 0 = KALSHI, 1 = COINBASE. Both read the
    // SAME selected market's prices; the only thing that differs is which
    // ledger a fill lands in.
    struct AccountPanel {
        QString name;
        QLabel* yes_price = nullptr;
        QLabel* no_price = nullptr;
        QComboBox* side = nullptr;
        QDoubleSpinBox* price = nullptr;
        QSpinBox* qty = nullptr;
        QPushButton* preview = nullptr;
        QPushButton* place = nullptr;
        QLabel* confirm = nullptr;
        QTableWidget* positions = nullptr;
    };

    void build_ui();
    QWidget* build_account_panel(int idx, const QString& name, const QString& subtitle);
    void build_scoreboard(QVBoxLayout* into);
    void wire_adapter();
    void set_cadence(const QString& cadence);
    void refresh();
    services::prediction::PredictionExchangeAdapter* adapter() const;

    void populate_events(const QVector<services::prediction::PredictionEvent>& events);
    void handle_markets(const QVector<services::prediction::PredictionMarket>& markets);
    void rebuild_selector();
    void on_market_selected();
    void update_prices();
    bool current_market(services::prediction::PredictionMarket* out) const;
    double mark_price(const QString& market_id, const QString& side) const;

    void preview_order(int idx);
    void place_order(int idx);
    void refresh_positions_table(int idx);
    void update_scoreboard();

    QString cadence_ = QStringLiteral("fifteen_min");
    QLabel* status_label_ = nullptr;
    QComboBox* market_combo_ = nullptr;
    QWidget* cadence_bar_ = nullptr;
    QPushButton* refresh_button_ = nullptr;

    // BTC contracts for the currently-selected cadence, keyed by ticker. Only
    // ever holds one cadence at a time (set_cadence clears it), so the two
    // Kalshi list_events endpoints are never in flight at once — one fetch per
    // toggle, mirroring KalshiScreen's own cadence bar.
    QHash<QString, services::prediction::PredictionMarket> markets_by_id_;
    QString selected_market_id_;
    bool fetch_pending_ = false;
    bool first_show_ = true;
    bool rebuilding_selector_ = false;
    QTimer* refresh_timer_ = nullptr;
    // Guards against the 15s refresh clobbering live user input. The combo is
    // only rebuilt when the set of contracts actually changes, and a price
    // spin is only re-seeded to the current mid when its market or side
    // changes — not on every tick, which would erase a typed limit price.
    QStringList last_selector_ids_;
    QString last_priced_market_id_;

    AccountPanel panels_[2];
    QVector<PaperPosition> positions_;

    // Combined scoreboard: index 0 = KALSHI, 1 = COINBASE, 2 = COMBINED.
    QLabel* sb_staked_[3] = {nullptr, nullptr, nullptr};
    QLabel* sb_fees_[3] = {nullptr, nullptr, nullptr};
    QLabel* sb_pnl_[3] = {nullptr, nullptr, nullptr};
    QLabel* sb_positions_[3] = {nullptr, nullptr, nullptr};
};

} // namespace openmarketterminal::screens
