// tst_sandbox_ticktail.cpp — TickTail: tail reader over scalp_ticks.jsonl
// for the Strategy Sandbox fill model (Task 3).
//
// Pure file reader (no DB, no profile/HOME games needed): ticks_since takes
// the ticks file path directly, so each test slot points it at its own
// QTemporaryDir fixture.

#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>

#include "services/sandbox/TickTail.h"

using namespace openmarketterminal::services::sandbox;

namespace {

QString tick_line(const QString& symbol, double price, double bid, double ask, qint64 ts_ms) {
    return QStringLiteral(
               R"({"symbol":"%1","price":%2,"best_bid":%3,"best_ask":%4,"received_ts_ms":"%5","source":"coinbase"})")
        .arg(symbol)
        .arg(price)
        .arg(bid)
        .arg(ask)
        .arg(ts_ms);
}

void write_lines(const QString& path, const QStringList& lines) {
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly | QIODevice::Truncate), qUtf8Printable(path));
    for (const QString& line : lines) {
        f.write(line.toUtf8());
        f.write("\n");
    }
}

} // namespace

class TstSandboxTickTail : public QObject {
    Q_OBJECT
  private slots:

    // Fixture: 3 ascending BTC-USD ticks, 1 ETH-USD tick, 1 corrupt line, 1
    // BTC-USD tick with price 0. Verifies: symbol filter, since_ms exclusive
    // filter, ascending order, and that corrupt/zero-price rows are skipped.
    void filters_symbol_since_and_bad_rows() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("scalp_ticks.jsonl");

        write_lines(path, {
            tick_line("BTC-USD", 60000.0, 59999.0, 60000.5, 1000),
            tick_line("BTC-USD", 60010.0, 60009.0, 60010.5, 2000),
            tick_line("BTC-USD", 60020.0, 60019.0, 60020.5, 3000),
            tick_line("ETH-USD", 1783.02, 1783.01, 1783.02, 1500),
            QStringLiteral("{ this is not valid json"),
            tick_line("BTC-USD", 0.0, 59999.0, 60000.5, 4000),
        });

        const auto rows = ticks_since(path, "BTC-USD", 1000);

        // Only the ts=2000 and ts=3000 BTC-USD rows survive: ts=1000 is
        // excluded by since_ms (exclusive), the price<=0 row is dropped, the
        // corrupt line is skipped, and ETH-USD is filtered out.
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0].symbol, QStringLiteral("BTC-USD"));
        QCOMPARE(rows[0].ts_ms, qint64(2000));
        QCOMPARE(rows[0].price, 60010.0);
        QCOMPARE(rows[0].best_bid, 60009.0);
        QCOMPARE(rows[0].best_ask, 60010.5);
        QCOMPARE(rows[1].ts_ms, qint64(3000));
        QVERIFY(rows[0].ts_ms < rows[1].ts_ms);

        // since_ms = 0 picks up everything including ts=1000, still
        // excluding the corrupt/zero-price/other-symbol rows.
        const auto all_rows = ticks_since(path, "BTC-USD", 0);
        QCOMPARE(all_rows.size(), 3);
        QCOMPARE(all_rows[0].ts_ms, qint64(1000));
    }

    // A candidate tick sits only in the rotated ".1" predecessor, and the
    // active file is short enough that the remaining tail_bytes budget must
    // be filled from ".1" to find it -- same blending rule as
    // automation::latest_candidate.
    void finds_candidate_only_in_prev_generation() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("scalp_ticks.jsonl");
        const QString prev_path = path + QStringLiteral(".1");

        write_lines(prev_path, {
            tick_line("BTC-USD", 59000.0, 58999.0, 59000.5, 500),
        });
        write_lines(path, {
            tick_line("BTC-USD", 60000.0, 59999.0, 60000.5, 1000),
        });

        // tail_bytes far larger than either file, so the whole budget is
        // available and the short active file triggers the prev-generation
        // fallback fill.
        const auto rows = ticks_since(path, "BTC-USD", 0, 1024 * 1024);

        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0].ts_ms, qint64(500));
        QCOMPARE(rows[1].ts_ms, qint64(1000));
    }
};

QTEST_GUILESS_MAIN(TstSandboxTickTail)
#include "tst_sandbox_ticktail.moc"
