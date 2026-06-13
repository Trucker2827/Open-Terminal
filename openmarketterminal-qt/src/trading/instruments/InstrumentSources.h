#pragma once
// Seam for registering broker instrument-master sources. The India-only brokers
// that used this have been removed; the kept brokers need no instrument master,
// so this is currently a no-op. Called once from InstrumentService.
namespace openmarketterminal::trading {
void register_extra_instrument_sources();
}
