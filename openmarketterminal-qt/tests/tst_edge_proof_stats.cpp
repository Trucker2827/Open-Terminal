#include <QtTest>
#include "services/edge_radar/EdgeProofStats.h"

using namespace openmarketterminal::services::edge_radar;

// The shared paper-proof scoreboard (issue #95): sample tiers, verdict
// thresholds, the per-row fold and the persisted-move parser used by BOTH the
// CLI `edge journal proof-loop` command and the GUI SPOT/SCALP cockpit.
// Every threshold below is asserted at its exact boundary on purpose: moving
// a threshold in EdgeProofStats.cpp must break this test (neuter check).
class TstEdgeProofStats : public QObject {
    Q_OBJECT

    static EdgeProofStats stats(int resolved, int buy_resolved, int buy_wins, double paper_pnl,
                                int no_trade_resolved, int no_trade_correct) {
        EdgeProofStats s;
        s.resolved = resolved;
        s.buy_resolved = buy_resolved;
        s.buy_wins = buy_wins;
        s.paper_pnl = paper_pnl;
        s.no_trade_resolved = no_trade_resolved;
        s.no_trade_correct = no_trade_correct;
        return s;
    }

private slots:
    void rate_guards_zero_denominator() {
        QCOMPARE(edge_proof_rate(3, 0), 0.0);
        QVERIFY(qAbs(edge_proof_rate(3, 4) - 0.75) < 1e-12);
    }

    void sample_status_tier_boundaries() {
        QCOMPARE(edge_proof_sample_status(0), QString("warmup"));
        QCOMPARE(edge_proof_sample_status(29), QString("warmup"));
        QCOMPARE(edge_proof_sample_status(30), QString("early-sample"));
        QCOMPARE(edge_proof_sample_status(99), QString("early-sample"));
        QCOMPARE(edge_proof_sample_status(100), QString("useful-sample"));
        QCOMPARE(edge_proof_sample_status(499), QString("useful-sample"));
        QCOMPARE(edge_proof_sample_status(500), QString("strong-sample"));
        QCOMPARE(edge_proof_sample_status(999), QString("strong-sample"));
        QCOMPARE(edge_proof_sample_status(1000), QString("institutional-sample"));
    }

    void next_milestone_ladder() {
        QCOMPARE(edge_proof_next_milestone(0), 30);
        QCOMPARE(edge_proof_next_milestone(29), 30);
        QCOMPARE(edge_proof_next_milestone(30), 100);
        QCOMPARE(edge_proof_next_milestone(100), 500);
        QCOMPARE(edge_proof_next_milestone(500), 1000);
        QCOMPARE(edge_proof_next_milestone(1000), 0);
    }

    void verdict_warmup_below_30_resolved() {
        QCOMPARE(edge_proof_verdict(stats(29, 20, 20, 100.0, 0, 0)), QString("WARMUP"));
        QCOMPARE(edge_proof_verdict(stats(30, 0, 0, 0.0, 0, 0)), QString("MIXED"));
    }

    void verdict_buy_edge_proving_needs_pnl_and_55pct() {
        // 10 buys resolved, positive pnl, exactly 55% profitable → PROVING.
        QCOMPARE(edge_proof_verdict(stats(40, 20, 11, 1.0, 0, 0)), QString("BUY EDGE PROVING"));
        // Same but rate just below 0.55 (10/20=0.50 is BUY WEAK's edge; use 54%).
        QCOMPARE(edge_proof_verdict(stats(100, 50, 27, 1.0, 0, 0)), QString("MIXED"));
        // Rate fine but pnl not positive → not proving (and ≤0 pnl → BUY WEAK).
        QCOMPARE(edge_proof_verdict(stats(40, 20, 12, 0.0, 0, 0)), QString("BUY WEAK"));
        // Fewer than 10 resolved buys can never prove.
        QCOMPARE(edge_proof_verdict(stats(40, 9, 9, 5.0, 0, 0)), QString("MIXED"));
    }

    void verdict_buy_weak_on_losses_or_sub_50pct() {
        QCOMPARE(edge_proof_verdict(stats(40, 10, 4, 1.0, 0, 0)), QString("BUY WEAK"));
        QCOMPARE(edge_proof_verdict(stats(40, 10, 5, 1.0, 0, 0)), QString("MIXED"));
        QCOMPARE(edge_proof_verdict(stats(40, 10, 9, -0.01, 0, 0)), QString("BUY WEAK"));
    }

