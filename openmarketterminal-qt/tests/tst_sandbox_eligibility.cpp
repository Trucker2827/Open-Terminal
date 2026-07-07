// tst_sandbox_eligibility.cpp — SandboxEligibility::evaluate_eligibility
// (Task 10): the spec-§4.7 boundary matrix for the report-only Live
// Eligibility Gate. evaluate_eligibility is a pure function over a plain
// EligibilityInput struct -- no Database, no Qt-global state -- so every
// test here constructs an input by hand and asserts on the returned
// EligibilityVerdict. No DB bring-up, unlike this file's sandbox siblings.

#include <QtTest>

#include "services/sandbox/SandboxEligibility.h"

using namespace openmarketterminal::services::sandbox;

namespace {

// A hand-built input that clears every bar with headroom: 30 active days
// over the 28 minimum, 40 resolved over the 30 minimum, a real positive
// net_pnl, a 1% drawdown fraction (well under the 10% cap), and a 0%
// degraded share (well under the 10% cap). Every boundary test below starts
// from this and moves exactly ONE field to its boundary value so a failure
// pinpoints exactly which bar broke.
EligibilityInput passing_input() {
    EligibilityInput in;
    in.active_days = 30;
    in.resolved = 40;
    in.degraded = 0;
    in.total_positions = 40;
    in.net_pnl = 10.0;
    in.max_drawdown = 1.0;
    in.gross_notional = 100.0; // drawdown_frac = 0.01
    in.hypothetical = false;
    return in;
}

} // namespace

class TstSandboxEligibility : public QObject {
    Q_OBJECT
  private slots:
    void baseline_all_pass_is_eligible_with_empty_blockers() {
        const auto v = evaluate_eligibility(passing_input());
        QVERIFY(v.eligible);
        QVERIFY(v.blockers.isEmpty());
    }

    // --- resolved: 29 vs 30 (kMinResolvedSample) -----------------------------
    void resolved_29_is_blocked() {
        auto in = passing_input();
        in.resolved = 29;
        in.total_positions = 29;
        const auto v = evaluate_eligibility(in);
        QVERIFY(!v.eligible);
        bool found = false;
        for (const auto& b : v.blockers)
            if (b.contains(QStringLiteral("resolved sample")))
                found = true;
        QVERIFY2(found, qUtf8Printable(v.blockers.join(", ")));
    }

    void resolved_30_is_not_blocked_on_this_bar() {
        auto in = passing_input();
        in.resolved = 30;
        in.total_positions = 30;
        const auto v = evaluate_eligibility(in);
        QVERIFY2(v.eligible, qUtf8Printable(v.blockers.join(", ")));
    }

    // --- active_days: 27 vs 28 (kMinActiveDays) ------------------------------
    void active_days_27_is_blocked() {
        auto in = passing_input();
        in.active_days = 27;
        const auto v = evaluate_eligibility(in);
        QVERIFY(!v.eligible);
        bool found = false;
        for (const auto& b : v.blockers)
            if (b.contains(QStringLiteral("active days")))
                found = true;
        QVERIFY2(found, qUtf8Printable(v.blockers.join(", ")));
    }

    void active_days_28_is_not_blocked_on_this_bar() {
        auto in = passing_input();
        in.active_days = 28;
        const auto v = evaluate_eligibility(in);
        QVERIFY2(v.eligible, qUtf8Printable(v.blockers.join(", ")));
    }

    // --- net_pnl: bar is strictly `> 0` --------------------------------------
    void net_pnl_exactly_zero_is_blocked() {
        auto in = passing_input();
        in.net_pnl = 0.0;
        const auto v = evaluate_eligibility(in);
        QVERIFY(!v.eligible);
        bool found = false;
        for (const auto& b : v.blockers)
            if (b.contains(QStringLiteral("demonstrated edge")))
                found = true;
        QVERIFY2(found, qUtf8Printable(v.blockers.join(", ")));
    }

    void net_pnl_barely_positive_is_not_blocked_on_this_bar() {
        auto in = passing_input();
        in.net_pnl = 0.01;
        const auto v = evaluate_eligibility(in);
        QVERIFY2(v.eligible, qUtf8Printable(v.blockers.join(", ")));
    }

    // --- drawdown: bar is `<=` kMaxDrawdownFrac (10%) ------------------------
    void drawdown_exactly_at_cap_is_allowed() {
        auto in = passing_input();
        in.max_drawdown = 10.0;
        in.gross_notional = 100.0; // frac == 0.10 exactly
        const auto v = evaluate_eligibility(in);
        QVERIFY2(v.eligible, qUtf8Printable(v.blockers.join(", ")));
    }

