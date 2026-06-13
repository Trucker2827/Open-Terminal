#include "trading/instruments/InstrumentSources.h"

namespace openmarketterminal::trading {

// The 17 India-only brokers that registered downloaded instrument-master sources
// here have been removed. The kept brokers (Alpaca, IBKR, Tradier, Saxo, MT4)
// trade plain symbols and need no instrument master, so there is nothing extra
// to register. Kept as a seam for any future broker that needs one.
void register_extra_instrument_sources() {
}

} // namespace openmarketterminal::trading
