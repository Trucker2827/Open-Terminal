#include <QtTest>

#include "screens/manual_trading/ManualTradingPnl.h"

using openmarketterminal::screens::manual_position_pnl;

// The Manual Trading (paper) screen's teaching claim, tested against the real
// P&L core: two SEPARATE paper accounts holding opposite outcomes (YES on one,
// NO on the other) for the SAME contract and size, marked to the SAME order
// book, net to roughly minus the total fees. Same book ⇒ yes+no mids are ~1
// and the price moves cancel; only the fees remain.
class TstManualTradingPnl : public QObject {
    Q_OBJECT
  private slots:
    // Single-position mark-to-mid: (mark - entry) * qty - fee.
    void single_position_marks_to_mid() {
        // Bought YES at 40c, mark now 55c, 10 contracts, 3c fee.
        // (0.55 - 0.40) * 10 - 0.03 = 1.50 - 0.03 = 1.47
        QCOMPARE(manual_position_pnl(0.40, 0.55, 10, 0.03), 1.47);
    }

    // A flat mark (mark == entry) leaves exactly minus the fee.
    void flat_mark_is_minus_fee() {
        QCOMPARE(manual_position_pnl(0.50, 0.50, 5, 0.02), -0.02);
    }

    // The core invariant the screen exists to teach.
    void opposite_yes_no_nets_to_minus_fees() {
        const int qty = 20;
        const double fee = 0.04;  // same fee on each account

        // Entry mids: YES 0.42, NO 0.58 (sum = 1.00, a coherent book).
        const double yes_entry = 0.42;
        const double no_entry = 0.58;

        // The book moves. New mids still sum to 1.00 (same order book).
        const double yes_mark = 0.63;
        const double no_mark = 0.37;

        const double kalshi_yes = manual_position_pnl(yes_entry, yes_mark, qty, fee);
        const double coinbase_no = manual_position_pnl(no_entry, no_mark, qty, fee);
        const double combined = kalshi_yes + coinbase_no;

        // Price moves cancel exactly; combined is exactly minus the two fees.
        QVERIFY(qFuzzyCompare(combined, -(2.0 * fee)));

        // And the two legs did NOT net away individually — one wins, one loses.
        QVERIFY(kalshi_yes > 0.0);
        QVERIFY(coinbase_no < 0.0);
    }

    // The invariant holds for ANY coherent book move, not just the one above.
    void invariant_holds_across_book_moves() {
        const int qty = 7;
        const double fee = 0.05;
        const double yes_entry = 0.30, no_entry = 0.70;  // sum 1.0
        for (double yes_mark = 0.05; yes_mark <= 0.95; yes_mark += 0.05) {
            const double no_mark = 1.0 - yes_mark;  // same book
            const double combined = manual_position_pnl(yes_entry, yes_mark, qty, fee) +
                                    manual_position_pnl(no_entry, no_mark, qty, fee);
            QVERIFY2(qFuzzyCompare(combined, -(2.0 * fee)),
                     qPrintable(QStringLiteral("combined=%1 at yes_mark=%2").arg(combined).arg(yes_mark)));
        }
    }
};

QTEST_APPLESS_MAIN(TstManualTradingPnl)
#include "tst_manual_trading_pnl.moc"
