#pragma once
// Ambient-vol + move-in-sigmas readout for the crypto ticker bar (issue #97).
// Pure state/formatting logic so the available/warming/unavailable states are
// unit-testable without widgets. The math is the CLI scalp gate's: realized
// vol from KalshiAutoEngine::estimate_realized_volatility converted through
// annual_vol_to_per_min_bps, sigma through move_noise_sigma (both shared in
// services/edge_radar/CryptoMicrostructureRadar.h) — never re-derived here.

#include "services/edge_radar/CryptoMicrostructureRadar.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdlib>

namespace openmarketterminal::crypto {

// Nominal readout window plus honesty bounds: the baseline sample must be at
// least the minimum coverage old before the bar claims a move (a younger
// window would dress a seconds-long blip up as a one-minute move), and
// samples beyond the max age never serve as baseline (a stalled stream must
// not fabricate a fresh reading).
constexpr int kTickerMoveWindowSec = 60;
constexpr int kTickerMoveMinCoverageSec = 45;
constexpr int kTickerMoveMaxAgeSec = 90;

struct TickerMoveSample {
    qint64 ts_ms = 0;
    double price = 0.0;
};

// Rolling price window fed by the existing ticker refresh — no new feeds.
class TickerMoveWindow {
  public:
    struct Move {
        bool available = false;
        double move_bps = 0.0; // signed (last / baseline − 1) in bps
        int window_sec = 0;    // actual elapsed span used for sqrt-time scaling
        int coverage_sec = 0;  // history held so far (drives the warming text)
    };

    void clear() { samples_.clear(); }

    void record(qint64 ts_ms, double price) {
        if (ts_ms <= 0 || price <= 0.0)
            return;
        if (!samples_.isEmpty() && ts_ms <= samples_.last().ts_ms)
            return;
        samples_.append({ts_ms, price});
        const qint64 cutoff = ts_ms - qint64(kTickerMoveMaxAgeSec) * 1000;
        while (!samples_.isEmpty() && samples_.first().ts_ms < cutoff)
            samples_.removeFirst();
    }

    Move move(qint64 now_ms) const {
        Move out;
        if (samples_.isEmpty() || now_ms <= 0)
            return out;
        out.coverage_sec = static_cast<int>((now_ms - samples_.first().ts_ms) / 1000);
        // Baseline: the sample closest to the nominal window ago, within age bounds.
        const qint64 target_ms = now_ms - qint64(kTickerMoveWindowSec) * 1000;
        const TickerMoveSample* baseline = nullptr;
        qint64 best_diff = 0;
        for (const auto& s : samples_) {
            if (now_ms - s.ts_ms > qint64(kTickerMoveMaxAgeSec) * 1000)
                continue;
            const qint64 diff = std::llabs(s.ts_ms - target_ms);
            if (!baseline || diff < best_diff) {
                baseline = &s;
                best_diff = diff;
            }
        }
        if (!baseline)
            return out;
        const qint64 age_ms = now_ms - baseline->ts_ms;
        if (age_ms < qint64(kTickerMoveMinCoverageSec) * 1000)
            return out;
        const double last_price = samples_.last().price;
        if (baseline->price <= 0.0 || last_price <= 0.0)
            return out;
        out.available = true;
        out.window_sec = static_cast<int>(age_ms / 1000);
        out.move_bps = (last_price / baseline->price - 1.0) * 10000.0;
        return out;
    }

  private:
    QVector<TickerMoveSample> samples_;
};

// Realized-vol estimate as the bar caches it between 60s estimator refreshes.
// ready=false covers both "no stored series" and "not enough samples"; the
// reason string names which, sample_count says how far warming has come.
struct TickerVolState {
    bool ready = false;
    double vol_per_min_bps = 0.0;
    int sample_count = 0;
    QString reason;
};

struct TickerVolSigmaDisplay {
    QString text;
    QString tooltip;
};

inline TickerVolSigmaDisplay format_ticker_vol_sigma(const TickerVolState& vol,
                                                     const TickerMoveWindow::Move& move) {
    TickerVolSigmaDisplay out;
    QString vol_part;
    QString sigma_part;
    QStringList tips;

    const bool vol_ok = vol.ready && vol.vol_per_min_bps > 0.0;
    if (vol_ok) {
        vol_part = QString("VOL %1bps/m").arg(vol.vol_per_min_bps, 0, 'f', 1);
        tips << QString("Ambient realized vol %1 bps/min from %2 stored 1-min samples "
                        "(same EWMA estimator as the CLI scalp gate).")
                    .arg(vol.vol_per_min_bps, 0, 'f', 2)
                    .arg(vol.sample_count);
    } else {
        vol_part = QStringLiteral("VOL --");
        tips << QString("Realized vol unavailable (%1 samples): %2")
                    .arg(vol.sample_count)
                    .arg(vol.reason.isEmpty() ? QStringLiteral("no estimate yet") : vol.reason);
    }

    if (vol_ok && move.available) {
        const double sigma = services::edge_radar::move_noise_sigma(
            move.move_bps, vol.vol_per_min_bps, move.window_sec);
        sigma_part = QString("1m %1σ").arg(sigma, 0, 'f', 1);
        tips << QString("Move %1%2bps over %3s → %4σ of ambient noise.")
                    .arg(move.move_bps > 0.0 ? "+" : "")
                    .arg(move.move_bps, 0, 'f', 1)
                    .arg(move.window_sec)
                    .arg(sigma, 0, 'f', 2);
    } else {
        sigma_part = QStringLiteral("1m --σ");
        if (vol_ok)
            tips << QString("Move window warming: %1s of %2s.")
                        .arg(move.coverage_sec)
                        .arg(kTickerMoveWindowSec);
        else
            tips << QStringLiteral("Move-in-sigmas needs the realized-vol estimate.");
    }

    tips << QStringLiteral("Display only — mirrors the CLI scalp gate's noise floor.");
    out.text = vol_part + QStringLiteral(" · ") + sigma_part;
    out.tooltip = tips.join(QLatin1Char('\n'));
    return out;
}

} // namespace openmarketterminal::crypto
