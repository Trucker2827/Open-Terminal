#include <QtTest>
#include <QFile>
#include "web/WebSearchParser.h"
using namespace openmarketterminal::web;

class TstWebSearchParser : public QObject {
    Q_OBJECT
private slots:
    void parses_fixture() {
        QFile f(QFINDTESTDATA("fixtures/ddg_results.html"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString html = QString::fromUtf8(f.readAll());
        const QVector<WebResult> rows = parse_ddg_results(html, 5);
        QVERIFY2(rows.size() >= 3, "expected at least 3 parsed results");
        QVERIFY(rows.size() <= 5);
        for (const auto& r : rows) {
            QVERIFY(!r.title.trimmed().isEmpty());
            QVERIFY(r.url.startsWith("http"));          // decoded, absolute
            QVERIFY(!r.url.contains("/l/?uddg="));        // DDG redirect decoded
        }
    }
    void empty_html_yields_empty() {
        QVERIFY(parse_ddg_results("", 5).isEmpty());
        QVERIFY(parse_ddg_results("<html><body>no results</body></html>", 5).isEmpty());
    }
    void respects_max_results() {
        QFile f(QFINDTESTDATA("fixtures/ddg_results.html"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(parse_ddg_results(QString::fromUtf8(f.readAll()), 2).size(), 2);
    }
    void entity_decode_no_remnants() {
        // Regression: strip_tags must decode HTML entities in titles without
        // leaving &amp; / &lt; / &gt; remnants or double-encoding them.
        const QString html =
            "<a class=\"result__a\" href=\"https://example.com\">"
            "News &amp; Tools &lt;x&gt;"
            "</a>";
        const auto rows = parse_ddg_results(html, 1);
        QVERIFY2(rows.size() == 1, "expected 1 result");
        QCOMPARE(rows[0].title, QString("News & Tools <x>"));
    }
};
QTEST_MAIN(TstWebSearchParser)
#include "tst_web_search_parser.moc"
