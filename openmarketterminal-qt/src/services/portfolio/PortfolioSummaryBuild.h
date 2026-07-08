// src/services/portfolio/PortfolioSummaryBuild.h
#pragma once
// Pure portfolio-summary construction, split out of
// PortfolioService::finalize_summary so the per-holding pricing + FX conversion
// + aggregation (and, critically, the "is this summary safe to snapshot?"
// decision that gates NAV snapshot persistence) can be unit-tested without
// standing up the service (DB + market data + SectorResolver singleton +
// signals).
//
// The base math is reproduced verbatim from the inline code it replaced; the
// additions are the per-holding `priced` flag, the FX rate conversion (all
// values reported in the portfolio's base currency), and the snapshot-safety
// tally.

#include "screens/portfolio/PortfolioTypes.h"
#include "services/markets/MarketDataService.h" // services::QuoteData

#include <QHash>
#include <QString>
#include <QVector>

#include <functional>

namespace openmarketterminal::portfolio {

struct BuiltSummary {
    PortfolioSummary summary;
    /// Holdings with no entry in the quote map: their price fell back to the
    /// average buy price, so their market value / P&L are fabricated. Callers
    /// MUST NOT persist a NAV snapshot when this is > 0 — the total market value
    /// carries the fabricated mark and would permanently contaminate history.
    int unpriced_count = 0;
    /// Holdings whose FX rate to the portfolio's base currency was NOT resolved
    /// (rate_lookup returned <= 0): either the listing currency isn't known yet
    /// or the FX pair hasn't loaded. Their values were converted at 1.0 for
    /// DISPLAY, but that is a guess — persisting it would contaminate NAV history
    /// the same way a fabricated price does, so it also blocks the snapshot.
    int fx_unresolved_count = 0;

    /// A summary is safe to persist as a NAV snapshot only when every holding was
    /// priced from a live quote AND converted at a resolved FX rate. Otherwise the
    /// total market value carries a guessed mark that must not enter the permanent
    /// performance history. Pure + unit-tested — this is the snapshot gate.
    bool snapshot_safe() const { return unpriced_count == 0 && fx_unresolved_count == 0; }
};

/// Build a PortfolioSummary from stored @p assets and a live @p quote_map keyed
/// by the asset's canonical `symbol`. @p sector_lookup supplies the fallback
/// sector for assets that have none. @p rate_lookup returns the multiplier that
/// converts an asset's native (listing) currency into the portfolio's base
/// currency — 1.0 for base-currency holdings, the FX rate for foreign ones, and
/// <= 0 when the rate is not yet resolved (converted at 1.0 for display but
/// counted in fx_unresolved_count so the caller skips the snapshot). An empty
/// rate_lookup means "no conversion" (every rate 1.0) — used by callers, e.g.
/// the broker path, that don't do FX. All reported values (price, cost, market
/// value, P&L, day change) are in the base currency.
///
/// Does NOT set `summary.last_updated`, touch the DB, cache, or emit signals —
/// the caller owns those side effects.
inline BuiltSummary build_summary(const QVector<PortfolioAsset>& assets,
                                  const Portfolio& portfolio,
                                  const QHash<QString, services::QuoteData>& quote_map,
                                  const std::function<QString(const QString&)>& sector_lookup,
                                  const std::function<double(const QString&)>& rate_lookup = {}) {
    BuiltSummary out;
    PortfolioSummary& summary = out.summary;
    summary.portfolio = portfolio;
    summary.holdings.reserve(assets.size());

    double total_mv = 0;
    double total_cost = 0;
    double total_day = 0;
    double total_prev = 0; // previous-close value of PRICED holdings only (day% base)

    for (const auto& asset : assets) {
        // FX rate: native listing currency -> portfolio base currency. <= 0 means
        // "not resolved yet" — convert at 1.0 for display but block the snapshot.
        const double raw_rate = rate_lookup ? rate_lookup(asset.symbol) : 1.0;
        const double rate = raw_rate > 0.0 ? raw_rate : 1.0;
        if (raw_rate <= 0.0)
            ++out.fx_unresolved_count;

        HoldingWithQuote h;
        h.symbol = asset.symbol;
        h.quantity = asset.quantity;
        h.avg_buy_price = asset.avg_buy_price * rate; // base currency
        h.cost_basis = h.quantity * h.avg_buy_price;
        h.sector = asset.sector.isEmpty() ? sector_lookup(asset.symbol) : asset.sector;

        auto it = quote_map.find(asset.symbol);
        if (it != quote_map.end()) {
            h.current_price = it->price * rate;   // base currency
            h.day_change = it->change * rate;     // base currency
            h.day_change_percent = it->change_pct; // ratio — unaffected by FX
            h.priced = true;
            total_prev += (h.current_price - h.day_change) * h.quantity; // priced holdings only
        } else {
            // No quote (broker missed the symbol, or yfinance returned nothing):
            // fall back to the (already-converted) avg buy price but mark it
            // unpriced so the mark is not mistaken for a real flat position and
            // NAV snapshots skip it.
            h.current_price = h.avg_buy_price;
            h.priced = false;
            ++out.unpriced_count;
        }

        h.market_value = h.quantity * h.current_price;
        h.unrealized_pnl = h.market_value - h.cost_basis;
        h.unrealized_pnl_percent = (h.cost_basis > 0) ? (h.unrealized_pnl / h.cost_basis) * 100.0 : 0;

        total_mv += h.market_value;
        total_cost += h.cost_basis;
        total_day += h.day_change * h.quantity;

        if (h.unrealized_pnl >= 0)
            summary.gainers++;
        else
            summary.losers++;

        summary.holdings.append(h);
    }

    // Compute weights
    for (auto& h : summary.holdings)
        h.weight = (total_mv > 0) ? (h.market_value / total_mv) * 100.0 : 0;

    summary.total_market_value = total_mv;
    summary.total_cost_basis = total_cost;
    summary.total_unrealized_pnl = total_mv - total_cost;
    summary.total_unrealized_pnl_percent = (total_cost > 0) ? ((total_mv - total_cost) / total_cost) * 100.0 : 0;
    summary.total_day_change = total_day;
    // Percent off the previous-close base of PRICED holdings only. Using
    // (total_mv − total_day) folded unpriced holdings' full stale value into the
    // denominator, diluting the day % whenever any symbol failed to quote.
    summary.total_day_change_percent = (total_prev > 0) ? (total_day / total_prev) * 100.0 : 0;
    summary.total_positions = static_cast<int>(assets.size());
    return out;
}

} // namespace openmarketterminal::portfolio
