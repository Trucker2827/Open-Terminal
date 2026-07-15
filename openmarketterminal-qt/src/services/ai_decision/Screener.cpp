#include "services/ai_decision/Screener.h"

#include "services/ai_decision/DecisionContext.h"
#include "storage/sqlite/Database.h"

#include <QJsonObject>
#include <QPair>
#include <QSet>
#include <QSqlQuery>
#include <algorithm>

namespace openmarketterminal::ai_decision {

// ---------------------------------------------------------------------------
// LOCKSTEP INVARIANT (binding): market_for_venue() (venue -> market, applied
// per-row in C++) and market_venue_filter() (market -> SQL predicate over the
// `venue` column, applied in the universe query) MUST classify identically.
// A venue that market_for_venue() maps to "crypto" MUST be matched by
// market_venue_filter("crypto")'s LIKE predicate, and vice versa — otherwise
// `ai screen --market crypto` and the all-markets path disagree about which
// venues are crypto, silently dropping or mis-tagging candidates.
//
// The mapping is PATTERN-based (prefix/exact), not a fixed venue enumeration:
// the journal writer (crypto_fee_venue_key() in CommandDispatch.cpp) emits a
// large, open-ended venue vocabulary — coinbase_advanced/base, coinbase_tier1
// ..9, coinbase_tier_1..9, coinbase_<band>_plus fee bands, alpaca_crypto,
// binance/binanceus, kraken*, etc. A hardcoded IN-list would silently exclude
// every non-enumerated fee tier. When you change one function's rules, change
// the other's to match.
// ---------------------------------------------------------------------------

QString market_venue_filter(const QString& market) {
    if (market.isEmpty())
        return QString(); // no filter — all venues
    if (market == QStringLiteral("prediction"))
        return QStringLiteral(" AND (venue LIKE 'kalshi%' OR venue LIKE 'polymarket%')");
    if (market == QStringLiteral("equity"))
        return QStringLiteral(" AND (venue = 'alpaca' OR venue = 'chronos2_equity')");
    if (market == QStringLiteral("crypto"))
        return QStringLiteral(
            " AND (venue LIKE 'coinbase%' OR venue LIKE 'kraken%' OR venue LIKE 'binance%'"
            " OR venue = 'alpaca_crypto' OR (venue LIKE 'chronos2%' AND venue <> 'chronos2_equity'))");
    // Unrecognized, non-empty market: match nothing rather than silently
    // falling through to "all venues" (see file header).
    return QStringLiteral(" AND 0");
}

QString market_for_venue(const QString& venue) {
    // ORDER MATTERS: check the equity exact-matches (chronos2_equity, alpaca)
    // BEFORE the crypto chronos2*/alpaca_crypto prefix rules, or chronos2_equity
    // would fall into crypto via the "chronos2" prefix and alpaca into crypto is
    // avoided (alpaca crypto is the distinct venue "alpaca_crypto").
    if (venue.startsWith(QStringLiteral("kalshi")) || venue.startsWith(QStringLiteral("polymarket")))
        return QStringLiteral("prediction");
    if (venue == QStringLiteral("alpaca") || venue == QStringLiteral("chronos2_equity"))
        return QStringLiteral("equity");
    if (venue.startsWith(QStringLiteral("coinbase")) || venue.startsWith(QStringLiteral("kraken"))
        || venue.startsWith(QStringLiteral("binance")) || venue == QStringLiteral("alpaca_crypto")
        || (venue.startsWith(QStringLiteral("chronos2")) && venue != QStringLiteral("chronos2_equity")))
        return QStringLiteral("crypto");
    return QString(); // unclassifiable
}

QVector<ScreenRow> screen(const QString& market, int limit) {
    QVector<ScreenRow> out;

    const QString filter = market_venue_filter(market);
    const QString sql = QStringLiteral(
        "SELECT DISTINCT symbol, venue FROM edge_decision_journal WHERE created_at >= "
        "(SELECT COALESCE(MAX(created_at),0) FROM edge_decision_journal WHERE 1=1%1) - 86400000%1")
        .arg(filter);

    auto rows = openmarketterminal::Database::instance().execute(sql);
    if (rows.is_err())
        return out;

    // Drain the universe query into a local vector before issuing any
    // further queries (assess() below runs its own SELECT per symbol) —
    // keeps the two query lifetimes from overlapping.
    QVector<QPair<QString, QString>> universe;
    {
        QSqlQuery& q = rows.value();
        while (q.next())
            universe.append({q.value(0).toString(), q.value(1).toString()});
    }

    // Dedup by (symbol, market): the universe is DISTINCT (symbol, venue),
    // but assess() keys on symbol alone, so a symbol logged under multiple
    // venues that map to the SAME market (e.g. BTC-USD on coinbase_advanced
    // AND coinbase AND coinbase_tier3, all "crypto") would otherwise be
    // screened once per venue and appear multiple times in the shortlist,
    // burning limit slots with duplicates. Compute row_market first and skip
    // BEFORE assess() so redundant per-symbol SELECTs are avoided too.
    QSet<QString> seen;
    for (const auto& symbol_venue : universe) {
        const QString& symbol = symbol_venue.first;
        const QString& venue = symbol_venue.second;
        const QString row_market = market_for_venue(venue);

        // Drop unclassifiable venues: a shortlisted row must never carry an
        // empty market tag. In the all-markets path (no venue filter), the
        // universe can surface venues the mapping doesn't recognize; skip them
        // so screen("") only ever emits classified rows.
        if (row_market.isEmpty())
            continue;

        const QString key = symbol + QStringLiteral("|") + row_market;
        if (seen.contains(key))
            continue;
        seen.insert(key);

        DecisionPacket p = assess(symbol, row_market);
        if (p.recommendation_hint != QStringLiteral("all gates pass"))
            continue;

        ScreenRow row;
        row.symbol = symbol;
        row.market = row_market;
        row.edge_after_cost = p.edge_after_cost;
        row.side = p.side;
        row.horizon = p.horizon;
        row.freshness = p.freshness;
        row.recommendation_hint = p.recommendation_hint;
        out.append(row);
    }

    std::sort(out.begin(), out.end(), [](const ScreenRow& a, const ScreenRow& b) {
        return a.edge_after_cost > b.edge_after_cost;
    });

    const int effective_limit = limit <= 0 ? 5 : limit;
    if (out.size() > effective_limit)
        out.resize(effective_limit);

    return out;
}

QJsonArray screen_to_json(const QVector<ScreenRow>& rows) {
    QJsonArray arr;
    for (const auto& row : rows) {
        QJsonObject obj;
        obj[QStringLiteral("symbol")] = row.symbol;
        obj[QStringLiteral("market")] = row.market;
        obj[QStringLiteral("edge_after_cost")] = row.edge_after_cost;
        obj[QStringLiteral("side")] = row.side;
        obj[QStringLiteral("horizon")] = row.horizon;
        obj[QStringLiteral("freshness")] = row.freshness;
        obj[QStringLiteral("recommendation_hint")] = row.recommendation_hint;
        arr.append(obj);
    }
    return arr;
}

} // namespace openmarketterminal::ai_decision
