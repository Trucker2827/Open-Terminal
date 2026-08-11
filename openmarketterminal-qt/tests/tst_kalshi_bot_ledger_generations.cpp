// The bot decision ledger's generations (issue #152).
//
// The ledger is not a rolling window: it is the sealed promotion gate's whole
// evidence and the order book's only memory of what is resting, what is filled,
// and which contracts have already resolved. Before this, `append_jsonl()`
// rotated it at 64 MB by DELETING `.1` and renaming the base file onto it,
// while every reader read only the base path. On the rotation tick, the book
// forgot its resting orders and reported $0.00 exposure against outstanding
// paper orders.
//
// So three properties are pinned here, all against the real
// append_jsonl → read → replay path rather than hand-built books:
//
//   1. rotation NAMES a new generation and deletes none of them;
//   2. the reader concatenates generations OLDEST FIRST — replay() is
//      order-dependent, and the reversed order is asserted to produce a
//      different (wrong) book, so a flipped concatenation cannot pass;
//   3. a resting order and its exposure survive the rotation, where the
//      base-path-only reader is shown reporting $0.00 for the same record.
//
// The 64 MB threshold is reached by growing the file rather than by writing
// 64 MB of rows — see pad_to_rotation_threshold().

#include "services/prediction/kalshi/KalshiBotDecision.h"
#include "services/prediction/kalshi/KalshiBotOrders.h"
#include "services/prediction/kalshi/KalshiBotRuntime.h"
#include "services/prediction/kalshi/KalshiEvidenceEngine.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

using openmarketterminal::services::prediction::kalshi_ns::KalshiBotDecision;
using openmarketterminal::services::prediction::kalshi_ns::KalshiBotOrders;
using openmarketterminal::services::prediction::kalshi_ns::KalshiBotLedgerRecord;
using openmarketterminal::services::prediction::kalshi_ns::KalshiEvidenceEngine;
using openmarketterminal::services::prediction::kalshi_ns::kalshi_bot_ledger_record;
using openmarketterminal::services::prediction::kalshi_ns::kalshi_bot_loop_status;
using openmarketterminal::services::prediction::kalshi_ns::kalshi_bot_newest_ts_ms;
using openmarketterminal::services::prediction::kalshi_ns::kalshi_bot_next_generation_path;
using openmarketterminal::services::prediction::kalshi_ns::kalshi_bot_oldest_row_ts_ms;
using openmarketterminal::services::prediction::kalshi_ns::kalshi_bot_read_ledger;
using openmarketterminal::services::prediction::kalshi_ns::kalshi_bot_read_ledger_tail;
using openmarketterminal::services::prediction::kalshi_ns::kKalshiBotDecisionLedgerFile;

