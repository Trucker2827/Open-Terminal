#include <QtTest>
#include <QTemporaryDir>
#include "core/headless/HeadlessRuntime.h"
#include "storage/sqlite/Database.h"
#include "services/ai_ledger/Scorecard.h"

using namespace openmarketterminal;
using ai_ledger::Scorecard;
using ai_ledger::SymbolScore;
using ai_ledger::scorecard_of;

class TstAiScorecard : public QObject {
    Q_OBJECT
    QTemporaryDir home_;

    static bool seed(const QString& id, const QString& handler, const QString& symbol,
                     const QString& side, double qty, double price, double realized, qint64 ts) {
        return Database::instance().execute(
            "INSERT INTO ai_fill (id,handler,symbol,side,quantity,fill_price,fee,realized_pnl,ts,draft_id) "
            "VALUES (?,?,?,?,?,?,0,?,?,'d')",
            {id, handler, symbol, side, qty, price, realized, ts}).is_ok();
    }

  private slots:
    void initTestCase() {
        QVERIFY(home_.isValid());
        qputenv("HOME", home_.path().toUtf8());
        headless::HeadlessRuntime hr;
        headless::InitResult ir = hr.init(QString{});
        QVERIFY2(ir.ok, qUtf8Printable(ir.error));
        auto probe = Database::instance().execute("SELECT name FROM sqlite_master WHERE name='ai_fill'");
        QVERIFY(probe.is_ok());
        QVERIFY(probe.value().next());
    }

    void aggregates_realized_closes_only() {
        // 1 open (realized 0, excluded) + 4 closes (+100,+50,-30,-20).
        QVERIFY(seed("s1", "SC", "A-USD", "buy", 10, 100, 0.0, 1000));    // open, excluded
        QVERIFY(seed("s2", "SC", "A-USD", "sell", 2, 110, 100.0, 1001));  // close win
        QVERIFY(seed("s3", "SC", "A-USD", "sell", 2, 105, 50.0, 1002));   // close win
        QVERIFY(seed("s4", "SC", "A-USD", "sell", 2, 90, -30.0, 1003));   // close loss
        QVERIFY(seed("s5", "SC", "A-USD", "sell", 2, 92, -20.0, 1004));   // close loss

        Scorecard sc = scorecard_of("SC", "A-USD");
        QCOMPARE(sc.trades, 4);
        QCOMPARE(sc.wins, 2);
        QCOMPARE(sc.losses, 2);
        QCOMPARE(sc.hit_rate, 0.5);
        QCOMPARE(sc.realized_total, 100.0);   // 100+50-30-20
        QCOMPARE(sc.avg_realized, 25.0);      // 100/4
        QCOMPARE(sc.best, 100.0);
        QCOMPARE(sc.worst, -30.0);
        QVERIFY(sc.per_symbol.isEmpty());     // symbol filter set
    }

    void limit_windows_most_recent_closes() {
        QVERIFY(seed("w1", "WIN", "W-USD", "sell", 1, 10, 10.0, 2000));
        QVERIFY(seed("w2", "WIN", "W-USD", "sell", 1, 10, 20.0, 2001));
        QVERIFY(seed("w3", "WIN", "W-USD", "sell", 1, 10, 30.0, 2002));
        QVERIFY(seed("w4", "WIN", "W-USD", "sell", 1, 10, 40.0, 2003));
        QVERIFY(seed("w5", "WIN", "W-USD", "sell", 1, 10, 50.0, 2004));

        Scorecard sc = scorecard_of("WIN", "W-USD", 2);
        QCOMPARE(sc.trades, 2);
        QCOMPARE(sc.realized_total, 90.0);    // last two closes: 50 + 40
    }

    void per_symbol_breakdown_when_unfiltered() {
        QVERIFY(seed("p1", "PS", "AA-USD", "sell", 1, 10, 100.0, 3000));
        QVERIFY(seed("p2", "PS", "BB-USD", "sell", 1, 10, 40.0, 3001));
        QVERIFY(seed("p3", "PS", "BB-USD", "sell", 1, 10, -10.0, 3002));

        Scorecard sc = scorecard_of("PS");            // no symbol filter
        QCOMPARE(sc.trades, 3);
        QCOMPARE(sc.per_symbol.size(), 2);
        QCOMPARE(sc.per_symbol.at(0).symbol, QStringLiteral("AA-USD"));  // realized 100, sorted first
        QCOMPARE(sc.per_symbol.at(0).trades, 1);
        QCOMPARE(sc.per_symbol.at(1).symbol, QStringLiteral("BB-USD"));  // realized 30
        QCOMPARE(sc.per_symbol.at(1).trades, 2);
    }

    void empty_is_all_zero() {
        Scorecard sc = scorecard_of("nobody");
        QCOMPARE(sc.trades, 0);
        QCOMPARE(sc.wins, 0);
        QCOMPARE(sc.hit_rate, 0.0);
        QCOMPARE(sc.realized_total, 0.0);
        QVERIFY(sc.per_symbol.isEmpty());
    }
};

QTEST_MAIN(TstAiScorecard)
#include "tst_ai_scorecard.moc"
