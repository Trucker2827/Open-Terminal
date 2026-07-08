// src/services/portfolio/PortfolioSummaryBuild.h
#pragma once
// Pure portfolio-summary construction, split out of
// PortfolioService::finalize_summary so the per-holding pricing + aggregation
// (and, critically, the "was every holding priced?" decision that gates NAV
// snapshot persistence) can be unit-tested without standing up the service
// (DB + market data + SectorResolver singleton + signals).
//
// The math is reproduced verbatim from the inline code it replaces; the only
// additions are the per-holding `priced` flag and the `unpriced_count` tally.

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
};

/// Build a PortfolioSummary from stored @p assets and a live @p quote_map keyed
/// by the asset's canonical `symbol`. @p sector_lookup supplies the fallback
/// sector for assets that have none (a callback so this stays free of the
/// SectorResolver singleton). Does NOT set `summary.last_updated`, touch the DB,
/// cache, or emit signals — the caller owns those side effects.
inline BuiltSummary build_summary(const QVector<PortfolioAsset>& assets,
                                  const Portfolio& portfolio,
                                  const QHash<QString, services::QuoteData>& quote_map,
                                  const std::function<QString(const QString&)>& sector_lookup) {
    BuiltSummary out;
    PortfolioSummary& summary = out.summary;
    summary.portfolio = portfolio;
    summary.holdings.reserve(assets.size());

    double total_mv = 0;
    double total_cost = 0;
    double total_day = 0;
    double total_prev = 0; // previous-close value of PRICED holdings only (day% base)

    for (const auto& asset : assets) {
        HoldingWithQuote h;
        h.symbol = asset.symbol;
        h.quantity = asset.quantity;
        h.avg_buy_price = asset.avg_buy_price;
        h.cost_basis = asset.quantity * asset.avg_buy_price;
        h.sector = asset.sector.isEmpty() ? sector_lookup(asset.symbol) : asset.sector;

        auto it = quote_map.find(asset.symbol);
        if (it != quote_map.end()) {
            h.current_price = it->price;
            h.day_change = it->change;
            h.day_change_percent = it->change_pct;
            h.priced = true;
            total_prev += (h.current_price - h.day_change) * h.quantity; // priced holdings only
        } else {
            // No quote (broker missed the symbol, or yfinance returned nothing):
            // fall back to avg buy price but mark it unpriced so the mark is not
            // mistaken for a real flat position and NAV snapshots skip it.
            h.current_price = asset.avg_buy_price;
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
