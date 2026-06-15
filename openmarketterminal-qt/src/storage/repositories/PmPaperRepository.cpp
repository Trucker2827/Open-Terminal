#include "storage/repositories/PmPaperRepository.h"

namespace openmarketterminal {

PmPaperRepository& PmPaperRepository::instance() {
    static PmPaperRepository s;
    return s;
}

static const char* kPositionColumns =
    "id, venue, market_id, asset_id, outcome, category, contracts, avg_price, cost_basis, opened_at, status";

PmPosition PmPaperRepository::map_position(QSqlQuery& q) {
    PmPosition p;
    p.id = q.value(0).toLongLong();
    p.venue = q.value(1).toString();
    p.market_id = q.value(2).toString();
    p.asset_id = q.value(3).toString();
    p.outcome = q.value(4).toString();
    p.category = q.value(5).toString();
    p.contracts = q.value(6).toDouble();
    p.avg_price = q.value(7).toDouble();
    p.cost_basis = q.value(8).toDouble();
    p.opened_at = q.value(9).toString();
    p.status = q.value(10).toString();
    return p;
}

Result<double> PmPaperRepository::cash() {
    auto r = db().execute("SELECT cash FROM pm_paper_account WHERE id = 1", {});
    if (r.is_err())
        return Result<double>::err(r.error());
    if (r.value().next())
        return Result<double>::ok(r.value().value(0).toDouble());

    // No account row yet — seed the default and return it.
    auto ins = exec_write("INSERT INTO pm_paper_account (id, cash) VALUES (1, 100000)");
    if (ins.is_err())
        return Result<double>::err(ins.error());
    return Result<double>::ok(100000.0);
}

Result<void> PmPaperRepository::adjust_cash(double delta) {
    // Ensure the id=1 row exists (seeds default on first use).
    auto c = cash();
    if (c.is_err())
        return Result<void>::err(c.error());
    return exec_write("UPDATE pm_paper_account SET cash = cash + ? WHERE id = 1", {delta});
}

Result<std::optional<PmPosition>> PmPaperRepository::get_open(const QString& venue, const QString& asset_id) {
    auto r = db().execute(
        QString("SELECT %1 FROM pm_paper_positions WHERE venue = ? AND asset_id = ? AND status = 'open' LIMIT 1")
            .arg(kPositionColumns),
        {venue, asset_id});
    if (r.is_err())
        return Result<std::optional<PmPosition>>::err(r.error());
    auto& q = r.value();
    if (!q.next())
        return Result<std::optional<PmPosition>>::ok(std::nullopt);
    return Result<std::optional<PmPosition>>::ok(map_position(q));
}

Result<qint64> PmPaperRepository::insert_open(const PmPosition& p) {
    return exec_insert(
        "INSERT INTO pm_paper_positions "
        "(venue, market_id, asset_id, outcome, category, contracts, avg_price, cost_basis, opened_at, status) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        {p.venue, p.market_id, p.asset_id, p.outcome, p.category, p.contracts, p.avg_price, p.cost_basis,
         p.opened_at, p.status});
}

Result<void> PmPaperRepository::set_contracts(qint64 id, double contracts, double cost_basis,
                                              const QString& status) {
    return exec_write("UPDATE pm_paper_positions SET contracts = ?, cost_basis = ?, status = ? WHERE id = ?",
                      {contracts, cost_basis, status, id});
}

Result<QVector<PmPosition>> PmPaperRepository::list_open() {
    return query_list(QString("SELECT %1 FROM pm_paper_positions WHERE status = 'open'").arg(kPositionColumns), {},
                      map_position);
}

Result<double> PmPaperRepository::open_stake_in_category(const QString& category) {
    auto r = db().execute(
        "SELECT COALESCE(SUM(cost_basis), 0) FROM pm_paper_positions WHERE status = 'open' AND category = ?",
        {category});
    if (r.is_err())
        return Result<double>::err(r.error());
    if (!r.value().next())
        return Result<double>::ok(0.0);
    return Result<double>::ok(r.value().value(0).toDouble());
}

} // namespace openmarketterminal
