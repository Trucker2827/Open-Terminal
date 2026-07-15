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

int reconnect_jitter_ms(const QString& symbol, const QString& source, bool rate_limited) {
    const QByteArray key = (symbol.trimmed().toUpper() + QLatin1Char('|')
                            + source.trimmed().toLower()).toUtf8();
    quint32 hash = 2166136261u;
    for (const unsigned char byte : key) {
        hash ^= byte;
        hash *= 16777619u;
    }
    const int span_ms = rate_limited ? 5000 : 1000;
    return static_cast<int>(hash % static_cast<quint32>(span_ms));
}

} // namespace openmarketterminal::services::crypto_latency
