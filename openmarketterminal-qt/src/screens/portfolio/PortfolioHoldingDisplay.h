// src/screens/portfolio/PortfolioHoldingDisplay.h
#pragma once
// Pure presentation helper for a portfolio holding row, split out of
// PortfolioBlotter so the "unpriced holding" display can be unit-tested without
// standing up the widget (which pulls in DataHub + sparkline subscriptions).
//
// When a holding has no live quote (priced == false), PortfolioService fell its
// price back to the average buy price, so the price / market value / P&L / day
// change are FABRICATED — market_value == cost_basis, P&L == 0. Rendering those
// as "+0.00%" is indistinguishable from a genuinely flat position, so instead we
// show an em-dash and flag the row muted. Quantity / avg cost / cost basis are
// real and are formatted by the caller.

#include "screens/portfolio/PortfolioTypes.h"

#include <QString>

namespace openmarketterminal::portfolio {

/// Placeholder shown in place of a fabricated numeric cell for an unpriced row.
inline QString unpriced_cell_placeholder() { return QStringLiteral("—"); }

/// Human-readable reason surfaced (e.g. as a tooltip) on an unpriced row.
inline QString unpriced_reason() {
    return QStringLiteral("Unpriced — no live quote; value shown falls back to cost");
}

/// The price-dependent display strings for one holding row. When `muted` is true
/// the caller should render these (and ideally the symbol) in a muted colour and
/// attach unpriced_reason() as a tooltip.
struct HoldingDisplayCells {
    QString last;            ///< LAST price column
    QString market_value;    ///< MKT VAL column
    QString pnl;             ///< P&L column
    QString pnl_pct;         ///< P&L% column
    QString day_change_pct;  ///< CHG% column
    bool muted = false;      ///< true when the holding is unpriced (fabricated marks)
};

/// Format the price-dependent cells for @p h. Numeric formatting mirrors
/// PortfolioBlotter::format_value (fixed-point, `dp` decimals) with a leading
/// sign on the P&L / change columns. An unpriced holding yields em-dashes and
/// muted == true so no fabricated "+0.00%" is ever shown.
inline HoldingDisplayCells price_dependent_cells(const HoldingWithQuote& h, int dp = 2) {
    HoldingDisplayCells c;
    if (!h.priced) {
        const QString dash = unpriced_cell_placeholder();
        c.last = dash;
        c.market_value = dash;
        c.pnl = dash;
        c.pnl_pct = dash;
        c.day_change_pct = dash;
        c.muted = true;
        return c;
    }
    const auto num = [dp](double v) { return QString::number(v, 'f', dp); };
    const auto signed_num = [&num](double v) { return (v >= 0 ? QStringLiteral("+") : QString()) + num(v); };
    c.last = num(h.current_price);
    c.market_value = num(h.market_value);
    c.pnl = signed_num(h.unrealized_pnl);
    c.pnl_pct = signed_num(h.unrealized_pnl_percent) + QStringLiteral("%");
    c.day_change_pct = signed_num(h.day_change_percent) + QStringLiteral("%");
    return c;
}

} // namespace openmarketterminal::portfolio
