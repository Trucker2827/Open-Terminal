#include "storage/repositories/AiFillRepository.h"

namespace openmarketterminal {

AiFillRepository& AiFillRepository::instance() {
    static AiFillRepository s;
    return s;
}

AiFill AiFillRepository::map_row(QSqlQuery& q) {
    return {q.value(0).toString(),  q.value(1).toString(),  q.value(2).toString(), q.value(3).toString(),
            q.value(4).toDouble(),  q.value(5).toDouble(),  q.value(6).toDouble(), q.value(7).toDouble(),
            q.value(8).toLongLong(), q.value(9).toString()};
}

Result<void> AiFillRepository::append(const AiFill& f) {
    return exec_write(
        "INSERT INTO ai_fill (id, handler, symbol, side, quantity, fill_price, fee, realized_pnl, ts, draft_id) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        {f.id, f.handler, f.symbol, f.side, f.quantity, f.fill_price, f.fee, f.realized_pnl,
         static_cast<qlonglong>(f.ts), f.draft_id});
}

Result<QVector<AiFill>> AiFillRepository::fills_for(const QString& handler, const QString& symbol) {
    return query_list_as<AiFill>(
        "SELECT id, handler, symbol, side, quantity, fill_price, fee, realized_pnl, ts, draft_id "
        "FROM ai_fill WHERE handler = ? AND symbol = ? ORDER BY ts ASC, id ASC",
        {handler, symbol}, map_row);
}

Result<QVector<AiFill>> AiFillRepository::list(const QString& handler, const QString& symbol, int limit) {
    QString sql =
        "SELECT id, handler, symbol, side, quantity, fill_price, fee, realized_pnl, ts, draft_id FROM ai_fill";
    QVariantList params;
    QStringList clauses;
    if (!handler.isEmpty()) { clauses << "handler = ?"; params << handler; }
    if (!symbol.isEmpty())  { clauses << "symbol = ?";  params << symbol; }
    if (!clauses.isEmpty())
        sql += " WHERE " + clauses.join(" AND ");
    sql += " ORDER BY ts DESC, id DESC";
    if (limit > 0) {
        sql += " LIMIT ?";
        params << limit;
    }
    return query_list_as<AiFill>(sql, params, map_row);
}

Result<QVector<QPair<QString, QString>>> AiFillRepository::distinct_handler_symbols(const QString& handler) {
    QString sql = "SELECT DISTINCT handler, symbol FROM ai_fill";
    QVariantList params;
    if (!handler.isEmpty()) {
        sql += " WHERE handler = ?";
        params << handler;
    }
    sql += " ORDER BY handler, symbol";
    return query_list_as<QPair<QString, QString>>(
        sql, params, [](QSqlQuery& q) { return qMakePair(q.value(0).toString(), q.value(1).toString()); });
}

} // namespace openmarketterminal
