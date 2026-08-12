#pragma once

#include <QString>

namespace openmarketterminal::screens::kalshi {

constexpr qint64 kContextIndexCockpitStaleAfterMs = 10 * 60 * 1000;

enum class ContextIndexReadState {
    Absent,
    Unreadable,
    Empty,
    Current,
};

struct ContextIndexCockpitInput {
    ContextIndexReadState read_state = ContextIndexReadState::Absent;
    QString current_batch_id;
    QString current_digest;
    qint64 capture_finished_at_ms = 0;
    int node_count = 0;
    int edge_count = 0;
    int unresolved_count = 0;
    int invariant_failure_count = 0;
    QString error;
};

struct ContextIndexCockpitCard {
    QString headline = QStringLiteral("PROVENANCE INDEX · NOT CHECKED");
    QString detail = QStringLiteral("OBSERVATION ONLY · never authorizes trading");
    QString role = QStringLiteral("grey");
    QString state = QStringLiteral("unknown");
};

inline QString context_index_age_text(qint64 age_ms) {
    if (age_ms < 0)
        return QStringLiteral("unknown age");
    if (age_ms < 60'000)
        return QStringLiteral("%1s ago").arg(age_ms / 1000);
    if (age_ms < 3'600'000)
        return QStringLiteral("%1m ago").arg(age_ms / 60'000);
    if (age_ms < 86'400'000)
        return QStringLiteral("%1h ago").arg(age_ms / 3'600'000);
    return QStringLiteral("%1d ago").arg(age_ms / 86'400'000);
}

/// Pure presentation. This card is deliberately outside the trading-health
/// ladder: the disposable index may explain a decision but may never authorize
/// one or make an otherwise valid trading action fail.
inline ContextIndexCockpitCard present_context_index_cockpit(const ContextIndexCockpitInput& input, qint64 now_ms) {
    ContextIndexCockpitCard card;
    switch (input.read_state) {
        case ContextIndexReadState::Absent:
            card.headline = QStringLiteral("PROVENANCE INDEX · NOT MATERIALIZED");
            card.detail = QStringLiteral("OBSERVATION ONLY · no context batch has been built");
            card.state = QStringLiteral("absent");
            return card;
        case ContextIndexReadState::Unreadable:
            card.headline = QStringLiteral("PROVENANCE INDEX · UNREADABLE");
            card.detail =
                QStringLiteral("OBSERVATION ONLY · %1")
                    .arg(input.error.isEmpty() ? QStringLiteral("index could not be inspected") : input.error);
            card.role = QStringLiteral("amber");
            card.state = QStringLiteral("unreadable");
            return card;
        case ContextIndexReadState::Empty:
            card.headline = QStringLiteral("PROVENANCE INDEX · EMPTY");
            card.detail = QStringLiteral("OBSERVATION ONLY · schema ready; no published batch");
            card.state = QStringLiteral("empty");
            return card;
        case ContextIndexReadState::Current:
            break;
    }

    const qint64 age_ms =
        input.capture_finished_at_ms > 0 ? qMax<qint64>(0, now_ms - input.capture_finished_at_ms) : -1;
    const bool stale = age_ms < 0 || age_ms > kContextIndexCockpitStaleAfterMs;
    const QString digest = input.current_digest.startsWith(QStringLiteral("sha256:")) ? input.current_digest.mid(7, 10)
                                                                                      : input.current_digest.left(10);
    const QString identity = QStringLiteral("batch %1 · %2%3")
                                 .arg(input.current_batch_id, digest.isEmpty() ? QStringLiteral("no digest") : digest,
                                      digest.isEmpty() ? QString() : QStringLiteral("…"));
    const QString counts = QStringLiteral("%1 nodes · %2 edges").arg(input.node_count).arg(input.edge_count);

    if (input.invariant_failure_count > 0) {
        card.headline = QStringLiteral("PROVENANCE INDEX · %1 INVARIANT FAILURE%2")
                            .arg(input.invariant_failure_count)
                            .arg(input.invariant_failure_count == 1 ? QString() : QStringLiteral("S"));
        card.role = QStringLiteral("red");
        card.state = QStringLiteral("invariant_failure");
    } else if (input.unresolved_count > 0) {
        card.headline = QStringLiteral("PROVENANCE INDEX · %1 UNRESOLVED").arg(input.unresolved_count);
        card.role = QStringLiteral("amber");
        card.state = QStringLiteral("unresolved");
    } else if (stale) {
        card.headline = QStringLiteral("PROVENANCE INDEX · STALE");
        card.role = QStringLiteral("amber");
        card.state = QStringLiteral("stale");
    } else {
        card.headline = QStringLiteral("PROVENANCE INDEX · INDEXED · %1").arg(counts);
        // Cyan means inspectable, not authorised. Green is intentionally not
        // used for this non-authoritative subsystem.
        card.role = QStringLiteral("cyan");
        card.state = QStringLiteral("indexed");
    }
    card.detail =
        QStringLiteral("OBSERVATION ONLY · %1 · %2 · %3").arg(identity, context_index_age_text(age_ms), counts);
    return card;
}

} // namespace openmarketterminal::screens::kalshi
