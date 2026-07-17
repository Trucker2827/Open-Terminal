#include <QtTest>
#include <QJsonObject>
#include "cli/ServeCommand.h"

using namespace openmarketterminal::cli;

// Phase 2 / Track 2 — daemon `paper-strategy` jobs must carry F5's risk-cap
// flags (--max-notional-per-order / --max-position-qty / --max-aggregate-qty)
// into the scheduled `strategy paper-run` subprocess, so an automated paper
// run is capped the same way a manual `ai run strategy` invocation is.
// command_for_job_kind() is the pure spec->args builder (ServeCommand.cpp);
// it is exercised directly here rather than via the daemon's jobs.json
// pipeline since it has no I/O of its own.
class TstDaemonJobCaps : public QObject {
    Q_OBJECT
private slots:
    // A spec with all three caps saved (as strings, matching how take_named()
    // persists CLI flag values into the job spec) must produce all three
    // flags, each immediately followed by its numeric value.
    void capped_spec_appends_all_three_flags() {
        const QJsonObject spec{{"strategy", "meanrev"},
                               {"symbols", "AAPL"},
                               {"max_iters", 1},
                               {"max_notional", "500"},
                               {"max_position", "7"},
                               {"max_aggregate", "5"}};
        const QStringList args = command_for_job_kind("paper-strategy", spec);

        const auto flag_value = [&](const QString& flag) -> QString {
            const int idx = args.indexOf(flag);
            if (idx < 0 || idx + 1 >= args.size())
                return QString();
            return args.at(idx + 1);
        };
        QCOMPARE(flag_value("--max-notional-per-order"), QStringLiteral("500"));
        QCOMPARE(flag_value("--max-position-qty"), QStringLiteral("7"));
        QCOMPARE(flag_value("--max-aggregate-qty"), QStringLiteral("5"));
    }

    // Default-off must be preserved: a spec with no cap keys at all must
    // carry none of the cap flags (only the pre-existing strategy/symbols/
    // max-iters/interval-sec args).
    void uncapped_spec_omits_all_cap_flags() {
        const QJsonObject spec{{"strategy", "meanrev"},
                               {"symbols", "AAPL"},
                               {"max_iters", 1}};
        const QStringList args = command_for_job_kind("paper-strategy", spec);

        QVERIFY(!args.contains(QStringLiteral("--max-notional-per-order")));
        QVERIFY(!args.contains(QStringLiteral("--max-position-qty")));
        QVERIFY(!args.contains(QStringLiteral("--max-aggregate-qty")));
    }

    // Non-numeric or non-positive cap values must not be appended: a bad
    // save (or an explicit zero/"off") should not silently turn into a cap.
    void non_numeric_or_zero_caps_are_omitted() {
        const QJsonObject spec{{"strategy", "meanrev"},
                               {"max_iters", 1},
                               {"max_notional", "not-a-number"},
                               {"max_position", "0"},
                               {"max_aggregate", "-3"}};
        const QStringList args = command_for_job_kind("paper-strategy", spec);

        QVERIFY(!args.contains(QStringLiteral("--max-notional-per-order")));
        QVERIFY(!args.contains(QStringLiteral("--max-position-qty")));
        QVERIFY(!args.contains(QStringLiteral("--max-aggregate-qty")));
    }
};
QTEST_MAIN(TstDaemonJobCaps)
#include "tst_daemon_job_caps.moc"