namespace {

constexpr qint64 kNow = 1'784'900'000'000LL;
constexpr auto kTicker = "KXBTC15M-26JUL241015-15";
constexpr qint64 kRotationBytes = 64LL * 1024 * 1024;

/// The calibrator report shape rung 1's tests use, with one trusted contract.
QJsonObject one(double p_full, double market_mid, qint64 generated_ms = kNow) {
    return QJsonObject{
        {QStringLiteral("schema"), 2},
        {QStringLiteral("event"), QStringLiteral("spot_calibrator")},
        {QStringLiteral("advisory_only"), true},
        {QStringLiteral("generated_at_ms"), static_cast<double>(generated_ms)},
        {QStringLiteral("resolved_contracts"), 371},
        {QStringLiteral("scored_contracts"), 244},
        {QStringLiteral("training_observations"), 12'049},
        {QStringLiteral("brier_full"), 0.1079},
        {QStringLiteral("brier_market_mid_raw"), 0.1083},
        {QStringLiteral("brier_market_trained_logit"), 0.1101},
        {QStringLiteral("adds_value_over_market"), true},
        {QStringLiteral("eligible_scored_contracts"), 200},
        {QStringLiteral("min_eligible_contracts"), 100},
        {QStringLiteral("families"), QJsonArray{QStringLiteral("KXBTC15M")}},
        {QStringLiteral("adds_value_on_bet_eligible"), true},
        {QStringLiteral("brier_eligible_full"), 0.2130},
        {QStringLiteral("brier_eligible_market_mid_raw"), 0.2576},
        {QStringLiteral("predictions"),
         QJsonObject{
             {QString::fromLatin1(kTicker),
              QJsonObject{
                  {QStringLiteral("p_yes_full"), p_full},
                  {QStringLiteral("p_yes_market_baseline"), market_mid},
                  {QStringLiteral("market_yes_mid"), market_mid},
                  {QStringLiteral("features"),
                   QJsonObject{{QStringLiteral("signed_distance_bps"), 120.0},
                               {QStringLiteral("per_min_vol_bps"), 8.0},
                               {QStringLiteral("sqrt_minutes_left"), std::sqrt(10.0)},
                               {QStringLiteral("required_move_sigma"), 1.5},
                               {QStringLiteral("realized_move_bps"), 0.0},
                               {QStringLiteral("yes_mid"), market_mid}}}}}}}};
}

/// The bot's real opening order on kTicker: YES at $0.83 × 2, resting, $1.66 of
/// limit-price exposure. Produced by decide(), not hand-written.
QJsonArray opening_bid(qint64 now_ms = kNow) {
    // 0.15 of edge: past the base threshold AND the resting tier's
    // adverse-selection premium (#165), so this really is an order.
    return KalshiBotDecision::decide(one(0.98, 0.83, now_ms), {}, {}, now_ms, {});
}

/// An undated filler row, for bulking a fixture out.
QJsonObject filler_row(int index) {
    return QJsonObject{{QStringLiteral("event"), QStringLiteral("kalshi_bot_decision")},
                       {QStringLiteral("ts_ms"), static_cast<double>(kNow + index)},
                       {QStringLiteral("ticker"), QStringLiteral("KXBTC15M-FILL-%1").arg(index)},
                       {QStringLiteral("action"), QStringLiteral("pass")},
                       {QStringLiteral("reason_code"), QStringLiteral("NO_EDGE")}};
}

QJsonObject row_at(const QJsonArray& rows, int index) { return rows.at(index).toObject(); }

QJsonArray reversed(const QJsonArray& rows) {
    QJsonArray out;
    for (qsizetype i = rows.size() - 1; i >= 0; --i) out.append(rows.at(i));
    return out;
}

} // namespace

class TestKalshiBotLedgerGenerations : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir dir_;

    QString ledger() const { return dir_.filePath(QString::fromLatin1(kKalshiBotDecisionLedgerFile)); }
    QString generation(int index) const {
        return ledger() + QStringLiteral(".%1").arg(index);
    }

    /// Appends through the real writer, with the record policy the bot uses.
    void append(const QJsonObject& row) {
        QVERIFY(KalshiEvidenceEngine::append_jsonl(
            ledger(), row, KalshiEvidenceEngine::Rotation::KeepAllGenerations));
    }

    void append_all(const QJsonArray& rows) {
        for (const auto& value : rows) append(value.toObject());
    }