    void drawdown_just_over_cap_is_blocked() {
        auto in = passing_input();
        in.max_drawdown = 10.01;
        in.gross_notional = 100.0; // frac == 0.1001
        const auto v = evaluate_eligibility(in);
        QVERIFY(!v.eligible);
        bool found = false;
        for (const auto& b : v.blockers)
            if (b.contains(QStringLiteral("drawdown")))
                found = true;
        QVERIFY2(found, qUtf8Printable(v.blockers.join(", ")));
    }

    void zero_gross_notional_does_not_divide_by_zero_on_drawdown_bar() {
        auto in = passing_input();
        in.max_drawdown = 0.0;
        in.gross_notional = 0.0;
        const auto v = evaluate_eligibility(in);
        // gross_notional <= 0 is treated as a pass on the drawdown bar
        // specifically -- no NaN/inf blocker text should appear.
        for (const auto& b : v.blockers)
            QVERIFY2(!b.contains(QStringLiteral("drawdown")), qUtf8Printable(b));
    }

    // --- degraded share: bar is `< kMaxDegradedShare` (10%) ------------------
    void degraded_share_exactly_10_percent_is_blocked() {
        auto in = passing_input();
        in.degraded = 10;
        in.total_positions = 100;
        const auto v = evaluate_eligibility(in);
        QVERIFY(!v.eligible);
        bool found = false;
        for (const auto& b : v.blockers)
            if (b.contains(QStringLiteral("degraded share")))
                found = true;
        QVERIFY2(found, qUtf8Printable(v.blockers.join(", ")));
    }

    void degraded_share_just_under_10_percent_is_not_blocked_on_this_bar() {
        auto in = passing_input();
        in.degraded = 9;
        in.total_positions = 100;
        const auto v = evaluate_eligibility(in);
        QVERIFY2(v.eligible, qUtf8Printable(v.blockers.join(", ")));
    }

    // 'unknown' data_quality positions are NOT degraded (T5-review decision)
    // and so never inflate the numerator here -- but they DO still count in
    // total_positions like any other position. A book with a large
    // 'unknown' population and zero 'degraded' positions must clear this
    // bar regardless of how large that unknown population is.
    void many_unknown_positions_with_zero_degraded_does_not_block() {
        auto in = passing_input();
        in.degraded = 0;
        in.total_positions = 500; // resolved=40 + a large unknown/open/unfilled population.
        const auto v = evaluate_eligibility(in);
        QVERIFY2(v.eligible, qUtf8Printable(v.blockers.join(", ")));
    }

    // --- hypothetical: ALWAYS blocked, unconditionally, before any other bar -
    void hypothetical_is_always_blocked_even_when_otherwise_perfect() {
        auto in = passing_input();
        in.hypothetical = true;
        const auto v = evaluate_eligibility(in);
        QVERIFY(!v.eligible);
        QCOMPARE(v.blockers.size(), 1);
        QCOMPARE(v.blockers.first(), QStringLiteral("hypothetical instrument"));
    }

    void hypothetical_short_circuits_before_any_other_bar_is_even_checked() {
        // Every other bar fails too (0 active days, 0 resolved, 0 net_pnl,
        // huge degraded share) -- the verdict must still carry exactly the
        // one "hypothetical instrument" blocker, not a pile-up of every
        // other failed bar as well.
        EligibilityInput in; // all zero-valued fields.
        in.hypothetical = true;
        const auto v = evaluate_eligibility(in);
        QVERIFY(!v.eligible);
        QCOMPARE(v.blockers.size(), 1);
        QCOMPARE(v.blockers.first(), QStringLiteral("hypothetical instrument"));
    }

    // A book failing every non-hypothetical bar at once must report ALL of
    // the corresponding blockers, not just the first one hit.
    void multiple_failed_bars_all_appear_in_blockers() {
        EligibilityInput in; // active_days=0, resolved=0, degraded=0, total_positions=0,
                             // net_pnl=0, max_drawdown=0, gross_notional=0, hypothetical=false.
        const auto v = evaluate_eligibility(in);
        QVERIFY(!v.eligible);
        bool has_days = false, has_resolved = false, has_pnl = false;
        for (const auto& b : v.blockers) {
            if (b.contains(QStringLiteral("active days")))
                has_days = true;
            if (b.contains(QStringLiteral("resolved sample")))
                has_resolved = true;
            if (b.contains(QStringLiteral("demonstrated edge")))
                has_pnl = true;
        }
        QVERIFY2(has_days, qUtf8Printable(v.blockers.join(", ")));
        QVERIFY2(has_resolved, qUtf8Printable(v.blockers.join(", ")));
        QVERIFY2(has_pnl, qUtf8Printable(v.blockers.join(", ")));
    }
};

QTEST_GUILESS_MAIN(TstSandboxEligibility)
#include "tst_sandbox_eligibility.moc"
