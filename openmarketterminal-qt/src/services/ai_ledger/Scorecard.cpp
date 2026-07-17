#include "services/ai_ledger/Scorecard.h"

#include "storage/repositories/AiFillRepository.h"

#include <QJsonArray>
#include <QMap>
#include <algorithm>

namespace openmarketterminal {
namespace ai_ledger {

Scorecard scorecard_of(const QString& handler, const QString& symbol, int limit) {
    Scorecard sc;
    sc.handler = handler;
    sc.symbol = symbol;

    auto rows = AiFillRepository::instance().list(handler, symbol, 0);  // all, recent-first
    if (rows.is_err())
        return sc;  // all-zero on read error

    // Keep only realized closes (recent-first order preserved).
    QVector<AiFill> closes;
    for (const AiFill& f : rows.value())
        if (f.realized_pnl != 0.0)
            closes.append(f);

    // Window to the most-recent `limit` closes.
    if (limit > 0 && closes.size() > limit)
        closes.resize(limit);

    if (closes.isEmpty())
        return sc;

    // Per-symbol accumulation (only used when symbol filter is empty).
    QMap<QString, SymbolScore> by_symbol;

    for (const AiFill& f : closes) {
        const double pnl = f.realized_pnl;
        ++sc.trades;
        sc.realized_total += pnl;
        if (pnl > 0.0) ++sc.wins;
        else if (pnl < 0.0) ++sc.losses;
        if (sc.trades == 1) {
            sc.best = pnl;
            sc.worst = pnl;
        } else {
            sc.best = std::max(sc.best, pnl);
            sc.worst = std::min(sc.worst, pnl);
        }
        if (symbol.isEmpty()) {
            SymbolScore& ss = by_symbol[f.symbol];
            ss.symbol = f.symbol;
            ++ss.trades;
            ss.realized_total += pnl;
            if (pnl > 0.0) ++ss.wins;
            else if (pnl < 0.0) ++ss.losses;
        }
    }

    sc.hit_rate = sc.trades ? static_cast<double>(sc.wins) / sc.trades : 0.0;
    sc.avg_realized = sc.trades ? sc.realized_total / sc.trades : 0.0;

    if (symbol.isEmpty()) {
        for (SymbolScore ss : by_symbol) {
            ss.hit_rate = ss.trades ? static_cast<double>(ss.wins) / ss.trades : 0.0;
            sc.per_symbol.append(ss);
        }
        std::sort(sc.per_symbol.begin(), sc.per_symbol.end(),
                  [](const SymbolScore& a, const SymbolScore& b) {
                      return a.realized_total > b.realized_total;
                  });
    }
    return sc;
}

QJsonObject scorecard_to_json(const Scorecard& s) {
    QJsonArray per;
    for (const SymbolScore& ss : s.per_symbol) {
        per.append(QJsonObject{{"symbol", ss.symbol},
                               {"trades", ss.trades},
                               {"wins", ss.wins},
                               {"losses", ss.losses},
                               {"hit_rate", ss.hit_rate},
                               {"realized_total", ss.realized_total}});
    }
    return QJsonObject{{"handler", s.handler},
                       {"symbol", s.symbol},
                       {"trades", s.trades},
                       {"wins", s.wins},
                       {"losses", s.losses},
                       {"hit_rate", s.hit_rate},
                       {"realized_total", s.realized_total},
                       {"avg_realized", s.avg_realized},
                       {"best", s.best},
                       {"worst", s.worst},
                       {"per_symbol", per}};
}

} // namespace ai_ledger
} // namespace openmarketterminal
