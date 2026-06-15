#pragma once
// StrategyRunner.h — the paper strategy-loop harness (Task 1).
//
// Ticks a Strategy: reads market state via a ToolCaller, asks for trade
// intents, and runs each through prepare_order → submit_order(mode:paper),
// honoring the kill switch + substrate gates. It NEVER touches a live or
// destructive path — all enforcement rides the existing substrate tools.
#include "services/ai_strategy/Strategy.h"

#include <functional>

namespace openmarketterminal::ai_strategy {

/// Loop bounds. 0 means "unbounded" for that particular bound.
struct RunConfig {
    int interval_sec = 15;  ///< sleep between ticks; 0 = no sleep (tests).
    int max_iters = 0;      ///< stop after this many ticks; 0 = unbounded.
    int duration_sec = 0;   ///< stop after this wall-clock budget; 0 = unbounded.
};

/// Tally of what one run did. halted_by_kill_switch reflects EITHER the
/// top-of-tick kill-switch check OR a halt triggered by a substrate rejection
/// reason ("kill switch engaged" / "paper trading disabled").
struct RunSummary {
    int ticks = 0;
    int proposed = 0;
    int prepared = 0;
    int filled = 0;
    int rejected = 0;
    int errors = 0;
    bool halted_by_kill_switch = false;
};

class StrategyRunner {
  public:
    /// Sink for human-readable decision lines. Defaults to stdout ("[strategy] …").
    std::function<void(const QString&)> on_log;

    /// Drive `s` against `tc` under `cfg`. `stop_requested`, if set and it
    /// returns true, ends the loop after the current tick. Synchronous.
    RunSummary run(Strategy& s, ToolCaller& tc, const RunConfig& cfg,
                   std::function<bool()> stop_requested = {});
};

} // namespace openmarketterminal::ai_strategy
