#pragma once
#include "cli/CommandDispatch.h"
#include <QStringList>

namespace openmarketterminal::cli {

// `kalshi bot` command family — the PAPER Kalshi bot (ladder rung 1). Reads
// the spot calibrator's report, journals one decision row per contract (bids
// AND passes), and settles paper positions against real exchange settlement
// records. There is no live mode in this rung: `--mode live` is refused.
//
// Compiled as its own TU and excluded from unity builds like the other cli
// command families (MSVC front-end capacity; see CommandDispatch.cpp).
int kalshi_bot_command(const GlobalOpts& opts, QStringList args);

} // namespace openmarketterminal::cli
