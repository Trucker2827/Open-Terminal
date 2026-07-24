#pragma once
// REST account-poll cadence: relax to 30s ONLY while the authenticated WS
// stream has delivered an event within the last 15s; otherwise (never / gone
// stale) stay at the 5s baseline. Fail toward MORE polling, never less —
// REST remains the source of truth, the account WS is a latency optimization.

#include <QtGlobal>

namespace openmarketterminal::crypto {

inline int account_poll_interval_ms(qint64 now_ms, qint64 last_account_ws_event_ms) {
    constexpr int kBaselineMs = 5000;
    constexpr int kRelaxedMs = 30000;
    constexpr qint64 kWsFreshWindowMs = 15000;
    if (last_account_ws_event_ms <= 0)
        return kBaselineMs;
    return (now_ms - last_account_ws_event_ms < kWsFreshWindowMs) ? kRelaxedMs : kBaselineMs;
}

} // namespace openmarketterminal::crypto
