#pragma once
// FeedReconnect.h — pure decisions for rate-limit-aware WS feed reconnection.
// No sockets/timers/wall-clock: these are the unit-tested core that the
// CryptoLatencyService reconnection wiring (glue) drives.
#include <QString>
#include <QtGlobal>

namespace openmarketterminal::services::crypto_latency {

// True iff the socket/handshake error indicates a rate limit (HTTP 429).
bool is_rate_limited(const QString& error_string);

// Exponential backoff for reconnect attempt `attempt` (0-based). Doubles from
// base_ms, capped at cap_ms. When rate_limited, never returns less than
// rate_limited_floor_ms (so we do not re-trip the limit). Deterministic:
// jitter is applied by the caller.
int next_reconnect_delay_ms(int attempt, bool rate_limited, int base_ms, int cap_ms,
                            int rate_limited_floor_ms);

// True iff a feed that BELIEVES it is connected has gone SILENT long enough to
// be treated as dead.
//
// This exists because of a measured failure: a Coinbase socket delivered
// nothing for 74 minutes while reporting `error = none`. Nothing tore it down,
// because nothing was watching -- the reconnect path only fires on an error,
// and a half-open socket never produces one. A router reset did not fix it
// either: the app was not trying to reconnect, it believed it was fine. The
// only symptom was a red banner reading "FEED DOWN - no reason given", which
// is exactly what a status with no error attached looks like.
//
// `connected == false` returns false: a feed already known to be down is the
// existing reconnect path's business, not this one's. `last_message_ms <= 0`
// also returns false -- a feed that has never delivered anything has a connect
// problem, and treating "never" as "stale" would fight the backoff.
//
// The bound is a caller's policy, not a constant here. For crypto majors the
// natural cadence is sub-second on every venue (measured: 1s on all nine live
// streams), so any bound above a few seconds is generous; a market that could
// legitimately be quiet for minutes needs a larger one.
bool feed_is_silently_stale(qint64 last_message_ms, qint64 now_ms, bool connected,
                            qint64 silence_limit_ms);

// Stable per-(symbol, source) jitter. Distinct symbol feeds must not retry a
// rate-limited venue in lockstep and continually renew the same IP-level 429.
int reconnect_jitter_ms(const QString& symbol, const QString& source, bool rate_limited);

} // namespace openmarketterminal::services::crypto_latency
