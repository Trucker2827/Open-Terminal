#pragma once
// FeedReconnect.h — pure decisions for rate-limit-aware WS feed reconnection.
// No sockets/timers/wall-clock: these are the unit-tested core that the
// CryptoLatencyService reconnection wiring (glue) drives.
#include <QString>

namespace openmarketterminal::services::crypto_latency {

// True iff the socket/handshake error indicates a rate limit (HTTP 429).
bool is_rate_limited(const QString& error_string);

// Exponential backoff for reconnect attempt `attempt` (0-based). Doubles from
// base_ms, capped at cap_ms. When rate_limited, never returns less than
// rate_limited_floor_ms (so we do not re-trip the limit). Deterministic:
// jitter is applied by the caller.
int next_reconnect_delay_ms(int attempt, bool rate_limited, int base_ms, int cap_ms,
                            int rate_limited_floor_ms);

} // namespace openmarketterminal::services::crypto_latency
