// src/services/relationship_map/RelmapPeerMerge.h
// Pure mapping from the native EquityResearchService peer feed (PeerData
// ratio rows) to the relationship map's PeerCompany entries (issue #83).
// Header-only so the contract is unit-testable without service bring-up.
#pragma once

#include "screens/relationship_map/RelationshipMapTypes.h"
#include "services/equity/EquityResearchModels.h"

#include <QVector>

namespace openmarketterminal::relmap {

/// Map the native ratios feed to map peers, dropping the company's own row
/// (fetch_peers prepends the subject symbol for side-by-side comparison).
/// Fields the ratios feed does not carry (name, market cap, price, beta,
/// EV/EBITDA, 52-week change, recommendation, sector, revenue growth) stay
/// at their zero/empty defaults — honestly missing, never backfilled.
inline QVector<PeerCompany> merge_peer_data(
    const QString& self_ticker,
    const QVector<openmarketterminal::services::equity::PeerData>& rows) {
    QVector<PeerCompany> out;
    for (const auto& r : rows) {
        if (r.symbol.trimmed().isEmpty())
            continue;
        if (r.symbol.compare(self_ticker, Qt::CaseInsensitive) == 0)
            continue;
        PeerCompany p;
        p.ticker         = r.symbol.toUpper();
        p.pe_ratio       = r.pe_ratio;
        p.forward_pe     = r.forward_pe;
        p.price_to_book  = r.price_to_book;
        p.roe            = r.roe;
        p.profit_margins = r.profit_margin;
        p.gross_margins  = r.gross_margin;
        out.append(p);
    }
    return out;
}

} // namespace openmarketterminal::relmap
