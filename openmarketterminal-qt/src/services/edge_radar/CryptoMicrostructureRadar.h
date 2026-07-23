#pragma once

#include "services/crypto_latency/CryptoLatencyService.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <cmath>

namespace openmarketterminal::services::edge_radar {

// KalshiAutoEngine::estimate_realized_volatility annualizes over 24/7 minutes
// (365*24*60) with the time-of-day multiplier already applied. Convert back to
// a one-minute standard deviation expressed in basis points.
inline double annual_vol_to_per_min_bps(double annual_volatility) {
    if (annual_volatility <= 0.0)
        return 0.0;
    constexpr double kMinutesPerYear = 365.0 * 24.0 * 60.0;
    return annual_volatility / std::sqrt(kMinutesPerYear) * 10000.0;
}

// Express an observed move as a multiple of the ambient volatility scaled to
// the observation window (Brownian sqrt-time scaling). Returns 0 when the
// volatility estimate is unavailable so callers can fail open.
inline double move_noise_sigma(double observed_move_bps, double vol_per_min_bps,
                               int window_sec) {
    if (vol_per_min_bps <= 0.0 || window_sec <= 0)
        return 0.0;
    const double window_vol_bps = vol_per_min_bps * std::sqrt(window_sec / 60.0);
    return window_vol_bps > 0.0 ? std::abs(observed_move_bps) / window_vol_bps : 0.0;
}

struct CryptoMicrostructureSource {
    QString source;
    QString status;
    QString message;
    double price = 0.0;
    double best_bid = 0.0;
    double best_ask = 0.0;
    double bid_size = 0.0;
    double ask_size = 0.0;
    double microprice = 0.0;
    double top_book_imbalance = 0.0;
    double spread_bps = 0.0;
    qint64 age_ms = -1;
    qint64 quote_age_ms = -1;
    int ticks = 0;
};

struct CryptoMicrostructureWindow {
    int seconds = 0;
    qint64 coverage_ms = 0;
    int upticks = 0;
    int downticks = 0;
    int flat_ticks = 0;
    double start_price = 0.0;
    double end_price = 0.0;
    double move_pct = 0.0;
    double tape_pressure = 0.0;
    double aggressor_buy_volume = 0.0;
    double aggressor_sell_volume = 0.0;
    double unclassified_trade_volume = 0.0;
    double aggressor_pressure = 0.0;
    double aggressor_coverage = 0.0;
    int classified_trades = 0;
    int unclassified_trades = 0;
    bool available = false;
};

// Cost and noise inputs the radar cannot observe from ticks alone. The caller
// supplies them when computable; an absent context (the defaults) means net
// economics and the noise floor are UNKNOWN, and the snapshot must say so
// rather than treat missing data as zero-cost / zero-noise.
struct CryptoMicrostructureCostContext {
    // Round-trip venue economics from the shared fee table (services/crypto/
    // CryptoFees.h). fee_available=false = no venue profile supplied.
    bool fee_available = false;
    QString venue_key;
    double round_trip_fee_bps = 0.0; // entry + exit fees for the assumed liquidity mode
    double slippage_bps = 0.0;
    // Ambient realized volatility (KalshiAutoEngine::estimate_realized_volatility
    // over the stored 1-min tick series). vol_available=false = not computable.
    bool vol_available = false;
    double realized_vol_per_min_bps = 0.0;
    int realized_vol_samples = 0;
};

struct CryptoMicrostructureSnapshot {
    QString symbol = QStringLiteral("BTC-USD");
    QString call = QStringLiteral("NO TRADE");
    QString direction = QStringLiteral("flat");
    QString rationale;
    QString freshest_source;
    qint64 freshest_age_ms = -1;
    double reference_price = 0.0;
    double microprice = 0.0;
    double cross_source_spread_bps = 0.0;
    double book_pressure = 0.0;
    double tape_pressure = 0.0;
    double aggressor_pressure = 0.0;
    double aggressor_coverage = 0.0;
    double aggressor_buy_volume = 0.0;
    double aggressor_sell_volume = 0.0;
    int classified_trades = 0;
    double confidence = 0.0;
    int live_sources = 0;
    int top_book_sources = 0;
    int tick_count = 0;
    QString tick_pressure_kind = QStringLiteral("tick-direction proxy");
    QString aggressive_trade_flow_status = QStringLiteral("unavailable: dedicated aggressor feed not subscribed");
    // Net-of-round-trip economics (only meaningful when cost_context_available).
    bool cost_context_available = false;
    QString cost_venue_key;
    double round_trip_hurdle_bps = 0.0; // fees + slippage + observed cross-source divergence
    bool observed_move_available = false;
    double observed_move_bps = 0.0; // signed gross move of the primary window
    int observed_move_window_sec = 0;
    double net_move_bps = 0.0; // |observed_move_bps| - round_trip_hurdle_bps
    QString net_economics_status = QStringLiteral("unavailable: no venue fee profile supplied");
    // Noise floor (only meaningful when noise_sigma_available).
    bool realized_vol_available = false;
    double realized_vol_per_min_bps = 0.0;
    int realized_vol_samples = 0;
    bool noise_sigma_available = false;
    double observed_move_sigma = 0.0;
    QString noise_floor_status = QStringLiteral("unavailable: no realized-volatility estimate");
    QVector<CryptoMicrostructureSource> sources;
    QVector<CryptoMicrostructureWindow> windows;
};

class CryptoMicrostructureRadar {
  public:
    void clear();
    void add_tick(const openmarketterminal::services::crypto_latency::CryptoLatencyTick& tick);
    // Without a cost context the call logic is pressure-only (legacy behavior)
    // and the net/noise fields read explicitly unavailable. With one, a setup
    // that cannot clear the round-trip hurdle never renders TRADE CANDIDATE.
    CryptoMicrostructureSnapshot snapshot(
        const openmarketterminal::services::crypto_latency::CryptoLatencySnapshot& latency_snapshot,
        const CryptoMicrostructureCostContext& cost_context = {}) const;

    static QJsonObject to_json(const CryptoMicrostructureSnapshot& snapshot);

  private:
    CryptoMicrostructureWindow window(int seconds) const;
    QVector<openmarketterminal::services::crypto_latency::CryptoLatencyTick> ticks_;
    qint64 last_prune_newest_ms_ = 0;
};

} // namespace openmarketterminal::services::edge_radar
