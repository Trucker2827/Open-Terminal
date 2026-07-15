#include "services/crypto_latency/FeedReconnect.h"

#include <algorithm>

namespace openmarketterminal::services::crypto_latency {

bool is_rate_limited(const QString& error_string) {
    return error_string.contains(QStringLiteral("429"))
           || error_string.contains(QStringLiteral("Too Many Requests"), Qt::CaseInsensitive);
}

int next_reconnect_delay_ms(int attempt, bool rate_limited, int base_ms, int cap_ms,
                            int rate_limited_floor_ms) {
    const int shift = std::min(attempt < 0 ? 0 : attempt, 20);
    long long grown = static_cast<long long>(base_ms) << shift;
    if (grown > cap_ms)
        grown = cap_ms;
    if (rate_limited && grown < rate_limited_floor_ms)
        grown = std::min(rate_limited_floor_ms, cap_ms);
    return static_cast<int>(grown);
}

} // namespace openmarketterminal::services::crypto_latency
