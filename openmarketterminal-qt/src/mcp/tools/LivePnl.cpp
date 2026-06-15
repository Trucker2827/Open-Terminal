// LivePnl.cpp — live realized-P&L ledger helpers + the deterministic daily-loss
// gate (Phase C, Task 2). Paper-safe: this builds the daily-loss machinery that
// later LIVE tasks call before executing — it does NOT itself place orders.
//
// SCOPE (honest): accurate live realized P&L would need per-order broker fill
// reconciliation (a big subsystem, deferred). This MVP tracks realized P&L from
// the substrate's OWN fills at the price the substrate executed at (resolved /
// submitted price + tracked cost basis) — exact for sandbox/paper,
// resolved-price-approximate for real equity fills. Deterministic + conservative,
// and AI-uncontrolled (the cap is the GUI-only cli.risk.max_daily_loss).
//
// P&L-ONLY: this ledger carries NO cash account — it is distinct from the PM
// paper engine's cash book and never touches pm_paper_* tables.

#include "mcp/tools/LivePnl.h"

#include "storage/repositories/LivePnlRepository.h"
#include "storage/repositories/SettingsRepository.h"

#include <QDateTime>

#include <cmath>

namespace openmarketterminal::mcp::tools {

namespace {
constexpr double kCloseEps = 1e-9;  // qty at/below this counts as fully closed
}

QString today_utc() {
    return QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate);
}

void record_open(const QString& account, const QString& venue, const QString& instrument,
                 double qty, double fill_price) {
    auto& repo = LivePnlRepository::instance();
    const double cost = qty * fill_price;

    auto existing = repo.get_open(account, venue, instrument);
    if (existing.is_err())
        return;

    if (existing.value().has_value()) {
        const LivePosition& old = existing.value().value();
        const double new_qty = qty + old.qty;
        const double new_cost = old.cost_basis + cost;
        // Re-blend the average so realized P&L on the next close reflects the true
        // cost/unit after averaging in (not the first-entry price).
        const double new_avg = new_qty > 0.0 ? new_cost / new_qty : fill_price;
        repo.set_position(old.id, new_qty, new_avg, new_cost, "open");
    } else {
        LivePosition p;
        p.account = account;
        p.venue = venue;
        p.instrument = instrument;
        p.qty = qty;
        p.avg_cost = fill_price;
        p.cost_basis = cost;
        p.opened_at = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        p.status = "open";
        repo.insert_open(p);
    }
}

double record_close(const QString& account, const QString& venue, const QString& instrument,
                    double qty, double fill_price) {
    auto& repo = LivePnlRepository::instance();

    auto open = repo.get_open(account, venue, instrument);
    if (open.is_err() || !open.value().has_value())
        return 0.0;  // untracked close contributes nothing — conservative

    const LivePosition pos = open.value().value();
    const double closed_qty = qty < pos.qty ? qty : pos.qty;  // min(qty, held)
    const double realized = (fill_price - pos.avg_cost) * closed_qty;

    const double new_qty = pos.qty - closed_qty;
    // Pro-rata the remaining cost basis; selling part of a lot does not change the
    // remaining lot's average cost.
    const double new_cost = pos.qty > 0.0 ? pos.cost_basis * (new_qty / pos.qty) : 0.0;
    const QString status = new_qty <= kCloseEps ? "closed" : "open";
    repo.set_position(pos.id, new_qty, pos.avg_cost, new_cost, status);

    repo.add_realized(today_utc(), realized);
    return realized;
}

bool daily_loss_ok(double prospective_max_loss) {
    // Fail closed on a DB error — never let a trade through when the tally can't
    // be read.
    auto realized_r = LivePnlRepository::instance().realized_today(today_utc());
    if (realized_r.is_err())
        return false;
    const double realized = realized_r.value();
    // Conservative: profits never expand headroom into negative.
    const double today_loss = realized < 0 ? -realized : 0.0;
    // Cap is the GUI-only cli.risk.max_daily_loss (AI cannot write it); a
    // non-positive / non-finite value → finite default, never "no cap".
    auto cap_r = SettingsRepository::instance().get("cli.risk.max_daily_loss", "5000");
    double cap = cap_r.is_ok() ? cap_r.value().toDouble() : 5000.0;
    if (cap <= 0 || !std::isfinite(cap))
        cap = 5000.0;
    return (today_loss + prospective_max_loss) <= cap;
}

} // namespace openmarketterminal::mcp::tools
