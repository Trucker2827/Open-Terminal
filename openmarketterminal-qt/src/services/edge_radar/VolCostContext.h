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
// VOLATILITY ONLY, deliberately. In the radar, `cost_context_available` -- the
// flag that lets the hurdle and noise-floor blockers veto a TRADE CANDIDATE --
// is set by `fee_available`, NOT by the vol fields. Supplying vol alone
// populates the reporting fields and leaves the call logic untouched. Wiring
// fees as well would additionally tighten what the radar recommends; that is a
// separate decision, not a reporting fix.

#include <QString>

#include "services/edge_radar/CryptoMicrostructureRadar.h"

namespace openmarketterminal::services::edge_radar {

/// Ambient realized volatility for `base_symbol` (the tick store's bare form,
/// e.g. "BTC"), read from the stored 1-min series with the same estimator the
/// scalp gate uses. Returns a context with `vol_available == false` when there
/// is no usable series -- never a fabricated number.
CryptoMicrostructureCostContext vol_cost_context_for(const QString& base_symbol,
                                                     qint64 now_ms);

}  // namespace openmarketterminal::services::edge_radar
