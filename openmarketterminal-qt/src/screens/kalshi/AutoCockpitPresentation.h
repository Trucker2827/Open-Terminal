#pragma once

// Presentation for the Kalshi screen's AUTO COCKPIT header. Pure function of
// (the cockpit's own input freshness, now) so the honesty rules are regression-
// testable without a widget.
//
// The cockpit consumes three things: the market list (all_markets_), the event
// ladder's order books (kalshi_books_), and an auto_context assembled from
// spot/volatility/horizon evidence. Until this header existed, none of that
// freshness was on screen: a market list frozen at the last user interaction
// and a book feed that had stopped both rendered as a static ladder that was
// indistinguishable from a working one with nothing to say.
//
// Two rules this header exists to enforce:
//
//  1. Ages are measured against `now` on every tick, never against the last
//     time the engine happened to run. A cockpit whose engine has stopped
//     therefore reads STALE and gets staler, instead of freezing at its
//     last-good text.
//  2. When the inputs are not live, the ladder is NOT left showing its
//     last-good prices. `ladder_trustworthy` is false and `ladder_notice`
//     carries the one sentence that replaces the rows. Prices derived from
//     inputs the screen has just called untrustworthy are the "silently
//     degrades" failure, not a convenience.
//
// Book ages come from the surface points the engine already built
// (KalshiSurfacePoint::quote_observed_at_ms), never from a second lookup into
// kalshi_books_ — a parallel implementation would eventually disagree with the
// ladder rendered underneath it.

#include <QString>
#include <QStringList>

