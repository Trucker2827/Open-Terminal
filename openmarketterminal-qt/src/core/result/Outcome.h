// src/core/result/Outcome.h
#pragma once

#include <QString>

namespace openmarketterminal {

/// Honest outcome of any user-triggered action. The spine of the "honesty pass":
/// a UI element may render green/Ok ONLY when real work completed. Skipped,
/// stubbed, synthetic, unavailable, missing-input, passthrough, and
/// not-implemented paths must map to Failed/Unavailable/Demo — never Ok.
enum class OutcomeKind {
    Ok,           // real work happened
    Skipped,      // deliberately not run (e.g. branch not taken) — neutral, NOT a success
    Failed,       // tried, did not succeed (not-implemented, no-input, source-failed)
    Unavailable,  // cannot run as configured (requires server/package/key, not wired)
    Demo          // ran on synthetic data behind an explicit, visible opt-in
};

inline QString to_display_string(OutcomeKind k) {
    switch (k) {
        case OutcomeKind::Ok:          return QStringLiteral("OK");
        case OutcomeKind::Skipped:     return QStringLiteral("SKIPPED");
        case OutcomeKind::Failed:      return QStringLiteral("FAILED");
        case OutcomeKind::Unavailable: return QStringLiteral("UNAVAILABLE");
        case OutcomeKind::Demo:        return QStringLiteral("DEMO");
    }
    return QStringLiteral("FAILED");
}

} // namespace openmarketterminal
