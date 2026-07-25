// The Kalshi bot's observable-loop runtime (issue #129): the one classifier
// the GUI BOT chip and `kalshi bot status` both render, and the kill switch
// file they both write.
//
// What these tests defend: a killed bot must never read as running, a stop
// file that exists must engage the switch whatever its contents say, and the
// staleness threshold must come from one constant so the chip and the CLI
// cannot drift apart.

#include "services/prediction/kalshi/KalshiBotRuntime.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace openmarketterminal::services::prediction::kalshi_ns;

namespace {

constexpr qint64 kNow = 1'784'900'000'000LL;

KalshiBotStopFile engaged(qint64 ts_ms = kNow - 30'000,
                          const QString& source = QStringLiteral("cli"),
                          const QString& reason = QString()) {
    KalshiBotStopFile stop;
    stop.engaged = true;
    stop.ts_ms = ts_ms;
    stop.source = source;
    stop.reason = reason;
    return stop;
}

QJsonArray ledger(const QList<qint64>& timestamps) {
    QJsonArray rows;
    for (qint64 ts : timestamps)
        rows.append(QJsonObject{{QStringLiteral("event"), QStringLiteral("kalshi_bot_decision")},
                                {QStringLiteral("ts_ms"), static_cast<double>(ts)}});
    return rows;
}

} // namespace

class TestKalshiBotRuntime : public QObject {
    Q_OBJECT

  private slots:
    // --- the freshness classifier the chip and the CLI share ---------------

