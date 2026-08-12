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

bool feed_is_silently_stale(qint64 last_message_ms, qint64 now_ms, bool connected,
                            qint64 silence_limit_ms) {
    if (!connected) return false;
    if (last_message_ms <= 0) return false;
    if (silence_limit_ms <= 0) return false;
    // A clock that moved backwards is not evidence of silence.
    if (now_ms < last_message_ms) return false;
    return now_ms - last_message_ms > silence_limit_ms;
}

} // namespace openmarketterminal::services::crypto_latency
