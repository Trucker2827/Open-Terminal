#pragma once
// Live-mode ladder overlay normalization (Task 9).
// Pure: unified ccxt order JSON → CryptoLadderModel MyOrder entries, plus a
// running VWAP estimate of the open long's average entry from my-trades.
//
// The avg-entry marker is an ESTIMATE from per-symbol trade history (spot
// venues expose no fetchPositions): deposits/withdrawals make it approximate,
// and a flat-or-net-short book shows NO marker (short estimation is out of
// scope — the ladder labels it "est. avg entry").

#include "screens/crypto_trading/CryptoLadderModel.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace openmarketterminal::crypto {

/// Keep resting orders only: status open/partially_filled, matching symbol,
/// positive price and positive remaining quantity. qty = remaining.
inline QVector<MyOrder> live_orders_to_my_orders(const QVector<QJsonObject>& unified_orders,
                                                 const QString& symbol) {
    QVector<MyOrder> out;
    for (const QJsonObject& o : unified_orders) {
        if (o.value(QStringLiteral("symbol")).toString() != symbol)
            continue;
        const QString status = o.value(QStringLiteral("status")).toString();
        if (status != QLatin1String("open") && status != QLatin1String("partially_filled"))
            continue;
        const double price = o.value(QStringLiteral("price")).toDouble();
        const double remaining = o.value(QStringLiteral("remaining")).toDouble();
        if (price <= 0.0 || remaining <= 0.0)
            continue;
        MyOrder m;
        m.price = price;
        m.qty = remaining;
        m.is_buy = o.value(QStringLiteral("side")).toString() == QLatin1String("buy");
        out.append(m);
    }
    return out;
}

/// Running VWAP of the net open long quantity. Sells reduce at the current
/// average (cost-basis method); a net quantity <= 0 clears to flat.
class LiveAvgEntry {
  public:
    void add_trade(const QString& side, double price, double amount) {
        if (price <= 0.0 || amount <= 0.0)
            return;
        if (side == QLatin1String("buy")) {
            cost_basis_ += price * amount;
            net_qty_ += amount;
            return;
        }
        if (side != QLatin1String("sell"))
            return;
        cost_basis_ -= avg_entry() * amount;
        net_qty_ -= amount;
        if (net_qty_ <= 0.0)
            reset();
    }
    double avg_entry() const { return net_qty_ > 0.0 ? cost_basis_ / net_qty_ : 0.0; }
    double net_qty() const { return net_qty_; }
    void reset() {
        net_qty_ = 0.0;
        cost_basis_ = 0.0;
    }

  private:
    double net_qty_ = 0.0;
    double cost_basis_ = 0.0;
};

} // namespace openmarketterminal::crypto
