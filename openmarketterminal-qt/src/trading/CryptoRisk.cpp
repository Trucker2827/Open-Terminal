#include "trading/CryptoRisk.h"

#include <cmath>

namespace openmarketterminal::trading {

CryptoRiskVerdict crypto_risk_verdict(double quantity, double resolved_price, double max_order_value) {
    CryptoRiskVerdict v;
    if (!std::isfinite(quantity) || !std::isfinite(resolved_price) || !std::isfinite(max_order_value)) {
        v.reason = QStringLiteral("non-finite input");
        return v;
    }
    if (quantity <= 0.0) {
        v.reason = QStringLiteral("quantity must be > 0");
        return v;
    }
    if (resolved_price <= 0.0) {
        v.reason = QStringLiteral("no price available");
        return v;
    }
    v.order_value = quantity * resolved_price; // spot: contract multiplier = 1
    if (v.order_value > max_order_value) {
        v.reason = QStringLiteral("order value %1 exceeds max order value %2")
                       .arg(v.order_value, 0, 'f', 2).arg(max_order_value, 0, 'f', 2);
        return v;
    }
    v.ok = true;
    return v;
}

} // namespace openmarketterminal::trading
