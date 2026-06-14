#pragma once
// Seam for registering broker instrument-master sources. Registered US equity
// brokers (Alpaca, IBKR, Tradier) use plain symbols and need no instrument
// master download; this is currently a no-op. Called once from InstrumentService.
namespace openmarketterminal::trading {
void register_extra_instrument_sources();
}
