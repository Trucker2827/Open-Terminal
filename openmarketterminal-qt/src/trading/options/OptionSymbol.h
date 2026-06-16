#pragma once
#include <QString>

namespace openmarketterminal::trading {

/// True iff `symbol` is an OCC option symbol: root(1-6 A-Z) + YYMMDD + C|P + 8-digit strike.
/// Equity ("AAPL"), crypto ("BTC/USD"), and "EXCH:SYM:CONID" never match.
bool is_occ_option_symbol(const QString& symbol);

/// Shares represented per unit: 100 for an OCC option symbol, else 1.
int option_contract_multiplier(const QString& symbol);

} // namespace openmarketterminal::trading
