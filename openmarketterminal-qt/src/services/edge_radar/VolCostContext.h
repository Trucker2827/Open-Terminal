#pragma once

// Build the volatility half of a CryptoMicrostructureCostContext.
//
// CryptoMicrostructureRadar::snapshot() takes the cost context as a DEFAULTED
// parameter, and both CLI capture paths were passing nothing:
//
//   CommandDispatch.cpp  radar.snapshot(service.snapshot())
//   ServeCommand.cpp     radars_[symbol].snapshot(latency_snapshot)
//
// so every CLI-produced row reported
// `noise_floor_status = "unavailable: no realized-volatility estimate"`, and
// `realized_vol_available = false`, for 86,148 consecutive decision rows.
//
// That message was misleading rather than true: the scalp gate computes the
// very same estimate a level away and had it all along (2.847 bps/min from
// 356 samples when measured). Nothing was broken -- the reporting path was
// simply never wired.
//
// The volatility half is reporting-only: in the radar, `cost_context_available`
// -- the flag that lets the hurdle and noise-floor blockers veto a TRADE
// CANDIDATE -- is set by `fee_available`, NOT by the vol fields. Supplying vol
// alone populates the reporting fields and leaves the call logic untouched.
//
// The fee half (`with_fee_context`) DOES tighten what the radar recommends, so
// it is supplied only on the crypto recommend/scalp paths, which already resolve
// a venue fee profile and already apply the same economics a level down. It is
// deliberately NOT supplied on the plain `edge microstructure` display command
// (no venue to resolve) nor on any Kalshi path -- the serve-loop Kalshi radar
// feeds a signal state machine where a crypto-venue round-trip veto would be a
// category error.
//
// Measured before wiring: observed 5s moves ran 0.0-1.8 bps against a
// cross-source spread of 1.0-6.1 bps, so the hurdle is not cleared even at zero
// fees. The gate blocking every candidate is the honest answer, not a plumbing
// gap -- `observed_move_available` was true in every sample.

#include <QString>

#include "services/edge_radar/CryptoMicrostructureRadar.h"

namespace openmarketterminal::services::edge_radar {

/// Ambient realized volatility for `base_symbol` (the tick store's bare form,
/// e.g. "BTC"), read from the stored 1-min series with the same estimator the
/// scalp gate uses. Returns a context with `vol_available == false` when there
/// is no usable series -- never a fabricated number.
CryptoMicrostructureCostContext vol_cost_context_for(const QString& base_symbol,
                                                     qint64 now_ms);

/// The venue economics a caller has already resolved. `one_way_fee_bps` is the
/// maker or taker rate for the assumed liquidity mode; negative means the caller
/// has no profile and the fee half must stay unavailable.
struct CryptoFeeInputs {
    QString venue_key;
    double one_way_fee_bps = -1.0;
    double slippage_bps = 0.0;
};

/// Adds the fee half to `base` (typically the result of vol_cost_context_for),
/// leaving the vol fields untouched. The round trip is `one_way_fee_bps * 2`,
/// matching the scalp gate's `(fee_bps * 2.0) + slippage_bps` in
/// EdgeJournalCommandsC.cpp so the radar and the gate cannot disagree.
///
/// A negative `one_way_fee_bps` returns `base` unchanged -- an absent profile
/// must read as UNKNOWN, never as zero cost. Zero is a legitimate rate (fee
/// waiver, rebate tier) and does set `fee_available`.
CryptoMicrostructureCostContext with_fee_context(CryptoMicrostructureCostContext base,
                                                 const CryptoFeeInputs& fees);

}  // namespace openmarketterminal::services::edge_radar
