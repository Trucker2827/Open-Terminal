#pragma once
// Deduped fill/cancel notification decisions (Task 14). Pure: takes unified
// ccxt order events (account WS AND the confirming REST refresh — both can
// report the same transition) and returns a message exactly once per
// (order id, terminal status). open/partially_filled are silent.

#include <QJsonObject>
#include <QSet>
#include <QString>

namespace openmarketterminal::crypto {

class CryptoFillNotifier {
  public:
    /// Returns the notification message, or empty when suppressed.
    QString on_order_event(const QJsonObject& order) {
        const QString id = order.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            return {}; // never notify unidentifiable orders
        const QString status = order.value(QStringLiteral("status")).toString();
        if (status != QLatin1String("filled") && status != QLatin1String("closed") &&
            status != QLatin1String("canceled") && status != QLatin1String("rejected"))
            return {};
        const QString key = id + QLatin1Char(':') + status;
        if (seen_.contains(key))
            return {};
        if (seen_.size() > 4096)
            seen_.clear(); // bound memory; worst case = one duplicate notification
        seen_.insert(key);
        const double qty = order.value(QStringLiteral("filled")).toDouble();
        const double px = order.value(QStringLiteral("average")).toDouble() > 0
                              ? order.value(QStringLiteral("average")).toDouble()
                              : order.value(QStringLiteral("price")).toDouble();
        return QStringLiteral("%1 %2 %3 %4 @ %5")
            .arg(order.value(QStringLiteral("side")).toString().toUpper())
            .arg(qty)
            .arg(order.value(QStringLiteral("symbol")).toString())
            .arg(status)
            .arg(px);
    }

  private:
    QSet<QString> seen_;
};

} // namespace openmarketterminal::crypto
