// src/screens/portfolio/PortfolioAgentFilter.h
#pragma once
// Pure helpers for filtering the (large, ~54-agent) Agent Runner list down to
// agents relevant to what the active portfolio actually holds. Split out of
// PortfolioInsightsPanel so the classification + match logic is unit-testable.
//
// An agent carries `asset_classes` (from finagent_core's loader): "equity",
// "fx", "crypto", or "*" (cross-asset / general, always shown). A portfolio's
// held classes are inferred from its holding symbols by the yfinance-style
// naming convention.

#include <QSet>
#include <QString>
#include <QStringList>

namespace openmarketterminal::portfolio {

/// Asset class of a single canonical holding symbol.
///   "$CASH:USD" -> "cash"  |  "EURUSD=X" -> "fx"  |  "BTC-USD" -> "crypto"
///   "AAPL" -> "equity"
inline QString symbol_asset_class(const QString& symbol) {
    if (symbol.startsWith(QLatin1String("$CASH:")))
        return QStringLiteral("cash");
    if (symbol.endsWith(QLatin1String("=X")))
        return QStringLiteral("fx");
    // yfinance crypto convention is BASE-USD / BASE-USDT (e.g. BTC-USD); equities
    // are bare tickers (AAPL), so a "-USD" suffix reliably marks crypto.
    if (symbol.endsWith(QLatin1String("-USD")) || symbol.endsWith(QLatin1String("-USDT")))
        return QStringLiteral("crypto");
    return QStringLiteral("equity");
}

/// The set of asset classes a portfolio holds (ignoring cash), inferred from its
/// holding symbols. Empty when there are no (non-cash) holdings — the caller
/// then shows all agents rather than filtering to nothing.
inline QSet<QString> portfolio_asset_classes(const QStringList& symbols) {
    QSet<QString> out;
    for (const auto& s : symbols) {
        const QString c = symbol_asset_class(s);
        if (c != QLatin1String("cash"))
            out.insert(c);
    }
    return out;
}

/// Whether an agent (by its asset_classes) should be shown for a portfolio that
/// holds @p held classes. General agents ("*") and untagged agents always show;
/// when the held set is empty (unknown / cash-only) everything shows.
inline bool agent_matches_holdings(const QStringList& agent_classes, const QSet<QString>& held) {
    if (agent_classes.isEmpty() || held.isEmpty())
        return true;
    for (const auto& ac : agent_classes) {
        if (ac == QLatin1String("*") || held.contains(ac))
            return true;
    }
    return false;
}

} // namespace openmarketterminal::portfolio