    /// Writes `rows` straight to `path`, bypassing the rotation check — for
    /// building a fixture that is already in a post-rotation state.
    void write_rows(const QString& path, const QJsonArray& rows) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text));
        for (const auto& value : rows) {
            file.write(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
            file.write("\n");
        }
    }

    /// Brings the live file to the 64 MB rotation threshold WITHOUT writing
    /// 64 MB of rows: the file is grown, which pads it with a run of bytes no
    /// reader can parse as a row and every reader therefore skips. Nothing
    /// under test here depends on the filler's contents — the generation names,
    /// the concatenation order and the tail window are all properties of the
    /// real rows, which are written before and after it.
    void pad_to_rotation_threshold() {
        QFile file(ledger());
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.resize(kRotationBytes));
        file.close();
        QVERIFY(QFileInfo(ledger()).size() >= kRotationBytes);
    }

    static QByteArray digest(const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&file);
        return hash.result().toHex();
    }

  private slots:
    void init() {
        // One clean evidence dir per test.
        QVERIFY(dir_.isValid());
        const QDir dir(dir_.path());
        for (const QString& name : dir.entryList(QDir::Files))
            QVERIFY(QFile::remove(dir.filePath(name)));
    }

    // --- criterion 1: no generation is ever deleted -------------------------

    void two_rotations_keep_every_generation_byte_for_byte() {
        append(filler_row(1));
        pad_to_rotation_threshold();
        append(filler_row(2));  // rotation 1: the padded file becomes .1

        QVERIFY(QFileInfo::exists(generation(1)));
        const qint64 first_size = QFileInfo(generation(1)).size();
        const QByteArray first_digest = digest(generation(1));
        QVERIFY(!first_digest.isEmpty());

        pad_to_rotation_threshold();
        append(filler_row(3));  // rotation 2: the next generation is .2, not .1

        QVERIFY2(QFileInfo::exists(generation(1)),
                 "the second rotation deleted the first generation");
        QCOMPARE(QFileInfo(generation(1)).size(), first_size);
        QCOMPARE(digest(generation(1)), first_digest);
        QVERIFY(QFileInfo::exists(generation(2)));
        QVERIFY(!QFileInfo::exists(generation(3)));

        // And the live file is the newest row only.
        const QJsonArray passes = kalshi_bot_read_ledger(ledger(), [](const QJsonObject& row) {
            return row.value(QStringLiteral("action")).toString() == QStringLiteral("pass");
        });
        QCOMPARE(passes.size(), 3);  // all three rows are still in the record

        const KalshiBotLedgerRecord record = kalshi_bot_ledger_record(ledger());
        QCOMPARE(record.newest_generation, 2);
        QCOMPARE(record.paths, QStringList({generation(1), generation(2), ledger()}));
        QVERIFY(record.missing.isEmpty());
    }

    void the_recycling_rotation_is_unchanged_for_every_other_evidence_file() {
        // The non-goal, pinned: only the bot ledger keeps its generations. The
        // ladder/tick evidence is a window and still recycles two of them.
        const QString path = dir_.filePath(QStringLiteral("kalshi-ladder-snapshots.jsonl"));
        const auto grow = [&path]() {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.resize(kRotationBytes));
            file.close();
        };
        QVERIFY(KalshiEvidenceEngine::append_jsonl(path, filler_row(1)));
        grow();
        QVERIFY(KalshiEvidenceEngine::append_jsonl(path, filler_row(2)));
        const QByteArray first = digest(path + QStringLiteral(".1"));
        grow();
        QVERIFY(KalshiEvidenceEngine::append_jsonl(path, filler_row(3)));
        QVERIFY2(!QFileInfo::exists(path + QStringLiteral(".2")),
                 "the default rotation grew a third generation");
        QVERIFY2(digest(path + QStringLiteral(".1")) != first,
                 "the default rotation stopped recycling its oldest generation");
    }

    void the_next_generation_is_never_a_name_already_used() {
        write_rows(generation(2), QJsonArray{filler_row(1)});
        write_rows(ledger(), QJsonArray{filler_row(2)});
        // .1 is missing. Backfilling it would put the NEWEST chunk under the
        // OLDEST name and heal the hole the gate refuses on.
        QCOMPARE(kalshi_bot_next_generation_path(ledger()), generation(3));
    }

    // --- criterion 2: one reader, oldest generation first -------------------

    void a_bid_in_the_older_generation_fills_from_the_newer_one() {
        const QJsonArray bid = opening_bid();
        // The mid comes back through the $0.83 limit one second later: the real
        // fill row, produced by reconcile(), not hand-written.
        const QJsonArray fill = KalshiBotOrders::reconcile(
            KalshiBotOrders::replay(bid), one(0.95, 0.82, kNow + 1000), {}, kNow + 1000, {},
            [](const QJsonObject&, const QString&) { return true; });
        QCOMPARE(fill.size(), 1);
        QCOMPARE(row_at(fill, 0).value(QStringLiteral("action")).toString(), QStringLiteral("fill"));

        write_rows(generation(1), bid);   // the older generation holds the bid
        write_rows(ledger(), fill);       // the live file holds its fill

        const QJsonArray rows = kalshi_bot_read_ledger(ledger());
        QCOMPARE(rows.size(), bid.size() + 1);
        // Read in the record's own order, the two rows are one filled position.
        const KalshiBotOrders::Book book = KalshiBotOrders::replay(rows);
        QCOMPARE(book.positions.size(), 1);
        QCOMPARE(book.resting.size(), 0);
        QCOMPARE(book.exposure_usd, 1.66);

        // And the reverse concatenation does NOT quietly produce that book: a
        // fill lands only on an order the replay has already seen opened, so
        // newest-first would report the position as still resting.
        const KalshiBotOrders::Book flipped = KalshiBotOrders::replay(reversed(rows));
        QCOMPARE(flipped.positions.size(), 0);
        QCOMPARE(flipped.resting.size(), 1);
    }

    void the_reader_reports_the_generations_in_chronological_order() {
        write_rows(generation(1), QJsonArray{filler_row(1)});
        write_rows(generation(2), QJsonArray{filler_row(2)});
        write_rows(ledger(), QJsonArray{filler_row(3)});
        const QJsonArray rows = kalshi_bot_read_ledger(ledger());
        QCOMPARE(rows.size(), 3);
        for (int i = 0; i < 3; ++i)
            QCOMPARE(row_at(rows, i).value(QStringLiteral("ts_ms")).toDouble(),
                     static_cast<double>(kNow + i + 1));
        QCOMPARE(kalshi_bot_oldest_row_ts_ms(ledger()), kNow + 1);
    }

    void a_hole_in_the_sequence_is_reported_not_papered_over() {
        write_rows(generation(1), QJsonArray{filler_row(1)});
        write_rows(generation(2), QJsonArray{filler_row(2)});
        write_rows(generation(3), QJsonArray{filler_row(3)});
        write_rows(ledger(), QJsonArray{filler_row(4)});
        QVERIFY(QFile::remove(generation(2)));

        const KalshiBotLedgerRecord record = kalshi_bot_ledger_record(ledger());
        QCOMPARE(record.missing, QStringList({generation(2)}));
        QCOMPARE(record.paths, QStringList({generation(1), generation(3), ledger()}));
        // A record with a hole still READS — the refusal is the gate's job, and
        // the reader must not silently return nothing instead.
        QCOMPARE(kalshi_bot_read_ledger(ledger()).size(), 3);
    }

    void only_a_bare_integer_suffix_is_a_generation() {
        write_rows(generation(1), QJsonArray{filler_row(1)});
        write_rows(ledger() + QStringLiteral(".1.gz"), QJsonArray{filler_row(9)});
        write_rows(ledger() + QStringLiteral(".01"), QJsonArray{filler_row(9)});
        write_rows(ledger() + QStringLiteral(".old"), QJsonArray{filler_row(9)});
        write_rows(ledger(), QJsonArray{filler_row(2)});

        const KalshiBotLedgerRecord record = kalshi_bot_ledger_record(ledger());
        QCOMPARE(record.newest_generation, 1);
        QCOMPARE(record.paths, QStringList({generation(1), ledger()}));
        QVERIFY(record.missing.isEmpty());
        QCOMPARE(kalshi_bot_read_ledger(ledger()).size(), 2);
    }

    // --- criterion 3: the tick's book survives the rotation -----------------

    void a_resting_order_and_its_exposure_survive_a_rotation() {
        const QJsonArray bid = opening_bid();
        append_all(bid);
        pad_to_rotation_threshold();
        append(filler_row(50));  // the tick that rotates

        QVERIFY(QFileInfo::exists(generation(1)));
        const KalshiBotOrders::Book book =
            KalshiBotOrders::replay(kalshi_bot_read_ledger(ledger()));
        QCOMPARE(book.resting.size(), 1);
        QCOMPARE(row_at(book.resting, 0).value(QStringLiteral("ticker")).toString(),
                 QString::fromLatin1(kTicker));
        QCOMPARE(row_at(book.resting, 0).value(QStringLiteral("remaining_count")).toDouble(), 2.0);
        QCOMPARE(book.resting_usd, 1.66);
        QVERIFY2(book.exposure_usd != 0.0, "exposure read $0.00 with an order outstanding");
        QCOMPARE(book.exposure_usd, 1.66);

        // The bug this replaces, shown rather than described: the reader that
        // saw only the live file reported an empty book and $0.00 of exposure
        // for this very same record.
        QJsonArray base_only;
        QFile file(ledger());
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        while (!file.atEnd()) {
            const QJsonDocument document = QJsonDocument::fromJson(file.readLine());
            if (document.isObject()) base_only.append(document.object());
        }
        const KalshiBotOrders::Book blind = KalshiBotOrders::replay(base_only);
        QCOMPARE(blind.resting.size(), 0);
        QCOMPARE(blind.exposure_usd, 0.0);
    }

    // --- criterion 6: the surfaces do not go blank about a rotation ---------

    void the_tail_spans_generations() {
        QJsonArray older;
        for (int i = 1; i <= 5; ++i) older.append(filler_row(i));
        write_rows(generation(1), older);
        write_rows(ledger(), QJsonArray{filler_row(6), filler_row(7)});

        const QJsonArray tail = kalshi_bot_read_ledger_tail(ledger());
        QCOMPARE(tail.size(), 7);
        for (int i = 0; i < 7; ++i)
            QCOMPARE(row_at(tail, i).value(QStringLiteral("ts_ms")).toDouble(),
                     static_cast<double>(kNow + i + 1));
    }

    void a_short_window_drops_only_the_oldest_partial_row() {
        QJsonArray older;
        for (int i = 1; i <= 5; ++i) older.append(filler_row(i));
        write_rows(generation(1), older);
        write_rows(ledger(), QJsonArray{filler_row(6), filler_row(7)});

        // A window that ends mid-row inside the OLDER generation. Every row of
        // the newer file must survive: dropping a first line per file (rather
        // than per window) would silently eat row 6.
        const qint64 base_bytes = QFileInfo(ledger()).size();
        const qint64 older_bytes = QFileInfo(generation(1)).size();
        const qint64 window = base_bytes + (older_bytes / 5) + (older_bytes / 10);

        const QJsonArray tail = kalshi_bot_read_ledger_tail(ledger(), window);
        QVERIFY(tail.size() >= 3);
        QVERIFY2(tail.size() < 7, "the window was not short enough to prove anything");
        // Whatever the window reached, it ends with the live file's rows, in
        // order, and carries no partial row.
        QCOMPARE(row_at(tail, tail.size() - 2).value(QStringLiteral("ts_ms")).toDouble(),
                 static_cast<double>(kNow + 6));
        QCOMPARE(row_at(tail, tail.size() - 1).value(QStringLiteral("ts_ms")).toDouble(),
                 static_cast<double>(kNow + 7));
    }

    void a_running_bot_never_reads_as_off_when_the_live_file_is_empty() {
        // The rotation left the record in .1 and the live file did not land
        // (a failed append, a truncated write). The base-path-only tail read
        // nothing here and the chip said BOT OFF — "the bot has never run
        // here" — about a bot with 63,000 rows of record and orders resting.
        write_rows(generation(1), QJsonArray{filler_row(1), filler_row(2)});
        write_rows(ledger(), QJsonArray{});
        QVERIFY(QFileInfo::exists(ledger()));
        QCOMPARE(QFileInfo(ledger()).size(), 0);

        const QJsonArray tail = kalshi_bot_read_ledger_tail(ledger());
        QCOMPARE(tail.size(), 2);
        const qint64 newest = kalshi_bot_newest_ts_ms(tail);
        QCOMPARE(newest, kNow + 2);
        const auto status = kalshi_bot_loop_status(newest, {}, newest + 1000);
        QCOMPARE(status.state, QStringLiteral("running"));
    }
};

QTEST_MAIN(TestKalshiBotLedgerGenerations)
#include "tst_kalshi_bot_ledger_generations.moc"
