#include "trading/options/OptionSymbol.h"
#include <QRegularExpression>

namespace openmarketterminal::trading {

bool is_occ_option_symbol(const QString& symbol) {
    // OCC compact form (as Alpaca returns): root 1-6 uppercase letters, then
    // YYMMDD (6 digits), then C or P, then 8-digit strike (price × 1000).
    // \z (absolute end) instead of $ so a trailing newline does NOT match.
    // Standard OCC compact form only — adjusted option roots with embedded
    // digits (e.g. "AAPL1...") are intentionally out of scope.
    static const QRegularExpression re(QStringLiteral("^[A-Z]{1,6}[0-9]{6}[CP][0-9]{8}\\z"));
    return re.match(symbol).hasMatch();
}

int option_contract_multiplier(const QString& symbol) {
    return is_occ_option_symbol(symbol) ? 100 : 1;
}

} // namespace openmarketterminal::trading
