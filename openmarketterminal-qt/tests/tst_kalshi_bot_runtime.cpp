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

/// One decision row stating `mode`. An empty `mode` writes NO key at all — the
/// "a tick that states nothing" case, distinct from one that states nonsense.
QJsonObject tick(qint64 ts_ms, const QString& mode) {
    QJsonObject row{{QStringLiteral("event"), QStringLiteral("kalshi_bot_decision")},
                    {QStringLiteral("ts_ms"), static_cast<double>(ts_ms)},
                    {QStringLiteral("action"), QStringLiteral("pass")}};
    if (!mode.isEmpty()) row.insert(QStringLiteral("mode"), mode);
    return row;
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

    // --- the mode word both surfaces render (issue #155) --------------------
    // The rule itself is the BOT badge's, moved here so `kalshi bot status` can
    // render the same answer instead of the literal "paper" it used to print.

    void the_mode_is_the_newest_ticks_own_word() {
        QCOMPARE(kalshi_bot_mode(QJsonArray{tick(kNow - 30'000, QStringLiteral("paper"))}).mode,
                 QStringLiteral("paper"));
        QCOMPARE(kalshi_bot_mode(QJsonArray{tick(kNow - 30'000, QStringLiteral("live"))}).mode,
                 QStringLiteral("live"));
        QVERIFY(kalshi_bot_mode(QJsonArray{tick(kNow - 30'000, QStringLiteral("live"))}).live);
        QVERIFY(!kalshi_bot_mode(QJsonArray{tick(kNow - 30'000, QStringLiteral("paper"))}).live);
        // Badges are the uppercase of the same one word — no second spelling.
        QCOMPARE(kalshi_bot_mode(QJsonArray{tick(kNow, QStringLiteral("live"))}).badge(),
                 QStringLiteral("LIVE"));
    }

    /// The rule is NEWEST, not "ever": a bot that ran live an hour ago and
    /// papers now is papering. Ordering is by timestamp, not file position —
    /// two appenders interleave rows (issue #145).
    void a_live_row_further_back_does_not_make_this_tick_live() {
        const auto mode = kalshi_bot_mode(QJsonArray{tick(kNow - 20'000, QStringLiteral("paper")),
                                                     tick(kNow - 3'600'000, QStringLiteral("live"))});
        QCOMPARE(mode.mode, QStringLiteral("paper"));
        QVERIFY(!mode.live);
    }

    /// Fail CLOSED: the newest row being one this build cannot read means the
    /// loop is a different binary's, and reading the mode off the last row that
    /// happened to parse is how a surface announces LIVE over a paper record.
    void an_unreadable_newest_row_reads_unknown_never_paper_and_never_live() {
        const QJsonObject odd_event{{QStringLiteral("event"), QStringLiteral("kalshi_bot_tick")},
                                    {QStringLiteral("ts_ms"), double(kNow - 1'000)}};
        for (const QJsonArray& rows :
             {QJsonArray{tick(kNow - 20'000, QStringLiteral("live")), odd_event},
              QJsonArray{tick(kNow - 20'000, QStringLiteral("paper")), odd_event},
              // A known tick whose own `mode` is absent or unrecognised is
              // unknown too — an unstated mode is not a paper claim.
              QJsonArray{tick(kNow - 20'000, QStringLiteral("paper")), tick(kNow - 1'000, QString())},
              QJsonArray{tick(kNow - 1'000, QStringLiteral("shadow"))}}) {
            const auto mode = kalshi_bot_mode(rows);
            QCOMPARE(mode.mode, QStringLiteral("unknown"));
            QVERIFY(mode.unknown);
            QVERIFY2(!mode.live, "an unreadable newest row must never read live");
        }
    }

    /// A paper settlement is written by the same binary and is NOT a tick, so
    /// it makes no claim about the mode either way. Treating one as unreadable
    /// would blank a real LIVE badge; treating it as a paper tick would mask
    /// real money at the exchange.
    void a_settlement_is_not_a_tick_and_claims_no_mode() {
        const QJsonObject settlement{
            {QStringLiteral("event"), QStringLiteral("kalshi_bot_paper_settlement")},
            {QStringLiteral("ts_ms"), double(kNow - 500)}};
        const auto mode =
            kalshi_bot_mode(QJsonArray{tick(kNow - 20'000, QStringLiteral("live")), settlement});
        QCOMPARE(mode.mode, QStringLiteral("live"));
        QVERIFY(!mode.unknown);
        // And a record of settlements ALONE claims nothing at all — dated rows,
        // a running loop, but no tick has stated a mode.
        QVERIFY(!kalshi_bot_mode(QJsonArray{settlement}).stated());
    }

    /// Absent is absent, the same rule `last_decision_age_ms` follows: nothing
    /// that could claim a mode means NO mode word, never a default `paper`.
    ///
    /// "Nothing to claim" is not the same as "nothing dated": a record of dated
    /// settlements alone (previous case) claims no mode either, while a row
    /// this build cannot read at all claims UNKNOWN rather than nothing —
    /// an unreadable row is evidence of a binary, and saying so is the point.
    void a_record_with_no_tick_claims_no_mode_at_all() {
        const auto empty = kalshi_bot_mode({});
        QVERIFY2(!empty.stated(), qPrintable(empty.mode));
        QVERIFY(empty.badge().isEmpty());
        QVERIFY(!empty.live);
        QVERIFY(!empty.unknown);

        // A row with no `event` at all is not "no row" — it is a row this build
        // cannot read, and that is UNKNOWN.
        QCOMPARE(kalshi_bot_mode(QJsonArray{QJsonObject{}}).mode, QStringLiteral("unknown"));
    }

    /// An undated tick still STATES a mode, and dropping it would let a mode
    /// nothing claimed win by default — the stamp's validity is "came off a
    /// row", not "that row was dated".
    void an_undated_tick_still_states_its_mode() {
        QJsonObject undated = tick(0, QStringLiteral("paper"));
        undated.remove(QStringLiteral("ts_ms"));
        QCOMPARE(kalshi_bot_mode(QJsonArray{undated}).mode, QStringLiteral("paper"));
    }

    /// The `[MODE] status` form `kalshi bot status` prints and the BOT panel
    /// paints, from one function — including the UNKNOWN reason and the
    /// launchctl hint, so neither surface can word it differently.
    void the_headline_carries_the_badge_and_unknown_carries_its_reason() {
        const QString headline = QStringLiteral("BOT RUNNING · last decision 30s ago");
        QCOMPARE(kalshi_bot_mode_headline(
                     headline, kalshi_bot_mode(QJsonArray{tick(kNow, QStringLiteral("live"))})),
                 QStringLiteral("[LIVE] ") + headline);
        QCOMPARE(kalshi_bot_mode_headline(
                     headline, kalshi_bot_mode(QJsonArray{tick(kNow, QStringLiteral("paper"))})),
                 QStringLiteral("[PAPER] ") + headline);
        // Nothing to claim prints the headline bare — no `[PAPER]` by default.
        QCOMPARE(kalshi_bot_mode_headline(headline, kalshi_bot_mode({})), headline);

        const QString unknown =
            kalshi_bot_mode_headline(headline, kalshi_bot_mode(QJsonArray{
                                                   tick(kNow, QStringLiteral("shadow"))}));
        QVERIFY(unknown.startsWith(QStringLiteral("[UNKNOWN] ")));
        QVERIFY2(unknown.contains(QStringLiteral("no mode is claimed")), qPrintable(unknown));
        QVERIFY2(unknown.contains(kalshi_bot_vintage_hint()), qPrintable(unknown));
    }

    // --- the gate verdict's age, one reading for both surfaces (#167) -------

    /// A verdict the loop re-evaluated within the window states its age and is
    /// NOT stale. The sentence is the issue's own form ("evaluated Xs/Xm ago").
    void a_fresh_verdict_states_its_age_and_is_not_stale() {
        const KalshiBotGateFreshness fresh = kalshi_bot_gate_freshness(
            QJsonObject{{QStringLiteral("verdict"), QStringLiteral("PASS")},
                        {QStringLiteral("ts_ms"), static_cast<double>(kNow - 12'000)}},
            kNow);
        QVERIFY(fresh.dated);
        QVERIFY(!fresh.stale);
        QCOMPARE(fresh.age_ms, 12'000);
        QCOMPARE(fresh.text, QStringLiteral("evaluated 12s ago"));
    }

    /// Older than the staleness window: the age is still stated, and the
    /// reading says STALE so no surface can paint it as current. The threshold
    /// is the chip's own two-tick constant.
    void a_verdict_older_than_the_window_reads_stale() {
        const KalshiBotGateFreshness stale = kalshi_bot_gate_freshness(
            QJsonObject{{QStringLiteral("verdict"), QStringLiteral("PASS")},
                        {QStringLiteral("ts_ms"),
                         static_cast<double>(kNow - 5LL * 3'600'000)}},
            kNow);
        QVERIFY(stale.dated);
        QVERIFY(stale.stale);
        QVERIFY2(stale.text.startsWith(QStringLiteral("evaluated 5h ago")), qPrintable(stale.text));
        QVERIFY2(stale.text.contains(QStringLiteral("STALE")), qPrintable(stale.text));
        // The display threshold is deliberately the chip's, not the live
        // admission bound — a change to either must not silently move the other.
        QCOMPARE(kKalshiBotGateStaleMs, kKalshiBotStaleMs);
        QVERIFY(!kalshi_bot_gate_freshness(
                     QJsonObject{{QStringLiteral("ts_ms"),
                                  static_cast<double>(kNow - kKalshiBotGateStaleMs)}},
                     kNow)
                     .stale);
        QVERIFY(kalshi_bot_gate_freshness(
                    QJsonObject{{QStringLiteral("ts_ms"),
                                 static_cast<double>(kNow - kKalshiBotGateStaleMs - 1)}},
                    kNow)
                    .stale);
    }

    /// Every reason the age cannot be vouched for reads stale rather than
    /// current: no timestamp at all, and a timestamp in the future.
    void an_unvouchable_age_is_stale_never_current() {
        const KalshiBotGateFreshness undated = kalshi_bot_gate_freshness(
            QJsonObject{{QStringLiteral("verdict"), QStringLiteral("PASS")}}, kNow);
        QVERIFY(!undated.dated);
        QVERIFY(undated.stale);
        QCOMPARE(undated.age_ms, -1);
        QVERIFY2(undated.text.contains(QStringLiteral("UNKNOWN")), qPrintable(undated.text));

        const KalshiBotGateFreshness future = kalshi_bot_gate_freshness(
            QJsonObject{{QStringLiteral("ts_ms"), static_cast<double>(kNow + 60'000)}}, kNow);
        QVERIFY(!future.dated);
        QVERIFY(future.stale);
        QVERIFY2(future.text.contains(QStringLiteral("FUTURE")), qPrintable(future.text));
    }

    /// No file is not an age. The empty reading carries no sentence at all —
    /// the caller says NOT EVALUATED, which is a different fact from "old".
    void an_absent_verdict_has_no_age_to_state() {
        const KalshiBotGateFreshness absent = kalshi_bot_gate_freshness({}, kNow);
        QVERIFY(absent.text.isEmpty());
        QCOMPARE(absent.age_ms, -1);
        QVERIFY(!absent.dated);
    }
};

QTEST_APPLESS_MAIN(TestKalshiBotRuntime)
#include "tst_kalshi_bot_runtime.moc"
