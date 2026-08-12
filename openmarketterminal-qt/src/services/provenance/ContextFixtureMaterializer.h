#pragma once

#include "core/result/Result.h"
#include "services/provenance/TradeProvenanceIndex.h"

#include <QByteArray>

namespace openmarketterminal::provenance {

/// Parse a bounded, explicit context batch fixture. This is the foundation
/// materializer used to prove deterministic rebuilds before authoritative
/// source adapters are enabled. It grants no trading authority.
Result<ContextBatch> parse_context_fixture(const QByteArray& json);

} // namespace openmarketterminal::provenance
