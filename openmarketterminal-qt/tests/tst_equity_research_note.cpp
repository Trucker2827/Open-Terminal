// tests/tst_equity_research_note.cpp
//
// Issue #103 — the Equity Research NOTE action's composition helper
// (screens/equity_research/EquityResearchNote.h). The note must carry the
// symbol and date in its title, the symbol in its tickers field, and — only
// when a quote actually loaded — a snapshot line with the displayed price and
// change. Honesty rule under test: no quote → no snapshot line at all, never
// a zero-filled one.
#include "screens/equity_research/EquityResearchNote.h"

#include <QtTest/QtTest>

using namespace openmarketterminal::screens::equity_note;

class TstEquityResearchNote : public QObject {
    Q_OBJECT

  private slots:
    void loaded_quote_body_has_symbol_and_price() {
        ResearchNoteQuote q;
        q.loaded = true;
        q.price = 214.15;
        q.change = 1.23;
        q.change_pct = 0.58;
        q.currency_symbol = "$";

        const auto d = compose_research_note("AAPL", q, QDate(2026, 7, 23));

        QVERIFY(d.content.contains("AAPL"));
        QVERIFY(d.content.contains("$214.15"));
        QVERIFY(d.content.contains("+1.23"));
        QVERIFY(d.content.contains("(+0.58%)"));
        QVERIFY(d.word_count > 0);
    }

    void no_quote_body_has_no_snapshot_line() {
        ResearchNoteQuote q; // loaded == false, values at defaults
        const auto d = compose_research_note("AAPL", q, QDate(2026, 7, 23));

        // Honesty: the body is empty — no zero-filled price/change line.
        QVERIFY(d.content.isEmpty());
        QCOMPARE(d.word_count, 0);
        // Title and tickers are still set so the note remains findable.
        QVERIFY(d.title.contains("AAPL"));
        QCOMPARE(d.tickers, QString("AAPL"));
    }

    void unloaded_values_never_leak_even_if_nonzero() {
        // A stale quote struct with numbers in it but loaded == false must
        // still produce no snapshot line — `loaded` is the only gate.
        ResearchNoteQuote q;
        q.loaded = false;
        q.price = 99.0;
        q.change = -1.0;
        q.change_pct = -1.0;
        q.currency_symbol = "$";

        const auto d = compose_research_note("MSFT", q, QDate(2026, 7, 23));
        QVERIFY(d.content.isEmpty());
    }

    void title_names_symbol_and_date() {
        ResearchNoteQuote q;
        const auto d = compose_research_note("SAP.DE", q, QDate(2026, 7, 23));
        QVERIFY(d.title.contains("SAP.DE"));
        QVERIFY(d.title.contains("2026-07-23"));
        QCOMPARE(d.tickers, QString("SAP.DE"));
    }

    void negative_change_is_signed_correctly() {
        ResearchNoteQuote q;
        q.loaded = true;
        q.price = 100.50;
        q.change = -2.35;
        q.change_pct = -2.28;
        q.currency_symbol = "$";

        const auto d = compose_research_note("TSLA", q, QDate(2026, 7, 23));
        QVERIFY(d.content.contains("-2.35"));
        QVERIFY(d.content.contains("(-2.28%)"));
        QVERIFY(!d.content.contains("+-"));
    }

    void symbol_is_trimmed_and_uppercased() {
        ResearchNoteQuote q;
        const auto d = compose_research_note(" aapl ", q, QDate(2026, 7, 23));
        QCOMPARE(d.tickers, QString("AAPL"));
        QVERIFY(d.title.startsWith("AAPL"));
    }
};

QTEST_APPLESS_MAIN(TstEquityResearchNote)
#include "tst_equity_research_note.moc"
