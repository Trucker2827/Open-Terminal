#include "services/news/BtcNewsPulse.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

using openmarketterminal::services::news::BtcNewsPulse;

class TstBtcNewsPulse : public QObject {
    Q_OBJECT
  private slots:
    void classifies_deduplicates_and_summarizes() {
        const qint64 now = 1'800'000'000'000LL;
        const QJsonArray articles{
            QJsonObject{{"id", "1"}, {"headline", "Bitcoin ETF sees record inflow as institutions accumulate"},
                        {"summary", "Institutional funds reported a record Bitcoin ETF inflow while large holders continued to accumulate BTC."},
                        {"source", "WIRE A"}, {"tier", 1}, {"impact", "HIGH"},
                        {"time", QDateTime::fromMSecsSinceEpoch(now - 60000, Qt::UTC).toString(Qt::ISODate)}},
            QJsonObject{{"id", "2"}, {"headline", "Institutions accumulate as Bitcoin ETF posts record inflow"},
                        {"summary", "Duplicate wording from another syndication endpoint."},
                        {"source", "WIRE A"}, {"tier", 1},
                        {"time", QDateTime::fromMSecsSinceEpoch(now - 60000, Qt::UTC).toString(Qt::ISODate)}},
            QJsonObject{{"id", "3"}, {"headline", "Bitcoin exchange suffers hack and stolen funds"},
                        {"summary", "A security incident prompted concern about near-term selling pressure."},
                        {"source", "WIRE B"}, {"tier", 2}, {"impact", "HIGH"},
                        {"time", QDateTime::fromMSecsSinceEpoch(now - 120000, Qt::UTC).toString(Qt::ISODate)}},
            QJsonObject{{"id", "4"}, {"headline", "Unrelated equity earnings report"},
                        {"summary", "No cryptocurrency content."}, {"source", "WIRE C"}},
        };
        const auto pulse = BtcNewsPulse::analyze(articles, now, 20);
        QCOMPARE(pulse.relevant, 2);
        QCOMPARE(pulse.duplicates_removed, 1);
        QCOMPARE(pulse.bullish, 1);
        QCOMPARE(pulse.bearish, 1);
        QCOMPARE(pulse.distinct_sources, 2);
        QCOMPARE(pulse.verdict, QStringLiteral("CONFLICTED"));
        QVERIFY(!pulse.stories.first().paragraph.contains(QLatin1Char('\n')));
        QVERIFY(!pulse.stories.first().paragraph.isEmpty());
    }

    void output_is_explicitly_advisory_only() {
        const auto pulse = BtcNewsPulse::analyze({}, 1'800'000'000'000LL, 20);
        const QJsonObject json = BtcNewsPulse::to_json(pulse);
        QCOMPARE(json.value("model_role").toString(), QStringLiteral("advisory_only"));
        QVERIFY(!json.value("can_trigger_order").toBool(true));
        QCOMPARE(json.value("verdict").toString(), QStringLiteral("NOT ENOUGH NEWS"));
    }

    void btc_specific_language_overrides_generic_article_tone() {
        const qint64 now = 1'800'000'000'000LL;
        const QJsonArray articles{
            QJsonObject{{"headline", "Company shares rise after selling Bitcoin treasury"},
                        {"summary", "The company sold Bitcoin to fund another project."},
                        {"source", "WIRE A"}, {"sentiment", "BULLISH"}, {"tier", 1}},
            QJsonObject{{"headline", "Bitcoin little changed during geopolitical tension"},
                        {"summary", "BTC remained flat through the session."},
                        {"source", "WIRE B"}, {"sentiment", "BULLISH"}, {"tier", 1}},
        };
        const auto pulse = BtcNewsPulse::analyze(articles, now, 20);
        QCOMPARE(pulse.bullish, 0);
        QCOMPARE(pulse.bearish, 1);
        QCOMPARE(pulse.neutral, 1);
        QVERIFY(pulse.stories.first().paragraph.contains(QStringLiteral("Market read:")));
    }
};

QTEST_APPLESS_MAIN(TstBtcNewsPulse)
#include "tst_btc_news_pulse.moc"