    void verdict_no_trade_tiers_need_65pct_of_20() {
        // 20 no-trades resolved at exactly 65%, almost no buys → AVOID WEAK TRADES.
        QCOMPARE(edge_proof_verdict(stats(40, 4, 4, 1.0, 20, 13)), QString("AVOID WEAK TRADES"));
        // Same but 5+ resolved buys → NO-TRADE EDGE.
        QCOMPARE(edge_proof_verdict(stats(40, 5, 3, 1.0, 20, 13)), QString("NO-TRADE EDGE"));
        // 64% no-trade success does not qualify.
        QCOMPARE(edge_proof_verdict(stats(60, 0, 0, 0.0, 50, 32)), QString("MIXED"));
        // 19 resolved no-trades do not qualify regardless of rate.
        QCOMPARE(edge_proof_verdict(stats(40, 0, 0, 0.0, 19, 19)), QString("MIXED"));
    }

    void accumulate_folds_rows_like_proof_loop() {
        EdgeProofStats s;
        // Unresolved buy signal → waiting only.
        QCOMPARE(edge_proof_accumulate(s, true, EdgeProofRowOutcome{}, 100.0), 0.0);
        // Winning buy: move 0.02 over breakeven 0.005 at $100 → +$1.50.
        const double win = edge_proof_accumulate(s, true, EdgeProofRowOutcome{true, 1, 0.02, 0.005}, 100.0);
        QVERIFY(qAbs(win - 1.5) < 1e-12);
        // Losing buy: move below breakeven → -$0.30, not a buy win.
        const double loss = edge_proof_accumulate(s, true, EdgeProofRowOutcome{true, 0, 0.002, 0.005}, 100.0);
        QVERIFY(qAbs(loss + 0.3) < 1e-12);
        // Correct no-trade: avoided value = $100 * (breakeven - move).
        QCOMPARE(edge_proof_accumulate(s, false, EdgeProofRowOutcome{true, 1, -0.001, 0.004}, 100.0), 0.0);
        // Incorrect no-trade adds nothing to avoided value.
        QCOMPARE(edge_proof_accumulate(s, false, EdgeProofRowOutcome{true, 0, 0.02, 0.004}, 100.0), 0.0);

        QCOMPARE(s.signal_count, 5);
        QCOMPARE(s.waiting, 1);
        QCOMPARE(s.resolved, 4);
        QCOMPARE(s.wins, 2);
        QCOMPARE(s.buy_signals, 3);
        QCOMPARE(s.buy_resolved, 2);
        QCOMPARE(s.buy_wins, 1);
        QCOMPARE(s.no_trade_signals, 2);
        QCOMPARE(s.no_trade_resolved, 2);
        QCOMPARE(s.no_trade_correct, 1);
        QVERIFY(qAbs(s.paper_pnl - 1.2) < 1e-12);
        QVERIFY(qAbs(s.avoided_value - 0.5) < 1e-12);
    }

    void buy_call_classification() {
        QVERIFY(edge_proof_is_buy_call(QStringLiteral("BUY CANDIDATE"), QStringLiteral("buy")));
        QVERIFY(!edge_proof_is_buy_call(QStringLiteral("NO TRADE"), QStringLiteral("buy")));
        QVERIFY(!edge_proof_is_buy_call(QStringLiteral("buy candidate"), QStringLiteral("buy")));
    }

    void parse_scored_move_takes_last_persisted_value() {
        double move = -1.0;
        // Two scoring passes appended (the rescore case seen in the live DB):
        // the LAST one is the latest.
        const QString reasons = QStringLiteral(
            "edge below gate | scored: future=65065.37000000 move=0.00015414 breakeven=0.01263012"
            " | scored: future=65100.00000000 move=-0.00021000 breakeven=0.01263012");
        QVERIFY(edge_proof_parse_scored_move(reasons, &move));
        QVERIFY(qAbs(move - (-0.00021000)) < 1e-12);
    }

    void parse_scored_move_missing_reads_missing() {
        double move = 123.0;
        QVERIFY(!edge_proof_parse_scored_move(QStringLiteral("manually resolved by operator"), &move));
        QCOMPARE(move, 123.0);
        QVERIFY(!edge_proof_parse_scored_move(QString(), nullptr));
    }
};

QTEST_MAIN(TstEdgeProofStats)
#include "tst_edge_proof_stats.moc"
