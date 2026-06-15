#include "storage/repositories/LivePnlRepository.h"

#include <QDateTime>

namespace openmarketterminal {

LivePnlRepository& LivePnlRepository::instance() {
    static LivePnlRepository s;
    return s;
}

static const char* kPositionColumns =
    "id, account, venue, instrument, qty, avg_cost, cost_basis, opened_at, status";

LivePosition LivePnlRepository::map_position(QSqlQuery& q) {
    LivePosition p;
    p.id = q.value(0).toLongLong();
    p.account = q.value(1).toString();
    p.venue = q.value(2).toString();
    p.instrument = q.value(3).toString();
    p.qty = q.value(4).toDouble();
    p.avg_cost = q.value(5).toDouble();
    p.cost_basis = q.value(6).toDouble();
    p.opened_at = q.value(7).toString();
    p.status = q.value(8).toString();
    return p;
}

Result<std::optional<LivePosition>> LivePnlRepository::get_open(const QString& account,
                                                               const QString& venue,
                                                               const QString& instrument) {
    auto r = db().execute(
        QString("SELECT %1 FROM live_positions WHERE account = ? AND venue = ? AND instrument = ? "
                "AND status = 'open' LIMIT 1")
            .arg(kPositionColumns),
        {account, venue, instrument});
    if (r.is_err())
        return Result<std::optional<LivePosition>>::err(r.error());
    auto& q = r.value();
    if (!q.next())
        return Result<std::optional<LivePosition>>::ok(std::nullopt);
    return Result<std::optional<LivePosition>>::ok(map_position(q));
}

Result<qint64> LivePnlRepository::insert_open(const LivePosition& p) {
    return exec_insert(
        "INSERT INTO live_positions "
        "(account, venue, instrument, qty, avg_cost, cost_basis, opened_at, status) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        {p.account, p.venue, p.instrument, p.qty, p.avg_cost, p.cost_basis, p.opened_at, p.status});
}

Result<void> LivePnlRepository::set_position(qint64 id, double qty, double avg_cost,
                                             double cost_basis, const QString& status) {
    return exec_write(
        "UPDATE live_positions SET qty = ?, avg_cost = ?, cost_basis = ?, status = ? WHERE id = ?",
        {qty, avg_cost, cost_basis, status, id});
}

Result<double> LivePnlRepository::realized_today(const QString& utc_day) {
    auto r = db().execute("SELECT realized_pnl FROM daily_pnl WHERE utc_day = ?", {utc_day});
    if (r.is_err())
        return Result<double>::err(r.error());
    if (!r.value().next())
        return Result<double>::ok(0.0);
    return Result<double>::ok(r.value().value(0).toDouble());
}

Result<void> LivePnlRepository::add_realized(const QString& utc_day, double delta) {
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    auto existing = realized_today(utc_day);
    if (existing.is_err())
        return Result<void>::err(existing.error());

    auto chk = db().execute("SELECT 1 FROM daily_pnl WHERE utc_day = ?", {utc_day});
    if (chk.is_err())
        return Result<void>::err(chk.error());
    if (chk.value().next()) {
        return exec_write("UPDATE daily_pnl SET realized_pnl = realized_pnl + ?, updated_at = ? "
                          "WHERE utc_day = ?",
                          {delta, now, utc_day});
    }
    return exec_write(
        "INSERT INTO daily_pnl (utc_day, realized_pnl, updated_at) VALUES (?, ?, ?)",
        {utc_day, delta, now});
}

} // namespace openmarketterminal
