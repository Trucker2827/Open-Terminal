// tst_sandbox_maker_quotes.cpp — pure resting-price geometry for the maker
// spread producer. No sockets, no DB.
#include "services/sandbox/MakerQuotes.h"

#include <QtTest/QtTest>

using namespace openmarketterminal::services::sandbox;

class TstSandboxMakerQuotes : public QObject {
    Q_OBJECT
  private slots:
    void builds_symmetric_bid_and_ask_around_mid() {
        const MakerQuotePair p = build_maker_quotes(100.0, 2.0);
        QVERIFY(p.valid);
        QCOMPARE(p.bid.side, QStringLiteral("buy"));
        QCOMPARE(p.ask.side, QStringLiteral("sell"));
        QVERIFY(qAbs(p.bid.limit_price - 100.0 * (1.0 - 2.0 / 1e4)) < 1e-9); // 99.98
        QVERIFY(qAbs(p.ask.limit_price - 100.0 * (1.0 + 2.0 / 1e4)) < 1e-9); // 100.02
        QVERIFY(p.bid.limit_price < 100.0);
        QVERIFY(p.ask.limit_price > 100.0);
    }

    void non_positive_mid_is_invalid() {
        QVERIFY(!build_maker_quotes(0.0, 2.0).valid);
        QVERIFY(!build_maker_quotes(-5.0, 2.0).valid);
    }
};

QTEST_GUILESS_MAIN(TstSandboxMakerQuotes)
#include "tst_sandbox_maker_quotes.moc"
