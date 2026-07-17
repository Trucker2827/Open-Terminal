#include "services/ai_ledger/AiLedger.h"

#include "services/sandbox/PaperFillModel.h"

#include <QDateTime>
#include <QLatin1String>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace openmarketterminal {
namespace ai_ledger {

namespace {
// No real position is smaller than this; anything below is float dust from repeated
// weighted-average / addition ops on fractional (crypto) lot sizes.
constexpr double kFlatEpsilon = 1e-9;
} // namespace

FillDelta apply_fill(const LedgerPosition& current, const QString& side, double qty, double price, double fee) {
    const bool sell = (side == QLatin1String("sell") || side == QLatin1String("short"));
    const double signed_qty = sell ? -qty : qty;

    LedgerPosition p = current;
    double realized_this = 0.0;

    const double net = current.net_qty;
    const bool same_dir = (net == 0.0) || ((net > 0.0) == (signed_qty > 0.0));

    if (same_dir) {
        // Open or average-in. New avg is size-weighted, with the fee folded into
        // the cost basis (worsening the effective entry: +fee for a long, -fee for
        // a short). Booking the fee here rather than as immediate realized P&L
        // keeps an OPEN's realized_pnl == 0, so the scorecard's realized-close
        // filter still distinguishes opens from closes; the fee is realized at the
        // eventual close via the worsened entry. (F3)
        const double prev_abs = std::abs(net);
        const double add_abs = std::abs(signed_qty);
        const double total_abs = prev_abs + add_abs;
        const double fee_basis = sell ? -fee : fee;  // long adds cost, short reduces proceeds
        p.avg_entry_price = total_abs > 0.0
            ? (current.avg_entry_price * prev_abs + price * add_abs + fee_basis) / total_abs
            : 0.0;
        p.net_qty = net + signed_qty;
        realized_this = 0.0;
        p.realized_pnl = current.realized_pnl;  // unchanged on an open — fee lives in the basis
    } else {
        // Opposite direction: close up to min(|net|, qty), realizing PnL on the closed portion.
        const double closing = std::min(std::abs(net), qty);
        const QString open_side = net > 0.0 ? QStringLiteral("buy") : QStringLiteral("sell");
        realized_this = services::sandbox::realized_pnl(open_side, current.avg_entry_price, price, closing, 0.0, fee);
        p.realized_pnl = current.realized_pnl + realized_this;

        const double leftover_open = qty - closing;  // > 0 only when the fill flips the position
        if (leftover_open > 0.0) {
            p.net_qty = (signed_qty > 0.0 ? 1.0 : -1.0) * leftover_open;
            p.avg_entry_price = price;
        } else {
            p.net_qty = net + signed_qty;  // moves toward zero
            if (p.net_qty == 0.0)
                p.avg_entry_price = 0.0;
            // avg_entry unchanged on a partial close
        }
    }
    if (std::abs(p.net_qty) < kFlatEpsilon) {
        p.net_qty = 0.0;
        p.avg_entry_price = 0.0;
    }
    return FillDelta{p, realized_this};
}

double unrealized_of(const LedgerPosition& p, double mark_price) {
    return (mark_price - p.avg_entry_price) * p.net_qty;
}

namespace {

LedgerPosition fold(const QVector<AiFill>& fills) {
    LedgerPosition p;
    for (const AiFill& f : fills)
        p = apply_fill(p, f.side, f.quantity, f.fill_price, f.fee).position;
    return p;
}

} // namespace

LedgerPosition position_of(const QString& handler, const QString& symbol) {
    auto fills = AiFillRepository::instance().fills_for(handler, symbol);
    if (fills.is_err())
        return LedgerPosition{};
    return fold(fills.value());
}

Result<AiFill> record_fill(const QString& handler, const QString& symbol, const QString& side,
                           double qty, double price, double fee, const QString& draft_id) {
    if (qty <= 0.0)
        return Result<AiFill>::err("record_fill: non-positive quantity");
    if (price <= 0.0)
        return Result<AiFill>::err("record_fill: non-positive price");

    auto prior = AiFillRepository::instance().fills_for(handler, symbol);
    if (prior.is_err())
        return Result<AiFill>::err(prior.error());
    const LedgerPosition current = fold(prior.value());
    const FillDelta delta = apply_fill(current, side, qty, price, fee);

    AiFill row;
    row.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    row.handler = handler;
    row.symbol = symbol;
    row.side = side;
    row.quantity = qty;
    row.fill_price = price;
    row.fee = fee;
    row.realized_pnl = delta.realized_pnl_this_fill;
    row.ts = QDateTime::currentMSecsSinceEpoch();
    row.draft_id = draft_id;

    auto appended = AiFillRepository::instance().append(row);
    if (appended.is_err())
        return Result<AiFill>::err(appended.error());
    return Result<AiFill>::ok(row);
}

QVector<HandlerPosition> positions_of(const QString& handler) {
    QVector<HandlerPosition> out;
    auto pairs = AiFillRepository::instance().distinct_handler_symbols(handler);
    if (pairs.is_err())
        return out;
    for (const auto& hs : pairs.value()) {
        LedgerPosition p = position_of(hs.first, hs.second);
        if (p.net_qty != 0.0)
            out.push_back(HandlerPosition{hs.first, hs.second, p});
    }
    return out;
}

double net_position_for_symbol(const QString& symbol) {
    double sum = 0.0;
    for (const HandlerPosition& hp : positions_of()) {
        if (hp.symbol == symbol)
            sum += hp.position.net_qty;
    }
    return sum;
}

double realized_total(const QString& handler) {
    double sum = 0.0;
    auto fills = AiFillRepository::instance().list(handler, {}, 0);  // all fills, no symbol filter
    if (fills.is_err())
        return 0.0;
    for (const AiFill& f : fills.value())
        sum += f.realized_pnl;
    return sum;
}

} // namespace ai_ledger
} // namespace openmarketterminal
