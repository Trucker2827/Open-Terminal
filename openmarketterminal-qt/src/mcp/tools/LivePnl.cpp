// LivePnl.cpp — live realized-P&L ledger helpers + the deterministic daily-loss
// gate (Phase C, Task 2). Paper-safe: this builds the daily-loss machinery that
// later LIVE tasks call before executing — it does NOT itself place orders.
//
// SCOPE (honest): this ledger records only quantities the broker confirms as
// filled. It never treats submission acceptance as execution. When an adapter
// confirms fill quantity but omits average price, the resolved/submitted price
// is retained as a valuation fallback; quantity is never fabricated.
//
// P&L-ONLY: this ledger carries NO cash account — it is distinct from the PM
// paper engine's cash book and never touches pm_paper_* tables.

#include "mcp/tools/LivePnl.h"

#include "core/logging/Logger.h"
#include "storage/repositories/LivePnlRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "trading/AccountManager.h"
#include "trading/BrokerInterface.h"

#include <QDateTime>

#include <algorithm>
#include <cmath>

namespace openmarketterminal::mcp::tools {

namespace {
constexpr double kCloseEps = 1e-9;  // qty at/below this counts as fully closed

QString normalize_order_status(QString status) {
    status = status.trimmed().toLower();
    status.replace(QLatin1Char('-'), QLatin1Char('_'));
    status.replace(QLatin1Char(' '), QLatin1Char('_'));
    if (status == QLatin1String("complete") || status == QLatin1String("completed") ||
        status == QLatin1String("executed") || status == QLatin1String("traded"))
        return QStringLiteral("filled");
    if (status == QLatin1String("partial") || status == QLatin1String("partiallyfilled"))
        return QStringLiteral("partially_filled");
    if (status == QLatin1String("cancelled"))
        return QStringLiteral("canceled");
    return status;
}
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

ReconciledFill reconcile_and_record(const QString& account, const QString& venue,
                                    const QString& instrument, trading::OrderSide side,
                                    double qty, double resolved_price, const QString& order_id) {
    using namespace openmarketterminal::trading;

    ReconciledFill out;
    out.reconciled = false;
    out.recorded = false;

    // Best-effort: one-shot broker query for the just-placed order's ACTUAL fill.
    // Every failure mode leaves qty=0 and does not mutate the ledger. We never
    // throw and never block beyond this single get_orders call.
    if (!order_id.isEmpty() && !account.isEmpty()) {
        auto& mgr = AccountManager::instance();
        if (IBroker* broker = mgr.broker_for(account)) {
            const BrokerCredentials creds = mgr.load_credentials(account);
            const auto resp = broker->get_orders(creds);
            if (resp.success && resp.data.has_value()) {
                for (const auto& o : resp.data.value()) {
                    if (o.order_id != order_id)
                        continue;
                    // Capture the broker's reported status whenever the order is found —
                    // regardless of whether it has a fill price. This lets callers
                    // surface the honest state ("accepted", "open", "partially_filled",
                    // "filled", …) rather than the optimistic "filled" they assumed.
                    out.status = normalize_order_status(o.status);
                    // A broker-confirmed quantity is the only permission to touch
                    // exposure. Cap it at the submitted quantity so a malformed
                    // adapter response cannot over-book this order.
                    if (o.filled_qty > 0.0 && qty > 0.0) {
                        out.qty = std::min(o.filled_qty, qty);
                        out.price = o.avg_price > 0.0 ? o.avg_price : resolved_price;
                        out.reconciled = o.avg_price > 0.0;
                        out.recorded = out.price > 0.0;
                    }
                    break;
                }
            }
        }
    }

    // Audit honestly which path was taken — confirmed broker fill, confirmed
    // quantity with price fallback, or no execution.
    if (out.recorded && out.reconciled)
        LOG_INFO("LivePnl",
                 QStringLiteral("fill reconciled order %1: broker %2 (qty %3) vs resolved %4")
                     .arg(order_id)
                     .arg(out.price)
                     .arg(out.qty)
                     .arg(resolved_price));
    else if (out.recorded)
        LOG_INFO("LivePnl",
                 QStringLiteral("fill quantity confirmed order %1: using resolved price %2 (qty %3)")
                     .arg(order_id)
                     .arg(resolved_price)
                     .arg(out.qty));
    else
        LOG_INFO("LivePnl",
                 QStringLiteral("order %1 not executed yet (status=%2); ledger unchanged")
                     .arg(order_id, out.status.isEmpty() ? QStringLiteral("unknown") : out.status));

    if (out.recorded) {
        if (side == OrderSide::Buy)
            record_open(account, venue, instrument, out.qty, out.price);
        else
            record_close(account, venue, instrument, out.qty, out.price);
    }
    return out;
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
