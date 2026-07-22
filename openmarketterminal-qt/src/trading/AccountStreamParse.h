#pragma once
// Classification of ws_stream.py authenticated account lines (Task 7).
// Pure so the routing rules are unit-testable; ExchangeSession dispatches on
// the returned kind and publishes to DataHub account topics.

#include <QJsonObject>
#include <QString>

namespace openmarketterminal::trading {

struct AccountLine {
    QString kind;        // "order" | "mytrade" | "balance" | "" (not an account line)
    QString symbol;      // unified pair for order/mytrade; empty for balance
    QJsonObject payload; // the inner order/trade object, or the balances map
};

/// Classify a ws_stream.py JSON line. Non-account lines (ticker/orderbook/…)
/// return kind == "" so the public-data dispatch handles them unchanged.
inline AccountLine parse_account_line(const QJsonObject& j) {
    const QString t = j.value(QStringLiteral("type")).toString();
    if (t == QLatin1String("account_order")) {
        const QJsonObject o = j.value(QStringLiteral("order")).toObject();
        return {QStringLiteral("order"), o.value(QStringLiteral("symbol")).toString(), o};
    }
    if (t == QLatin1String("account_mytrade")) {
        const QJsonObject o = j.value(QStringLiteral("trade")).toObject();
        return {QStringLiteral("mytrade"), o.value(QStringLiteral("symbol")).toString(), o};
    }
    if (t == QLatin1String("account_balance"))
        return {QStringLiteral("balance"), QString(), j.value(QStringLiteral("balances")).toObject()};
    return {QString(), QString(), {}};
}

} // namespace openmarketterminal::trading
