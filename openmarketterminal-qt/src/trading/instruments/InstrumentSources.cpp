#include "trading/instruments/InstrumentSources.h"

namespace openmarketterminal::trading {

// Legacy instrument-master registration seam. Current US equity brokers trade
// plain symbols and need no downloaded master, so there is nothing to register.
void register_extra_instrument_sources() {
}

} // namespace openmarketterminal::trading
