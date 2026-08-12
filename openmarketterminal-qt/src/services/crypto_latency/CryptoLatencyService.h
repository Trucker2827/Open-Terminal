#pragma once

#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QWebSocket;
class QTcpSocket;
class QTimer;

namespace openmarketterminal::services::crypto_latency {

struct CryptoLatencyTick {
    QString source;
    QString symbol;
    QString venue_symbol;
    double price = 0.0;
    double best_bid = 0.0;
    double best_ask = 0.0;
    double bid_size = 0.0;
    double ask_size = 0.0;
    qint64 exchange_ts_ms = 0;
    qint64 received_ts_ms = 0;
    qint64 sequence = 0;
    bool is_trade = false;
    QString aggressor_side; // buy or sell; empty when the venue does not identify it
    double trade_size = 0.0;
};

struct CryptoLatencySourceState {
    QString source;
    QString status;
    QString error;
    QString last_message_type;
    qint64 connected_at_ms = 0;
    qint64 last_tick_ms = 0;
    qint64 last_message_ms = 0;
    int raw_messages = 0;
    int ticks = 0;
    int reconnect_attempts = 0;
    int last_close_code = 0;
    int reconnect_delay_ms = 0;
    bool rate_limited = false;
};

struct CryptoLatencySnapshot {
    QString symbol;
    QVector<CryptoLatencyTick> latest_ticks;
    QVector<CryptoLatencySourceState> sources;
    QString freshest_source;
    qint64 freshest_age_ms = -1;
    double min_price = 0.0;
    double max_price = 0.0;
    double mid_price = 0.0;
    double cross_source_spread_bps = 0.0;
    qint64 newest_tick_ms = 0;
    qint64 oldest_tick_ms = 0;
    int live_sources = 0;
    int total_ticks = 0;
};

/// How many sources are genuinely usable right now.
///
/// A source counts only when it is BOTH fresh and quoting a two-sided book
/// with size on each side. Recency alone was the old rule, and it let a thin
/// or heartbeat-shaped feed satisfy the "2 live sources" bar on data nobody
/// could actually trade against: a venue ticking a bare last-price every few
/// seconds looked identical to one streaming a real book.
///
/// The bar mirrors the one CryptoMicrostructureRadar already applies for
/// top_book_sources -- both sides priced and sized -- so the two counters stop
/// disagreeing about what "live" means.
///
/// Strictly tightening: this can only ever return fewer sources than the old
/// recency-only rule, never more.
int crypto_latency_count_live_sources(const QVector<CryptoLatencySourceState>& states,
                                      const QVector<CryptoLatencyTick>& latest_ticks,
                                      qint64 now_ms);

class CryptoLatencyService : public QObject {
    Q_OBJECT
  public:
    explicit CryptoLatencyService(QObject* parent = nullptr);
    ~CryptoLatencyService() override;

    static QStringList default_sources();
    static QStringList supported_sources();
    static QString normalize_symbol(QString symbol);
    static QJsonObject tick_to_json(const CryptoLatencyTick& tick);
    static QJsonObject source_to_json(const CryptoLatencySourceState& state);
    static QJsonObject snapshot_to_json(const CryptoLatencySnapshot& snapshot);
    static CryptoLatencySnapshot filtered_snapshot(const CryptoLatencySnapshot& snapshot,
                                                    const QStringList& sources);

    void start(const QString& symbol, const QStringList& sources = default_sources(),
               int initial_delay_ms = 0);
    void stop();
    bool is_running() const { return running_; }
    CryptoLatencySnapshot snapshot() const;

  signals:
    void tick_received(const openmarketterminal::services::crypto_latency::CryptoLatencyTick& tick);
    void source_state_changed(const openmarketterminal::services::crypto_latency::CryptoLatencySourceState& state);
    void snapshot_changed(const openmarketterminal::services::crypto_latency::CryptoLatencySnapshot& snapshot);

  private:
    struct Feed {
        QString source;
        QString url;
        QString venue_symbol;
        QWebSocket* socket = nullptr;
        QTcpSocket* tcp_socket = nullptr;
    };

    struct TerminalState {
        QStringList rows;
        int row = 0;
        int col = 0;
        bool in_escape = false;
        QString escape;
    };

    struct GeminiBook {
        QMap<double, double> bids;
        QMap<double, double> asks;
    };

    Feed make_feed(const QString& source, const QString& symbol) const;
    void open_feed(const Feed& feed);
    void open_tcp_feed(const Feed& feed);
    // Schedule a single-shot (re)open of `source` after `delay_ms`, tearing down
    // any prior socket for that source first. Reused for staggered startup opens.
    void schedule_open(const QString& source, int delay_ms);
    // Backoff-driven reconnect scheduling off the socket lifecycle. Dedups the
    // errorOccurred+disconnected pair via the per-source timer's active state.
    void schedule_reconnect(const QString& source, const QString& error_string);
    /// Tears down any socket that BELIEVES it is connected but has delivered
    /// nothing for kSilentFeedLimitMs, so a half-open connection self-heals
    /// instead of sitting silent until someone restarts the daemon.
    void sweep_silent_feeds();
    void handle_text(const QString& source, const QString& venue_symbol, const QString& text);
    void handle_tcp_bytes(const QString& source, const QString& venue_symbol, const QByteArray& bytes);
    void apply_terminal_escape(TerminalState& term, const QString& seq);
    double parse_bitcointicker_price(const TerminalState& term) const;
    bool parse_bitcointicker_book(const TerminalState& term, double* bid, double* ask) const;
    void note_message(const QString& source, const QString& message_type);
    void emit_tick(CryptoLatencyTick tick);
    void set_state(const QString& source, const QString& status, const QString& error = {});

    QString symbol_;
    bool running_ = false;
    qint64 sequence_ = 0;
    QHash<QString, Feed> feeds_;
    QHash<QString, CryptoLatencyTick> latest_;
    QHash<QString, CryptoLatencySourceState> states_;
    QHash<QString, TerminalState> terminal_states_;
    QHash<QString, GeminiBook> gemini_books_;
    QSet<QString> wanted_sources_;
    QHash<QString, QTimer*> reconnect_timers_;
    QTimer* silence_watchdog_ = nullptr;
    /// Measured: a Coinbase socket stayed silent 74 minutes reporting
    /// error=none, while every live stream on every venue delivered at ~1s.
    /// Two minutes is far above the natural cadence and far below the outage.
    static constexpr qint64 kSilentFeedLimitMs = 120'000;
    static constexpr int kSilenceSweepMs = 30'000;
};

} // namespace openmarketterminal::services::crypto_latency

Q_DECLARE_METATYPE(openmarketterminal::services::crypto_latency::CryptoLatencyTick)
Q_DECLARE_METATYPE(openmarketterminal::services::crypto_latency::CryptoLatencySourceState)
Q_DECLARE_METATYPE(openmarketterminal::services::crypto_latency::CryptoLatencySnapshot)
