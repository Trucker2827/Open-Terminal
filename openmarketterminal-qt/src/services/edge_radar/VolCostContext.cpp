#include "services/edge_radar/VolCostContext.h"

#include "services/edge_radar/KalshiAutoEngine.h"
#include "storage/repositories/EdgePredictionModelRepository.h"

#include <algorithm>

namespace openmarketterminal::services::edge_radar {

CryptoMicrostructureCostContext vol_cost_context_for(const QString& base_symbol,
                                                     qint64 now_ms) {
    CryptoMicrostructureCostContext ctx;
    if (base_symbol.trimmed().isEmpty() || now_ms <= 0)
        return ctx;

    // Same window and estimator the scalp gate uses, so the two cannot drift
    // apart and report different volatility for the same instant.
    const auto series = EdgePredictionModelRepository::instance().list_spot_price_series_since(
        base_symbol.trimmed().toUpper(), now_ms - 8LL * 60 * 60 * 1000, 30'000);
    if (series.is_err())
        return ctx;

    QVector<KalshiTimedPrice> prices;
    prices.reserve(series.value().size());
    for (const auto& tick : series.value())
        prices.append(KalshiTimedPrice{tick.exchange_ts, tick.price});

    const auto vol = KalshiAutoEngine::estimate_realized_volatility(prices, now_ms);
    const double per_min_bps = annual_vol_to_per_min_bps(vol.annual_volatility);
    if (vol.ready && per_min_bps > 0.0) {
        ctx.vol_available = true;
        ctx.realized_vol_per_min_bps = per_min_bps;
        ctx.realized_vol_samples = vol.sample_count;
    }
    return ctx;
}

CryptoMicrostructureCostContext with_fee_context(CryptoMicrostructureCostContext base,
                                                 const CryptoFeeInputs& fees) {
    // No profile means UNKNOWN, and the radar must keep saying so. Treating a
    // missing rate as zero would let a gross move read as net-of-cost.
    if (fees.one_way_fee_bps < 0.0) return base;
    base.fee_available = true;
    base.venue_key = fees.venue_key;
    base.round_trip_fee_bps = fees.one_way_fee_bps * 2.0;
    base.slippage_bps = std::max(0.0, fees.slippage_bps);
    return base;
}

}  // namespace openmarketterminal::services::edge_radar
