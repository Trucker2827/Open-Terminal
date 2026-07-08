// src/services/portfolio/AccountSyncTypes.h
//
// Normalized types + a PURE diff function for mirroring a connected broker /
// exchange account into a portfolio's holdings. "Mirror" means the portfolio
// ends up looking exactly like the fetched account: symbols absent from the
// fetch are removed, even on a successful EMPTY fetch (the account is flat).
// A FAILED fetch (FetchResult::ok == false) must NOT be passed through
// reconcile_mirror at all — callers keep the old holdings in that case; see
// FetchResult::ok doc below.
#pragma once
#include "screens/portfolio/PortfolioTypes.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <qglobal.h> // qAbs

namespace openmarketterminal::portfolio {

struct SyncedHolding {
    QString canonical_symbol; // yfinance format: "AAPL", "BTC-USD", or "$CASH:USD"
    double quantity = 0;
    double avg_cost = 0;
    bool has_cost_basis = true;
    QString native_currency; // ISO code, e.g. "USD","CAD","EUR"
    QString broker_symbol;   // native ticker / ccxt pair (e.g. "AAPL","BTC/USD")
    QString exchange;        // exchange code / ccxt id
};

struct FetchResult {         // an account source returns this
    bool ok = false;         // false => fetch failed; DO NOT mirror (keep old holdings)
    QString error;
    QVector<SyncedHolding> holdings;
};

struct MirrorAction {
    enum Kind { Add, Update, Remove } kind;
    SyncedHolding holding;
    QString symbol;
};

struct MirrorPlan {
    QVector<SyncedHolding> to_add;
    QVector<SyncedHolding> to_update;
    QStringList to_remove;
};

// Pure: compare current assets (from get_assets) to fetched holdings, keyed by
// canonical symbol. Returns the add/update/remove plan. Update = symbol present
// in both AND (quantity or avg_cost or has_cost_basis differs, doubles compared
// with a 1e-9 tolerance). Remove = current symbol absent from the fetched set
// (including the empty-fetch case — that IS the mirror semantics). Add =
// fetched symbol absent from current.
inline MirrorPlan reconcile_mirror(const QVector<PortfolioAsset>& current,
                                   const QVector<SyncedHolding>& fetched) {
    MirrorPlan plan;
    QHash<QString, const PortfolioAsset*> cur;
    for (const auto& a : current) cur.insert(a.symbol, &a);
    QSet<QString> seen;
    for (const auto& h : fetched) {
        seen.insert(h.canonical_symbol);
        auto it = cur.find(h.canonical_symbol);
        if (it == cur.end()) {
            plan.to_add.append(h);
            continue;
        }
        const auto* a = it.value();
        const bool changed = qAbs(a->quantity - h.quantity) > 1e-9 ||
                              qAbs(a->avg_buy_price - h.avg_cost) > 1e-9 ||
                              a->has_cost_basis != h.has_cost_basis;
        if (changed) plan.to_update.append(h);
    }
    for (const auto& a : current)
        if (!seen.contains(a.symbol)) plan.to_remove.append(a.symbol);
    return plan;
}

} // namespace openmarketterminal::portfolio
