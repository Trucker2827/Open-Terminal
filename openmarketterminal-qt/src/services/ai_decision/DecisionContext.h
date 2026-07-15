#pragma once
// DecisionContext.h — ai ctx decision-packet Task 2: DecisionContext::assess,
// the reusable, PURE-READ core that assembles a decision-ready packet for one
// symbol from edge_decision_journal plus the cost/freshness/lane machinery
// already built for the Strategy Sandbox. `ai screen` (Task 3) and the
// guardrail layer (Task 4/piece 4 of the wider AI decision surface) both
// reuse this seam instead of re-deriving the same gates.
//
// READ-ONLY INVARIANT (binding): assess() issues only SELECTs against
// edge_decision_journal and sandbox_position/sandbox_strategy. It never
// INSERTs/UPDATEs/DELETEs, places an order, or mutates a gate/setting. It is
// synchronous and does not touch the event loop or any broker connection —
// callers that need a live position/buying-power figure get it from the
// AI ledger (a later piece); until then position/headroom is a documented
// best-effort stub (see PositionSource note below).
//
// COST ACCOUNTING (binding): round_trip_cost_bps and clears_cost come from
// the edge_decision_journal row's OWN recorded spread_cost/fee_cost/
// edge_after_cost/gate columns — i.e. whatever cost model the producer that
// wrote the row already applied — NOT a fresh call into the spot maker cost
// model. This keeps the packet consistent with what actually produced the
// signal, and keeps assess() a pure DB read (no venue-specific cost
// recomputation).

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace openmarketterminal::ai_decision {

// One symbol's decision-ready packet: the latest edge_decision_journal row
// for `symbol` (raw fields, unchanged), plus derived cost/freshness/lane/
// position verdicts and a single pure-precedence recommendation_hint.
struct DecisionPacket {
    // --- Query identity ---
    QString symbol;
    QString market; // caller-supplied market/venue filter hint (not applied
                     // to the query — the journal is keyed on symbol alone,
                     // see assess()'s doc comment); carried through so
                     // consumers can label the packet.

    // --- Raw edge_decision_journal fields (latest row for `symbol`) ---
    bool has_edge_signal = false; // false when no row exists for `symbol`.
    QString venue;
    QString horizon;
    QString market_id;
    QString question;
    QString direction;
    QString side;
    QString call;
    QString gate;
    double market_probability = 0.0;
    double model_probability = 0.0;
    double raw_edge = 0.0;
    double edge_after_cost = 0.0;
    double gate_edge = 0.0;
    double spread_cost = 0.0;
    double fee_cost = 0.0;
    double liquidity_score = 0.0;
    double confidence = 0.0;
    qint64 seconds_left = -1;
    QString data_status;
    QString reasons;
    QString source;

    // --- Derived: cost (from the journal's own accounting, see file header) ---
    double round_trip_cost_bps = 0.0; // spread_cost + fee_cost
    QString clears_cost;              // "true"|"false"|"unknown" (no edge row)

    // --- Derived: freshness (Task 1's data_quality_from_freshness) ---
    QString freshness; // "ok"|"degraded"|"unknown"

    // --- Derived: lane significance (best-effort; see DecisionContext.cpp) ---
    QString lane_verdict; // "EDGE"|"no edge"|"insufficient"|"unknown"

    // --- Position/headroom: best-effort stub until the AI ledger (a later
    // piece) is wired. assess() is synchronous and must not reach into a
    // broker/event-loop connection, so these are placeholder values, not a
    // live read. See notes for the explicit reason. ---
    QString position_source = QStringLiteral("none");
    double position_qty = 0.0;
    double buying_power = -1.0; // -1 sentinel: "unknown", not "zero headroom".

    // --- Final verdict ---
    QString recommendation_hint;

    QStringList notes;
};

// Reads the latest edge_decision_journal row for `symbol` (ORDER BY
// created_at DESC LIMIT 1) and assembles a DecisionPacket: cost/clears_cost
// from the row's own spread_cost/fee_cost/edge_after_cost/gate columns,
// freshness via FreshnessGate.h's data_quality_from_freshness over the row's
// freshness_json, lane_verdict as a best-effort read of the symbol's
// resolved, honest sandbox_position samples through
// SandboxScorer.h's evaluate_lane_significance, and recommendation_hint via
// compute_hint(). `market` is not used to filter the query (the journal has
// no market-scoped uniqueness guarantee beyond symbol+created_at); it is
// only carried through into the returned packet as a label. PURE READ: no
// writes, no order placement, no gate mutation — see file header.
DecisionPacket assess(const QString& symbol, const QString& market = {});

// Serializes every DecisionPacket field to a QJsonObject.
QJsonObject to_json(const DecisionPacket& packet);

// Pure precedence rule (no DB, no side effects):
//   !has_edge_signal        -> "no edge signal"
//   freshness != "ok"       -> "blocked: stale data"
//   clears_cost == "false"  -> "blocked: below cost"
//   otherwise               -> "all gates pass"
QString compute_hint(const DecisionPacket& packet);

} // namespace openmarketterminal::ai_decision
