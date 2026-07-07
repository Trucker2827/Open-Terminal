#pragma once
// TickTail.h — market-data tail reader for the Strategy Sandbox fill model
// (Task 3). Reads recent rows out of a scalp_ticks.jsonl file (+ its ".1"
// rotation predecessor, exactly like automation::latest_candidate's
// active+prev blending) without pulling in the CLI layer.
//
// Layering note: the natural reuse target for tailing/rotation semantics is
// automation::read_tail (src/cli/automation/AutomationState.h), but that file
// is compiled into the CLI targets, not into openterminal_core. A
// services/sandbox file (core) including a cli/ header would invert the
// dependency graph. Rather than move AutomationState or add a cli->core
// link, this file carries its own small tail reader
// (read_tail_with_prev, in the .cpp) that copies automation::read_tail's
// exact trim-at-first-newline behavior plus latest_candidate's ".1"
// prepend-when-short fallback. Keep the two in sync by hand if either
// changes.

#include <QString>
#include <QVector>

namespace openmarketterminal::services::sandbox {

/// One parsed row of scalp_ticks.jsonl.
struct TickRow {
    QString symbol;
    double price = 0;
    double best_bid = 0;
    double best_ask = 0;
    qint64 ts_ms = 0;
};

// Reads the last tail_bytes of ticks_path (plus, when the active file is
// shorter than tail_bytes, the tail end of ticks_path + ".1" to fill the
// remaining budget -- same blending rule as automation::latest_candidate).
// Returns rows for the given symbol with ts_ms > since_ms (exclusive),
// ascending by ts_ms. Rows with price <= 0, an unparseable/non-positive
// received_ts_ms, or malformed JSON are skipped rather than erroring.
//
// tail_bytes default (kTickTailBytes = 2 MiB) covers roughly 8k ticks --
// comfortably more than a 30 second decision cycle needs, with margin.
inline constexpr qint64 kTickTailBytes = 2LL * 1024 * 1024;

QVector<TickRow> ticks_since(const QString& ticks_path, const QString& symbol, qint64 since_ms,
                              qint64 tail_bytes = kTickTailBytes);

} // namespace openmarketterminal::services::sandbox