namespace openmarketterminal::screens::kalshi {

// ── Role statements ────────────────────────────────────────────────────────
// The Predictions screen carries three automation-flavoured surfaces. One
// definition of what each one is, rendered on the cockpit (all three, so the
// relationship is stated) and on each surface's own tab.
inline QString auto_cockpit_role() {
    return QStringLiteral(
        "AUTO COCKPIT — deterministic research ladder + bounded manual execution.");
}
inline QString bot_surface_role() {
    return QStringLiteral(
        "BOT — autonomous paper/live bidding under the sealed promotion gate.");
}
inline QString advisor_canary_surface_role() {
    return QStringLiteral(
        "ADVISOR & CANARY — archived blind-duel protocol, read-only.");
}

// ── Freshness bounds ───────────────────────────────────────────────────────

// The market list is expected to re-list every 30 s (the periodic refresh
// tracked by #137). Two misses and the list can no longer be trusted to hold
// the contract that is currently open on a 15-minute cadence, so the ladder is
// working from contracts that may already have settled.
inline constexpr qint64 kCockpitMarketRefreshIntervalMs = 30'000;
inline constexpr qint64 kCockpitMarketsStaleMs = 2 * kCockpitMarketRefreshIntervalMs;

// The book feed's freshness window, matching the one the screen already
// applies to the same data in refresh_flow_meter (a daemon book snapshot older
// than 30 s is rejected there). Measured against the NEWEST leg quote: that is
// the question "is the feed delivering at all". An individual quiet leg is a
// fact to report, not an alarm — a deep out-of-the-money leg legitimately goes
// a while without a book event, so per-leg age is counted and printed rather
// than used to condemn the whole ladder.
inline constexpr qint64 kCockpitBooksStaleMs = 30'000;

// ── Inputs ─────────────────────────────────────────────────────────────────

struct AutoCockpitInputs {
    bool has_selection = false;      // a contract (and therefore an event) is selected
    QString event_ticker;            // the event whose ladder is being priced
    int markets_total = 0;           // all_markets_.size()
    qint64 markets_listed_at_ms = 0; // when populate_markets last ran; 0 = never
    int legs_total = 0;              // surface points built for this event
    int legs_with_book = 0;          // points with quote_observed_at_ms > 0
    // Among legs with a book. Both 0 when legs_with_book == 0.
    qint64 newest_leg_quote_ms = 0;
    qint64 oldest_leg_quote_ms = 0;
    // Legs whose own quote is older than kCockpitBooksStaleMs. Reported, not
    // used to classify — see the note on the constant.
    int legs_quote_past_bound = 0;
};

struct AutoCockpitView {
    QString state;       // "live" | "stale" | "absent"
    QString color_role;  // "green" | "amber" | "grey"
    QString headline;    // what the cockpit is and whether its inputs are usable
    QString markets_line;
    QString books_line;
    // False for every state but "live". The ladder table must then be cleared
    // to `ladder_notice` rather than left showing its last-good rows.
    bool ladder_trustworthy = false;
    QString ladder_notice;
};

inline QString auto_cockpit_state_color_role(const QString& state) {
    if (state == QStringLiteral("live")) return QStringLiteral("green");
    if (state == QStringLiteral("stale")) return QStringLiteral("amber");
    return QStringLiteral("grey");
}

inline QString auto_cockpit_age_text(qint64 age_ms) {
    if (age_ms < 0) return QStringLiteral("clock skew");
    if (age_ms < 120'000) return QStringLiteral("%1s ago").arg(age_ms / 1'000);
    if (age_ms < 7'200'000) return QStringLiteral("%1m ago").arg(age_ms / 60'000);
    return QStringLiteral("%1h ago").arg(age_ms / 3'600'000);
}

namespace auto_cockpit_detail {

/// A timestamp is stale when it is missing, older than `bound`, or in the
/// future. A future stamp is a broken clock, not freshness.
inline bool timestamp_stale(qint64 observed_at_ms, qint64 now_ms, qint64 bound_ms) {
    if (observed_at_ms <= 0) return true;
    const qint64 age = now_ms - observed_at_ms;
    return age < 0 || age > bound_ms;
}

inline bool markets_absent(const AutoCockpitInputs& inputs) {
    return inputs.markets_listed_at_ms <= 0 || inputs.markets_total <= 0;
}

/// Absent when no leg has a book at all — quote_observed_at_ms is 0 exactly
/// when KalshiAutoEngine::quote_for found no book and fell back to the cached
/// market snapshot, because both book producers always stamp a non-zero time.
inline bool books_absent(const AutoCockpitInputs& inputs) {
    return inputs.legs_with_book <= 0 || inputs.newest_leg_quote_ms <= 0;
}

inline QString markets_line(const AutoCockpitInputs& inputs, qint64 now_ms) {
    if (markets_absent(inputs))
        return QStringLiteral("MARKETS NOT LISTED YET · the contract list has never arrived");
    const bool stale = timestamp_stale(inputs.markets_listed_at_ms, now_ms, kCockpitMarketsStaleMs);
    QString line = QStringLiteral("MARKETS %1 · %2 listed · %3")
        .arg(stale ? QStringLiteral("STALE") : QStringLiteral("LIVE"))
        .arg(inputs.markets_total)
        .arg(auto_cockpit_age_text(now_ms - inputs.markets_listed_at_ms));
    if (stale)
        line += QStringLiteral(" · older than %1s, so it may no longer hold the open contract")
                    .arg(kCockpitMarketsStaleMs / 1'000);
    return line;
}

inline QString books_line(const AutoCockpitInputs& inputs, qint64 now_ms) {
    if (!inputs.has_selection)
        return QStringLiteral("BOOKS — no contract selected, so no ladder is being priced");
    if (inputs.legs_total <= 0)
        return QStringLiteral("BOOKS — the engine has produced no ladder legs for this event");
    if (books_absent(inputs))
        return QStringLiteral(
                   "BOOKS ABSENT · 0/%1 legs quoted — every leg is priced from the cached market "
                   "snapshot, not from a live book")
            .arg(inputs.legs_total);

    const bool stale = timestamp_stale(inputs.newest_leg_quote_ms, now_ms, kCockpitBooksStaleMs);
    QString line = QStringLiteral("BOOKS %1 · %2/%3 legs quoted · newest %4 · oldest %5")
        .arg(stale ? QStringLiteral("STALE") : QStringLiteral("LIVE"))
        .arg(inputs.legs_with_book)
        .arg(inputs.legs_total)
        .arg(auto_cockpit_age_text(now_ms - inputs.newest_leg_quote_ms),
             auto_cockpit_age_text(now_ms - inputs.oldest_leg_quote_ms));
    // Named, never averaged away: a leg with no book still gets a price and a
    // row, and the operator is entitled to know how many.
    const int without_book = inputs.legs_total > inputs.legs_with_book
        ? inputs.legs_total - inputs.legs_with_book : 0;
    if (without_book > 0)
        line += QStringLiteral(" · %1 priced from the cached market snapshot (no book)")
                    .arg(without_book);
    if (inputs.legs_quote_past_bound > 0)
        line += QStringLiteral(" · %1 quoted longer than %2s ago")
                    .arg(inputs.legs_quote_past_bound).arg(kCockpitBooksStaleMs / 1'000);
    return line;
}

} // namespace auto_cockpit_detail

/// The one classifier. Everything the header shows is derived here so a test
/// can hold the whole rendering to account without a running screen.
inline AutoCockpitView present_auto_cockpit(const AutoCockpitInputs& inputs, qint64 now_ms) {
    using namespace auto_cockpit_detail;
    AutoCockpitView view;
    view.markets_line = markets_line(inputs, now_ms);
    view.books_line = books_line(inputs, now_ms);

    const bool no_markets = markets_absent(inputs);
    const bool markets_stale =
        timestamp_stale(inputs.markets_listed_at_ms, now_ms, kCockpitMarketsStaleMs);
    const bool no_books = books_absent(inputs);
    const bool books_stale =
        timestamp_stale(inputs.newest_leg_quote_ms, now_ms, kCockpitBooksStaleMs);

    // ── STATE ──────────────────────────────────────────────────────────────
    // An input that has never arrived is absent; one that arrived and went old
    // is stale. Absent outranks stale: there is nothing to be stale about.
    QStringList reasons;
    if (!inputs.has_selection) {
        view.state = QStringLiteral("absent");
        reasons << QStringLiteral("no contract is selected");
    } else if (no_markets || inputs.legs_total <= 0 || no_books) {
        view.state = QStringLiteral("absent");
        if (no_markets) reasons << QStringLiteral("no contract list has arrived");
        if (inputs.legs_total <= 0) reasons << QStringLiteral("the engine produced no ladder legs");
        else if (no_books) reasons << QStringLiteral("no leg has a live book");
    } else if (markets_stale || books_stale) {
        view.state = QStringLiteral("stale");
        if (markets_stale)
            reasons << QStringLiteral("the contract list is %1")
                           .arg(auto_cockpit_age_text(now_ms - inputs.markets_listed_at_ms));
        if (books_stale)
            reasons << QStringLiteral("the newest leg book is %1")
                           .arg(auto_cockpit_age_text(now_ms - inputs.newest_leg_quote_ms));
    } else {
        view.state = QStringLiteral("live");
    }
    view.color_role = auto_cockpit_state_color_role(view.state);
    view.ladder_trustworthy = view.state == QStringLiteral("live");

    const QString event = inputs.event_ticker.trimmed().isEmpty()
        ? QStringLiteral("no event") : inputs.event_ticker.trimmed();
    if (view.ladder_trustworthy) {
        view.headline = QStringLiteral("AUTO COCKPIT LIVE · %1 · inputs fresh").arg(event);
    } else {
        view.headline = QStringLiteral("AUTO COCKPIT %1 · %2 · %3")
            .arg(view.state.toUpper(), event, reasons.join(QStringLiteral(" · ")));
        view.ladder_notice = QStringLiteral("LADDER NOT SHOWN — %1. The plan is computed from these "
                                            "inputs; it is withheld rather than shown stale.")
                                 .arg(reasons.join(QStringLiteral("; ")));
    }
    return view;
}

} // namespace openmarketterminal::screens::kalshi
