#pragma once

namespace openmarketterminal::services::news {

// Offscreen, network-free one-shot exercising the real event-impact producer
// glue (run_btc_event_impact) end to end via a stub Python scorer.  Proves both
// branches: (1) a valid scorer writes a parseable snapshot with an `events`
// array; (2) a failing scorer writes NO snapshot and leaves the prior good file
// untouched.  Prints `SELFTEST btc-event-impact OK` and returns 0 on success,
// a diagnostic and non-zero otherwise.
int run_btc_event_impact_selftest();

} // namespace openmarketterminal::services::news
