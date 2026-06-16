#pragma once
// CryptoRisk.h — pure notional risk verdict for crypto (spot) orders.
// No Qt-widget/DB deps so it is unit-testable in isolation (mirrors OptionSymbol).
#include <QString>

namespace openmarketterminal::trading {

struct CryptoRiskVerdict {
    bool ok = false;
    double order_value = 0.0;   // quantity * resolved_price
    QString reason;             // empty when ok; human-readable rejection otherwise
};

// order_value = quantity * resolved_price (spot → multiplier 1). Rejects on
// non-positive quantity, non-positive price ("no price available"), or
// order_value > max_order_value ("exceeds max order value").
CryptoRiskVerdict crypto_risk_verdict(double quantity, double resolved_price, double max_order_value);

} // namespace openmarketterminal::trading
