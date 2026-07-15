#include <QtTest>
#include "services/ai_ledger/AiLedger.h"

using namespace openmarketterminal;
using ai_ledger::LedgerPosition;
using ai_ledger::FillDelta;
using ai_ledger::apply_fill;
using ai_ledger::unrealized_of;

class TstAiLedger : public QObject {
    Q_OBJECT
  private slots:
    void open_long_sets_avg_entry() {
        FillDelta d = apply_fill(LedgerPosition{}, "buy", 10.0, 100.0, 0.0);
        QCOMPARE(d.position.net_qty, 10.0);
        QCOMPARE(d.position.avg_entry_price, 100.0);
        QCOMPARE(d.position.realized_pnl, 0.0);
        QCOMPARE(d.realized_pnl_this_fill, 0.0);
    }

    void average_in_same_direction() {
        LedgerPosition p{10.0, 100.0, 0.0};
        FillDelta d = apply_fill(p, "buy", 10.0, 120.0, 0.0);
        QCOMPARE(d.position.net_qty, 20.0);
        QCOMPARE(d.position.avg_entry_price, 110.0);   // (100*10 + 120*10)/20
        QCOMPARE(d.position.realized_pnl, 0.0);
    }

    void partial_close_realizes_pnl() {
        LedgerPosition p{10.0, 100.0, 0.0};
        FillDelta d = apply_fill(p, "sell", 4.0, 130.0, 0.0);
        QCOMPARE(d.position.net_qty, 6.0);
        QCOMPARE(d.position.avg_entry_price, 100.0);   // unchanged on partial close
        QCOMPARE(d.realized_pnl_this_fill, 120.0);     // (130-100)*4
        QCOMPARE(d.position.realized_pnl, 120.0);
    }

    void full_close_flattens() {
        LedgerPosition p{10.0, 100.0, 0.0};
        FillDelta d = apply_fill(p, "sell", 10.0, 90.0, 0.0);
        QCOMPARE(d.position.net_qty, 0.0);
        QCOMPARE(d.position.avg_entry_price, 0.0);
        QCOMPARE(d.realized_pnl_this_fill, -100.0);    // (90-100)*10
    }

    void flip_long_to_short() {
        LedgerPosition p{10.0, 100.0, 0.0};
        FillDelta d = apply_fill(p, "sell", 15.0, 130.0, 0.0);
        QCOMPARE(d.position.net_qty, -5.0);            // 5 short remainder
        QCOMPARE(d.position.avg_entry_price, 130.0);   // new avg = fill price
        QCOMPARE(d.realized_pnl_this_fill, 300.0);     // (130-100)*10 closed
    }

    void short_open_then_close() {
        FillDelta open = apply_fill(LedgerPosition{}, "sell", 10.0, 100.0, 0.0);
        QCOMPARE(open.position.net_qty, -10.0);
        QCOMPARE(open.position.avg_entry_price, 100.0);
        FillDelta close = apply_fill(open.position, "buy", 4.0, 80.0, 0.0);
        QCOMPARE(close.position.net_qty, -6.0);
        QCOMPARE(close.realized_pnl_this_fill, 80.0);  // short: (100-80)*4
    }

    void fee_reduces_realized_on_close() {
        LedgerPosition p{10.0, 100.0, 0.0};
        FillDelta d = apply_fill(p, "sell", 4.0, 130.0, 5.0);
        QCOMPARE(d.realized_pnl_this_fill, 115.0);     // (130-100)*4 - 5
    }

    void unrealized_long_and_short() {
        QCOMPARE(unrealized_of(LedgerPosition{10.0, 100.0, 0.0}, 110.0), 100.0);   // (110-100)*10
        QCOMPARE(unrealized_of(LedgerPosition{-10.0, 100.0, 0.0}, 110.0), -100.0); // short loses as price rises
    }
};

QTEST_MAIN(TstAiLedger)
#include "tst_ai_ledger.moc"
