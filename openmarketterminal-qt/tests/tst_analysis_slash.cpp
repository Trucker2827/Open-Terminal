#include <QtTest>

#include "screens/ai_chat/AnalysisSlashCommands.h"

using openmarketterminal::ai_chat::expand_analysis_slash_command;

class TstAnalysisSlash : public QObject {
    Q_OBJECT
private slots:
    void comps_with_ticker_expands() {
        QString usage;
        const QString out = expand_analysis_slash_command("/comps AAPL", &usage);
        QVERIFY(!out.isEmpty());
        QVERIFY(usage.isEmpty());
        QVERIFY(out.contains("comparable"));
        QVERIFY(out.contains("AAPL"));
    }

    void dcf_and_earnings_expand() {
        QString u;
        QVERIFY(expand_analysis_slash_command("/dcf MSFT", &u).contains("discounted"));
        QVERIFY(expand_analysis_slash_command("/dcf MSFT", &u).contains("MSFT"));
        QVERIFY(expand_analysis_slash_command("/earnings TSLA", &u).contains("earnings"));
    }

    void lbo_and_three_statement_expand() {
        QString u;
        QVERIFY(expand_analysis_slash_command("/lbo KKR", &u).contains("LBO"));
        QVERIFY(expand_analysis_slash_command("/lbo KKR", &u).contains("KKR"));
        const QString ts = expand_analysis_slash_command("/3statement NVDA", &u);
        QVERIFY(ts.contains("3-statement"));
        QVERIFY(ts.contains("NVDA"));
    }

    void ticker_is_uppercased() {
        QString u;
        QVERIFY(expand_analysis_slash_command("/comps aapl", &u).contains("AAPL"));
    }

    void known_command_without_arg_sets_usage() {
        QString usage;
        const QString out = expand_analysis_slash_command("/comps", &usage);
        QVERIFY(out.isEmpty());
        QCOMPARE(usage, QString("Usage: /comps TICKER"));
    }

    void unknown_slash_passes_through() {
        QString usage;
        QVERIFY(expand_analysis_slash_command("/foo BAR", &usage).isEmpty());
        QVERIFY(usage.isEmpty());   // not recognized → caller sends raw, no usage error
    }

    void plain_text_is_not_a_command() {
        QString usage;
        QVERIFY(expand_analysis_slash_command("what are AAPL comps", &usage).isEmpty());
        QVERIFY(usage.isEmpty());
    }
};

QTEST_MAIN(TstAnalysisSlash)
#include "tst_analysis_slash.moc"
