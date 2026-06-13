// src/algo_engine/DeploymentRunner.h
#pragma once
#include "algo_engine/AlgoEngineTypes.h"
#include "algo_engine/CandleAggregator.h"
#include "algo_engine/PositionManager.h"
#include "services/algo_trading/AlgoTradingTypes.h"

#include <QObject>
#include <QTimer>

#include <atomic>
#include <memory>

namespace openmarketterminal::algo {

class DeploymentRunner : public QObject {
    Q_OBJECT
public:
    explicit DeploymentRunner(const openmarketterminal::services::algo::AlgoDeployment& deployment,
                              const openmarketterminal::services::algo::AlgoStrategy& strategy,
                              QObject* parent = nullptr);
    ~DeploymentRunner() override;

    void start();
    void pause();
    void resume();
    void stop();
    bool is_running() const { return running_.load(); }
    bool is_paused() const { return paused_.load(); }
    QString deployment_id() const { return deployment_.id; }

    AlgoMetrics metrics() const;
    AlgoPosition position() const;

signals:
    void trade_executed(const openmarketterminal::algo::AlgoTradeRecord& trade);
    void metrics_updated(const QString& deployment_id, const openmarketterminal::algo::AlgoMetrics& metrics);
    void status_changed(const QString& deployment_id, const QString& status);
    void order_requested(const openmarketterminal::algo::AlgoOrderSignal& signal);
    void error_occurred(const QString& deployment_id, const QString& error);
    // Real-time snapshot for the Dashboard (LTP, P&L, position, per-condition status).
    void live_update(const QString& deployment_id, const openmarketterminal::algo::AlgoLiveSnapshot& snap);

public slots:
    void on_order_filled(const QString& broker_order_id, double fill_price, double fill_qty);
    void on_order_rejected(const QString& broker_order_id, const QString& reason);

private slots:
    void on_candle_closed(const openmarketterminal::algo::OhlcvCandle& candle);
    void on_tick_data(const QVariant& data);
    void on_heartbeat();

private:
    // Live market data: subscribe to the shared per-account quote feed via
    // DataStreamManager (the same feed the Equity watchlist uses). Quotes arrive
    // on the engine thread via on_tick_data(). No private broker polling — one
    // connection per (account, symbol) is shared across all consumers.
    void start_market_data();
    void stop_market_data();
    // Builds the candle window used for live (per-tick) evaluation: the closed
    // history plus the previous and current tick as the last two bars, so a
    // crossover is detected tick-to-tick against the live price.
    QVector<OhlcvCandle> live_eval_window(double price) const;
    // Evaluates entry/exit each tick (live timeframe only) and emits a snapshot.
    void evaluate_live(double price);
    // Pushes the real-time snapshot (LTP, P&L, position, per-condition status) to
    // the Dashboard, throttled. `note` is a short activity line.
    void emit_live_snapshot(double price, const QString& note);
    void evaluate_entry(const QVector<OhlcvCandle>& candles);
    void evaluate_exit(const QVector<OhlcvCandle>& candles);
    void emit_order_signal(const AlgoOrderSignal& signal);
    void persist_trade(const AlgoTradeRecord& trade);
    void persist_metrics();
    // True when the runner holds an open position of EITHER kind — a single-symbol
    // equity position or a multi-leg F&O basket. Entry/exit routing must treat a
    // basket as "in position" or it would keep re-entering (has_position() alone
    // only sees the single-symbol path).
    bool in_position() const;
    // Re-seed position + metrics from algo_metrics so a resumed deployment continues
    // its open position across restarts (no-op for a fresh deploy — no row yet).
    void restore_state_from_db();
    void update_deployment_status(const QString& status);

    openmarketterminal::services::algo::AlgoDeployment deployment_;
    openmarketterminal::services::algo::AlgoStrategy strategy_;
    Timeframe timeframe_;

    std::unique_ptr<CandleAggregator> aggregator_;
    std::unique_ptr<PositionManager> position_mgr_;

    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    QTimer* heartbeat_timer_ = nullptr;
    int64_t last_heartbeat_ms_ = 0;
    bool first_tick_logged_ = false;    // log the first live quote once, for trackability
    bool live_mode_ = false;            // timeframe == "live" → evaluate per tick
    int64_t last_emit_ms_ = 0;          // throttle for live_update emission
    double last_tick_price_ = 0;        // previous tick price → tick-to-tick crossovers

    struct PendingOrder {
        QString broker_order_id;
        AlgoOrderSignal signal;
        int64_t submitted_ms = 0;
    };
    QVector<PendingOrder> pending_orders_;
};

} // namespace openmarketterminal::algo
