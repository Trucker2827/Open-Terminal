#pragma once
#include "storage/repositories/BaseRepository.h"

#include <QPair>

namespace openmarketterminal {

struct AiFill {
    QString id;
    QString handler;
    QString symbol;
    QString side;
    double quantity = 0.0;
    double fill_price = 0.0;
    double fee = 0.0;
    double realized_pnl = 0.0;
    qint64 ts = 0;
    QString draft_id;
};

/// Append-only store of the AI trade-handler's paper fills.
class AiFillRepository : public BaseRepository<AiFill> {
  public:
    static AiFillRepository& instance();

    Result<void> append(const AiFill& f);
    /// Chronological (ts ASC) — for folding into a position.
    Result<QVector<AiFill>> fills_for(const QString& handler, const QString& symbol);
    /// Recent first (ts DESC). Empty handler/symbol = no filter on that column; limit <= 0 = no LIMIT.
    Result<QVector<AiFill>> list(const QString& handler, const QString& symbol, int limit);
    /// Distinct (handler, symbol) pairs. Empty handler = across all handlers.
    Result<QVector<QPair<QString, QString>>> distinct_handler_symbols(const QString& handler);

  private:
    AiFillRepository() = default;
    static AiFill map_row(QSqlQuery& q);
};

} // namespace openmarketterminal
