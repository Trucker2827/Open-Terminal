#include "services/ai_decision/Screener.h"

#include "services/ai_decision/DecisionContext.h"
#include "storage/sqlite/Database.h"

#include <QJsonObject>
#include <QPair>
#include <QSet>
#include <QSqlQuery>
#include <algorithm>

namespace openmarketterminal::ai_decision {

namespace {

// Single source of truth for the market<->venue mapping, shared by
// market_venue_filter() (market -> venue IN (...) fragment) and
// market_for_venue() (venue -> market, reverse lookup). Kept in this
// anonymous-namespace function (rather than a file-scope static) so a
// same-named helper in a unity batch-mate cannot collide with it — see
// DecisionContext.cpp's EdgeCol note; this TU is excluded from the unity
// batch in CMakeLists.txt for the same defensive reason.
const QVector<QPair<QString, QStringList>>& market_venue_table() {
    static const QVector<QPair<QString, QStringList>> table = {
        {QStringLiteral("prediction"), {QStringLiteral("kalshi"), QStringLiteral("polymarket")}},
        {QStringLiteral("equity"), {QStringLiteral("chronos2_equity"), QStringLiteral("alpaca")}},
        {QStringLiteral("crypto"),
         {QStringLiteral("coinbase_advanced"), QStringLiteral("coinbase"), QStringLiteral("kraken_pro"),
          QStringLiteral("coinbase_perps"), QStringLiteral("chronos2"), QStringLiteral("coinbase_tier3")}},
    };
    return table;
}

} // namespace

QString market_venue_filter(const QString& market) {
    if (market.isEmpty())
        return QString();
    for (const auto& entry : market_venue_table()) {
        if (entry.first == market) {
            QStringList quoted;
            quoted.reserve(entry.second.size());
            for (const auto& venue : entry.second)
                quoted << QStringLiteral("'%1'").arg(venue);
            return QStringLiteral(" AND venue IN (%1)").arg(quoted.join(QStringLiteral(",")));
        }
    }
    // Unrecognized, non-empty market: match nothing rather than silently
    // falling through to "all venues" (see file header).
    return QStringLiteral(" AND 0");
}

QString market_for_venue(const QString& venue) {
    for (const auto& entry : market_venue_table()) {
        if (entry.second.contains(venue))
            return entry.first;
    }
    return QString();
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
