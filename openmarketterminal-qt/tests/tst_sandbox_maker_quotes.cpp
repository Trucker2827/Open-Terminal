// tst_sandbox_maker_quotes.cpp — pure resting-price geometry for the maker
// spread producer. No sockets, no DB.
#include "services/sandbox/MakerQuotes.h"

#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

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

    void append_writes_bid_and_ask_rows() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("maker_decisions.jsonl");
        append_maker_decisions(path, QStringLiteral("BTC-USD"), QStringLiteral("coinbase_advanced"),
                               100.0, 2.0, 120.0, 3, 1234567);

        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QList<QByteArray> lines = f.readAll().split('\n');
        QStringList rows;
        for (const QByteArray& l : lines)
            if (!l.trimmed().isEmpty())
                rows << QString::fromUtf8(l);
        QCOMPARE(rows.size(), 2);

        const QJsonObject bid = QJsonDocument::fromJson(rows[0].toUtf8()).object();
        const QJsonObject ask = QJsonDocument::fromJson(rows[1].toUtf8()).object();
        QCOMPARE(bid.value("side").toString(), QStringLiteral("buy"));
        QCOMPARE(ask.value("side").toString(), QStringLiteral("sell"));
        QCOMPARE(bid.value("venue").toString(), QStringLiteral("coinbase_advanced"));
        QCOMPARE(bid.value("action").toString(), QStringLiteral("PAPER_MAKER_QUOTE"));
        QCOMPARE(bid.value("liquidity").toString(), QStringLiteral("maker"));
        QCOMPARE(bid.value("ts_ms").toString(), QStringLiteral("1234567"));
        QVERIFY(qAbs(bid.value("reference_price").toDouble() - 99.98) < 1e-9);
        QVERIFY(qAbs(ask.value("reference_price").toDouble() - 100.02) < 1e-9);
        QCOMPARE(bid.value("live_sources").toInt(), 3);
    }

    void append_skips_non_positive_mid() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("maker_decisions.jsonl");
        append_maker_decisions(path, QStringLiteral("BTC-USD"), QStringLiteral("coinbase_advanced"),
                               0.0, 2.0, 120.0, 3, 1234567);
        QFile f(path);
        // Either no file, or an empty one -- no rows written.
        const bool empty = !f.exists() || (f.open(QIODevice::ReadOnly) && f.readAll().trimmed().isEmpty());
        QVERIFY(empty);
    }
};

QTEST_GUILESS_MAIN(TstSandboxMakerQuotes)
#include "tst_sandbox_maker_quotes.moc"
