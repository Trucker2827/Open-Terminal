#pragma once
// Honest paper-mode market fills (Task 10): walk the live order book at
// submit time instead of stamping ticker.last (or a placeholder constant),
// and REFUSE to fill against a stale or empty book. Pure — no I/O, no Qt
// widgets — so every rule is unit-testable.
//
// Staleness threshold matches the sandbox freshness convention (5s — see
// services/sandbox/FreshnessGate.h data_quality_from_freshness).
//
// fee_paid is INFORMATIONAL (display): the pt engine derives the recorded
// fee from PtPortfolio.fee_rate at fill time — pass that same rate here so
// the shown number matches the booked one, and never record it twice.

#include "trading/TradingTypes.h"

#include <QString>

#include <cmath>

namespace openmarketterminal::crypto {

inline constexpr qint64 kMaxPaperBookAgeMs = 5000;

struct PaperFillVerdict {
    bool ok = false;      // false → REJECT: show reason, place no order
    QString reason;       // human-visible rejection / partial-fill note
    double fill_price = 0; // size-weighted walk price
    double filled_qty = 0; // may be < requested (thin visible depth)
    double fee_paid = 0;   // taker fee on filled notional (display only)
};

inline PaperFillVerdict paper_market_fill(const QString& side, double qty,
                                          const trading::OrderBookData& book,
                                          qint64 book_age_ms, double taker_fee_rate) {
    PaperFillVerdict v;
    if (!std::isfinite(qty) || qty <= 0.0) {
        v.reason = QStringLiteral("invalid quantity");
        return v;
    }
    const bool is_buy = side == QLatin1String("buy");
    if (!is_buy && side != QLatin1String("sell")) {
        v.reason = QStringLiteral("unknown side '%1'").arg(side);
        return v;
    }
    if (book_age_ms > kMaxPaperBookAgeMs) {
        v.reason = QStringLiteral("stale book (%1 ms old) — paper fill refused").arg(book_age_ms);
        return v;
    }
    const auto& levels = is_buy ? book.asks : book.bids;
    if (levels.isEmpty()) {
        v.reason = QStringLiteral("no visible depth — paper fill refused");
        return v;
    }
    double remaining = qty;
    double notional = 0.0;
    for (const auto& level : levels) {
        const double price = level.first;
        const double size = level.second;
        if (price <= 0.0 || size <= 0.0)
            continue;
        const double take = std::min(remaining, size);
        notional += take * price;
        remaining -= take;
        if (remaining <= 0.0)
            break;
    }
    v.filled_qty = qty - remaining;
    if (v.filled_qty <= 0.0) {
        v.reason = QStringLiteral("no usable depth — paper fill refused");
        return v;
    }
    v.ok = true;
    v.fill_price = notional / v.filled_qty;
    v.fee_paid = notional * taker_fee_rate;
    if (remaining > 0.0)
        v.reason = QStringLiteral("partial fill: visible book depth %1 of %2").arg(v.filled_qty).arg(qty);
    return v;
}

} // namespace openmarketterminal::crypto
