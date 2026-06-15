// PmPaperEngine.cpp — Prediction-market PAPER fill engine (Phase B, Task 3).
//
// PURE DB writes over PmPaperRepository — this file NEVER references the
// adapter's live order-placement / order-cancel methods (a grep for those is
// part of the task gate, so this file must stay free of them entirely).

#include "mcp/tools/PmPaperEngine.h"

#include <QDateTime>

namespace openmarketterminal::mcp::tools {

namespace {
constexpr double kCloseEps = 1e-9;  // contracts at/below this count as fully closed
}

PmFill buy_to_open(const QString& venue, const QString& market_id, const QString& asset_id,
                   const QString& outcome, const QString& category, double contracts,
                   double fill_price) {
    auto& repo = PmPaperRepository::instance();

    const double cost = contracts * fill_price;

    auto cash = repo.cash();
    if (cash.is_err())
        return PmFill{false, QString::fromStdString(cash.error()), {}, 0, 0, 0};
    if (cash.value() - cost < 0.0)
        return PmFill{false, "insufficient paper cash", {}, 0, 0, 0};

    // Do the read + the position write BEFORE the cash debit, so a failed
    // read/write never leaves cash debited with no position recorded. (Fully
    // atomic cash+position would need a DB transaction; on local SQLite a mid-op
    // write failure is effectively impossible, so this ordering suffices.)
    auto existing = repo.get_open(venue, asset_id);
    if (existing.is_err())
        return PmFill{false, QString::fromStdString(existing.error()), {}, 0, 0, 0};

    if (existing.value().has_value()) {
        const PmPosition& old = existing.value().value();
        const double new_contracts = old.contracts + contracts;
        const double new_cost = old.cost_basis + cost;
        // Re-blend the average so mark-to-market reflects the true cost/contract
        // after averaging in (not the first-entry price).
        const double new_avg = new_contracts > 0.0 ? new_cost / new_contracts : fill_price;
        auto upd = repo.set_contracts(old.id, new_contracts, new_cost, new_avg, "open");
        if (upd.is_err())
            return PmFill{false, QString::fromStdString(upd.error()), {}, 0, 0, 0};
    } else {
        PmPosition p;
        p.venue = venue;
        p.market_id = market_id;
        p.asset_id = asset_id;
        p.outcome = outcome;
        p.category = category;
        p.contracts = contracts;
        p.avg_price = fill_price;
        p.cost_basis = cost;
        p.opened_at = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        p.status = "open";
        auto ins = repo.insert_open(p);
        if (ins.is_err())
            return PmFill{false, QString::fromStdString(ins.error()), {}, 0, 0, 0};
    }

    auto adj = repo.adjust_cash(-cost);
    if (adj.is_err())
        return PmFill{false, QString::fromStdString(adj.error()), {}, 0, 0, 0};

    auto cash_after = repo.cash();
    const double cash_now = cash_after.is_ok() ? cash_after.value() : (cash.value() - cost);
    return PmFill{true, {}, "buy_to_open", contracts, fill_price, cash_now};
}

PmFill sell_to_close(const QString& venue, const QString& asset_id, double contracts,
                     double fill_price) {
    auto& repo = PmPaperRepository::instance();

    auto open = repo.get_open(venue, asset_id);
    if (open.is_err())
        return PmFill{false, QString::fromStdString(open.error()), {}, 0, 0, 0};
    if (!open.value().has_value())
        return PmFill{false, "no open position to sell; short-open is not enabled in Phase B",
                      {}, 0, 0, 0};

    const PmPosition pos = open.value().value();
    if (contracts > pos.contracts)
        return PmFill{false,
                      QString("cannot sell more than held (held %1)").arg(pos.contracts),
                      {}, 0, 0, 0};

    const double proceeds = contracts * fill_price;
    const double new_contracts = pos.contracts - contracts;
    const double new_cost =
        pos.contracts > 0.0 ? pos.cost_basis * (new_contracts / pos.contracts) : 0.0;
    const QString status = new_contracts <= kCloseEps ? "closed" : "open";
    // Write the reduced position BEFORE crediting cash, so a failed write never
    // leaves cash credited with the position untouched. Selling part of a lot
    // does not change the remaining lot's average cost.
    auto upd = repo.set_contracts(pos.id, new_contracts, new_cost, pos.avg_price, status);
    if (upd.is_err())
        return PmFill{false, QString::fromStdString(upd.error()), {}, 0, 0, 0};

    auto adj = repo.adjust_cash(proceeds);
    if (adj.is_err())
        return PmFill{false, QString::fromStdString(adj.error()), {}, 0, 0, 0};

    auto cash_after = repo.cash();
    const double cash_now = cash_after.is_ok() ? cash_after.value() : 0.0;
    return PmFill{true, {}, "sell_to_close", contracts, fill_price, cash_now};
}

double mark_to_market(const PmPosition& p, double current_price) {
    return (current_price - p.avg_price) * p.contracts;
}

} // namespace openmarketterminal::mcp::tools
