#pragma once
// StrategyRunner.h — the paper strategy-loop harness (Task 1).
//
// Ticks a Strategy: reads market state via a ToolCaller, asks for trade
// intents, and runs each through prepare_order → submit_order(mode:paper),
// honoring the kill switch + substrate gates. It NEVER touches a live or
// destructive path — all enforcement rides the existing substrate tools.
#include "services/ai_decision/DecisionContext.h"
#include "services/ai_strategy/Strategy.h"

#include <functional>

namespace openmarketterminal::ai_strategy {

/// Loop bounds. 0 means "unbounded" for that particular bound.
///
/// max_notional_per_order/max_position_qty/allowed_venues/require_cost_gate/
/// require_freshness_gate feed the pre-trade guardrail (PretradeGate.h) — a
/// GatePolicy is built from these each run(). 0/empty means "no cap" for the
/// numeric/list fields; the gate ONLY rejects, it never opens a live path.
struct RunConfig {
    int interval_sec = 15;  ///< sleep between ticks; 0 = no sleep (tests).
    int max_iters = 0;      ///< stop after this many ticks; 0 = unbounded.
    int duration_sec = 0;   ///< stop after this wall-clock budget; 0 = unbounded.

    double max_notional_per_order = 0.0; ///< 0 = no cap.
    double max_position_qty = 0.0;       ///< 0 = no cap.
    double max_aggregate_position_qty = 0.0;  ///< 0 = no cross-handler aggregate cap.
    QStringList allowed_venues;          ///< empty = no venue restriction.
    bool require_cost_gate = true;
    bool require_freshness_gate = true;
    bool require_floor = true;  ///< ON: skip symbols the honest edge system doesn't endorse.

    QString submit_mode = QStringLiteral("paper");  ///< "paper" (default) | "live". Live is set
                                                    ///< ONLY by the armed CLI path; submit_order
                                                    ///< re-checks every live gate as the authority.
};

/// Tally of what one run did. halted_by_kill_switch reflects EITHER the
/// top-of-tick kill-switch check OR a halt triggered by a substrate rejection
/// reason ("kill switch engaged" / "paper trading disabled").
struct RunSummary {
    /// One pre-trade-guardrail rejection: the intent never reached
    /// prepare_order/submit_order at all.
    struct GateRejection {
        QString symbol;
        QString side;
        QString reason;
        QString rule;
    };

    int ticks = 0;
    int proposed = 0;
    int prepared = 0;
    int filled = 0;
    int rejected = 0;
    int errors = 0;
    bool halted_by_kill_switch = false;
    QVector<GateRejection> gate_rejections;
    int floor_skipped = 0;
    QVector<GateRejection> floor_skips;  ///< symbols skipped by the deterministic floor (rule "floor").
};

class StrategyRunner {
  public:
    /// Sink for human-readable decision lines. Defaults to stdout ("[strategy] …").
    std::function<void(const QString&)> on_log;

    /// Resolves a DecisionPacket for a symbol, feeding the pre-trade
    /// guardrail's cost/freshness/edge inputs. Defaults to the real
    /// ai_decision::assess (a pure DB read); tests inject a fake to control
    /// gate outcomes without a live edge_decision_journal.
    std::function<ai_decision::DecisionPacket(const QString&)> assess_fn =
        [](const QString& sym) { return ai_decision::assess(sym); };

    /// Drive `s` against `tc` under `cfg`. `stop_requested`, if set and it
    /// returns true, ends the loop after the current tick. Synchronous.
    RunSummary run(Strategy& s, ToolCaller& tc, const RunConfig& cfg,
                   std::function<bool()> stop_requested = {});
};

} // namespace openmarketterminal::ai_strategy