    void the_staleness_line_is_two_tick_intervals() {
        QCOMPARE(kKalshiBotIntervalSeconds, 60);
        QCOMPARE(kKalshiBotStaleMs, 120'000LL);
    }

    void fresh_stale_and_absent_classify_green_amber_grey() {
        // Fresh: a decision inside the window.
        const auto fresh = kalshi_bot_loop_status(kNow - 30'000, {}, kNow);
        QCOMPARE(fresh.state, QStringLiteral("running"));
        QCOMPARE(kalshi_bot_state_color_role(fresh.state), QStringLiteral("green"));
        QCOMPARE(fresh.age_ms, 30'000LL);
        QVERIFY(fresh.headline.contains(QStringLiteral("BOT RUNNING")));

        // Exactly at the line is still running — stale is strictly past it.
        QCOMPARE(kalshi_bot_loop_status(kNow - kKalshiBotStaleMs, {}, kNow).state,
                 QStringLiteral("running"));
        const auto stale = kalshi_bot_loop_status(kNow - kKalshiBotStaleMs - 1, {}, kNow);
        QCOMPARE(stale.state, QStringLiteral("stale"));
        QCOMPARE(kalshi_bot_state_color_role(stale.state), QStringLiteral("amber"));
        QVERIFY(stale.headline.contains(QStringLiteral("BOT STALE")));

        // Absent: no ledger row at all.
        const auto off = kalshi_bot_loop_status(0, {}, kNow);
        QCOMPARE(off.state, QStringLiteral("off"));
        QCOMPARE(kalshi_bot_state_color_role(off.state), QStringLiteral("grey"));
        // An absent ledger has no age — never a zero-age heartbeat.
        QCOMPARE(off.age_ms, -1LL);
        QVERIFY(off.headline.contains(QStringLiteral("kalshi-bot-decisions.jsonl")));
    }

    void a_future_timestamp_is_mistrusted_not_clamped() {
        const auto status = kalshi_bot_loop_status(kNow + 60'000, {}, kNow);
        QCOMPARE(status.state, QStringLiteral("stale"));
        QVERIFY(status.headline.contains(QStringLiteral("future")));
        QCOMPARE(status.age_ms, -1LL);
    }

    /// The honesty rule this whole header exists for: the last thing a stopped
    /// bot writes is its own refusal row, which is FRESH. Without stop
    /// precedence the chip would sit green on a bot that is not bidding.
    void a_stopped_bot_is_never_reported_as_running() {
        const auto status = kalshi_bot_loop_status(kNow - 1'000, engaged(), kNow);
        QCOMPARE(status.state, QStringLiteral("stopped"));
        QCOMPARE(kalshi_bot_state_color_role(status.state), QStringLiteral("red"));
        QVERIFY(status.headline.contains(QStringLiteral("BOT STOPPED")));
        QVERIFY(status.headline.contains(QStringLiteral("kill switch engaged")));
        // And it outranks stale and absent too.
        QCOMPARE(kalshi_bot_loop_status(kNow - 10'000'000, engaged(), kNow).state,
                 QStringLiteral("stopped"));
        QCOMPARE(kalshi_bot_loop_status(0, engaged(), kNow).state, QStringLiteral("stopped"));
    }

    void the_stopped_headline_names_who_threw_it_and_when() {
        const auto status = kalshi_bot_loop_status(
            kNow, engaged(kNow - 90'000, QStringLiteral("gui"), QStringLiteral("bad calibrator")),
            kNow);
        QVERIFY(status.headline.contains(QStringLiteral("90s ago")));
        QVERIFY(status.headline.contains(QStringLiteral("by gui")));
        QVERIFY(status.headline.contains(QStringLiteral("bad calibrator")));
        // A switch that recorded nothing about itself claims nothing about
        // itself — no invented source, no invented time.
        KalshiBotStopFile bare;
        bare.engaged = true;
        const auto anonymous = kalshi_bot_loop_status(kNow, bare, kNow);
        QVERIFY(anonymous.headline.contains(QStringLiteral("BOT STOPPED")));
        QVERIFY(!anonymous.headline.contains(QStringLiteral(" by ")));
        QVERIFY(!anonymous.headline.contains(QStringLiteral("ago")));
    }

    // --- the kill switch file ----------------------------------------------

    void the_switch_is_the_files_existence_and_fails_closed() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("kalshi-bot-stop.json"));

        QVERIFY(!kalshi_bot_read_stop_file(path).engaged);

        // Junk, empty, and even an explicit "stopped": false still engage it —
        // contents are audit metadata, never the switch.
        for (const QByteArray& contents :
             {QByteArray(), QByteArray("not json at all"), QByteArray("{\"stopped\":false}")}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(contents);
            file.close();
            QVERIFY2(kalshi_bot_read_stop_file(path).engaged, contents.constData());
        }
        QVERIFY(kalshi_bot_clear_stop_file(path));
        QVERIFY(!kalshi_bot_read_stop_file(path).engaged);
        // Clearing a switch that is already clear is not an error.
        QVERIFY(kalshi_bot_clear_stop_file(path));
    }

    void writing_and_reading_the_switch_round_trips_its_metadata() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("kalshi-bot-stop.json"));
        QVERIFY(kalshi_bot_write_stop_file(
            path, kalshi_bot_stop_payload(QStringLiteral("gui"), QStringLiteral("  spooked  "),
                                          kNow)));
        const KalshiBotStopFile stop = kalshi_bot_read_stop_file(path);
        QVERIFY(stop.engaged);
        QCOMPARE(stop.ts_ms, kNow);
        QCOMPARE(stop.source, QStringLiteral("gui"));
        QCOMPARE(stop.reason, QStringLiteral("spooked"));

        // A reason that was not given is absent, not an empty-string claim.
        QVERIFY(!kalshi_bot_stop_payload(QStringLiteral("cli"), QString(), kNow)
                     .contains(QStringLiteral("reason")));
    }

    // --- the ledger tail both surfaces age ---------------------------------

    void the_newest_row_is_the_heartbeat_whatever_its_event() {
        QCOMPARE(kalshi_bot_newest_ts_ms({}), 0LL);
        QCOMPARE(kalshi_bot_newest_ts_ms(ledger({kNow - 5'000, kNow - 1'000, kNow - 9'000})),
                 kNow - 1'000);
        // Paper settlements are ticks too: the loop wrote them.
        QJsonArray rows = ledger({kNow - 5'000});
        rows.append(QJsonObject{{QStringLiteral("event"), QStringLiteral("kalshi_bot_paper_settlement")},
                                {QStringLiteral("ts_ms"), static_cast<double>(kNow - 500)}});
        QCOMPARE(kalshi_bot_newest_ts_ms(rows), kNow - 500);
    }

    /// Issue #145: a row this build cannot otherwise read still proves the loop
    /// ticked, as long as it is dated. The ISO `ts` every row carries beside
    /// `ts_ms` is enough; a reader that needed `ts_ms` would freeze the age at
    /// the last row it understood and report a running bot as stale.
    void a_row_dated_only_in_iso_still_counts_as_a_tick() {
        const QJsonObject iso_only{
            {QStringLiteral("event"), QStringLiteral("kalshi_bot_tick")},
            {QStringLiteral("ts"), QDateTime::fromMSecsSinceEpoch(kNow - 500, QTimeZone::UTC)
                                       .toString(Qt::ISODateWithMs)}};
        QCOMPARE(kalshi_bot_row_ts_ms(iso_only), kNow - 500);
        QCOMPARE(kalshi_bot_row_ts_ms(QJsonObject{{QStringLiteral("event"),
                                                   QStringLiteral("kalshi_bot_tick")}}),
                 0LL);  // undated is undated, never "now"
        QJsonArray rows = ledger({kNow - 5'000});
        rows.append(iso_only);
        QCOMPARE(kalshi_bot_newest_ts_ms(rows), kNow - 500);
    }

    void the_tail_reader_drops_the_partial_first_line_not_whole_rows() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("kalshi-bot-decisions.jsonl"));
        QCOMPARE(kalshi_bot_read_ledger_tail(path).size(), 0);  // absent reads absent

        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        for (int i = 0; i < 40; ++i)
            file.write(QJsonDocument(QJsonObject{{QStringLiteral("event"),
                                                  QStringLiteral("kalshi_bot_decision")},
                                                 {QStringLiteral("ts_ms"),
                                                  static_cast<double>(kNow + i)},
                                                 {QStringLiteral("pad"), QString(200, QLatin1Char('x'))}})
                           .toJson(QJsonDocument::Compact) + "\n");
        file.close();

        const QJsonArray all = kalshi_bot_read_ledger_tail(path);
        QCOMPARE(all.size(), 40);
        // A window that starts mid-line drops that fragment rather than
        // parsing a half row, and still sees the newest rows.
        const QJsonArray window = kalshi_bot_read_ledger_tail(path, 1'000);
        QVERIFY(!window.isEmpty());
        QVERIFY(window.size() < 40);
        QCOMPARE(kalshi_bot_newest_ts_ms(window), kNow + 39);
    }
};

QTEST_APPLESS_MAIN(TestKalshiBotRuntime)
#include "tst_kalshi_bot_runtime.moc"
