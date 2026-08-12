#pragma once

#include "screens/kalshi/ContextIndexCockpitPresentation.h"

#include <QString>

namespace openmarketterminal::screens::kalshi {

ContextIndexCockpitInput read_context_index_cockpit(const QString& path);
ContextIndexCockpitInput read_default_context_index_cockpit();

} // namespace openmarketterminal::screens::kalshi
