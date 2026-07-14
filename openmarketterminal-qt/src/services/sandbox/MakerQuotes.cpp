#include "services/sandbox/MakerQuotes.h"

namespace openmarketterminal::services::sandbox {

MakerQuotePair build_maker_quotes(double mid, double half_spread_bps) {
    MakerQuotePair pair;
    if (mid <= 0.0)
        return pair; // valid stays false
    pair.valid = true;
    pair.bid = MakerQuote{QStringLiteral("buy"), mid * (1.0 - half_spread_bps / 1e4)};
    pair.ask = MakerQuote{QStringLiteral("sell"), mid * (1.0 + half_spread_bps / 1e4)};
    return pair;
}

} // namespace openmarketterminal::services::sandbox
