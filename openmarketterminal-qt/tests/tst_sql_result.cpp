// SqlResult must behave like the QSqlQuery it replaces -- 408 call sites read
// it -- while holding no live statement.

#include <QtTest/QtTest>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "storage/sqlite/Database.h"
#include "storage/sqlite/SqlResult.h"

using namespace openmarketterminal;
using openmarketterminal::storage::sqlite::SqlResult;

class SqlResultTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        auto o = Database::instance().open(dir_.path() + "/t.db");
        QVERIFY2(o.is_ok(), o.is_err() ? o.error().c_str() : "");
        QVERIFY(Database::instance().exec("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER, s TEXT)").is_ok());
        for (int i = 1; i <= 3; ++i)
            QVERIFY(Database::instance()
                        .execute("INSERT INTO t (v, s) VALUES (?, ?)", {i * 10, QStringLiteral("r%1").arg(i)})
                        .is_ok());
    }
    void cleanupTestCase() { Database::instance().close(); }

    void walks_every_row_like_a_cursor() {
        auto r = Database::instance().execute("SELECT v FROM t ORDER BY v");
        QVERIFY(r.is_ok());
        QVector<int> seen;
        while (r.value().next())
            seen << r.value().value(0).toInt();
        QCOMPARE(seen, QVector<int>({10, 20, 30}));
    }

    void starts_before_the_first_row() {
        auto r = Database::instance().execute("SELECT v FROM t ORDER BY v");
        QVERIFY(r.is_ok());
        QVERIFY(!r.value().isValid());              // nothing selected yet
        QVERIFY(!r.value().value(0).isValid());     // reading now yields invalid
        QVERIFY(r.value().next());
        QCOMPARE(r.value().value(0).toInt(), 10);
    }

    void next_stays_false_past_the_end() {
        auto r = Database::instance().execute("SELECT v FROM t");
        QVERIFY(r.is_ok());
        while (r.value().next()) {}
        QVERIFY(!r.value().next());
        QVERIFY(!r.value().next());   // and does not wrap around
        QVERIFY(!r.value().isValid());
    }

    void an_empty_result_is_empty_not_null() {
        auto r = Database::instance().execute("SELECT v FROM t WHERE v = ?", {9999});
        QVERIFY(r.is_ok());
        QVERIFY(!r.value().next());
        QVERIFY(r.value().isEmpty());
        QCOMPARE(r.value().size(), 0);
        QVERIFY(!r.value().first());
    }

    void first_positions_on_row_zero() {
        auto r = Database::instance().execute("SELECT v FROM t ORDER BY v");
        QVERIFY(r.is_ok());
        QVERIFY(r.value().first());
        QCOMPARE(r.value().value(0).toInt(), 10);
    }

    void columns_are_addressable_by_name() {
        auto r = Database::instance().execute("SELECT v, s FROM t ORDER BY v");
        QVERIFY(r.is_ok());
        QVERIFY(r.value().next());
        QCOMPARE(r.value().value(QStringLiteral("s")).toString(), QStringLiteral("r1"));
        QCOMPARE(r.value().value(QStringLiteral("v")).toInt(), 10);
        QVERIFY(!r.value().value(QStringLiteral("nope")).isValid());
    }

    void out_of_range_columns_are_invalid_not_a_crash() {
        auto r = Database::instance().execute("SELECT v FROM t");
        QVERIFY(r.is_ok());
        QVERIFY(r.value().next());
        QVERIFY(!r.value().value(99).isValid());
        QVERIFY(!r.value().value(-1).isValid());
    }

    void writes_report_rows_affected() {
        auto r = Database::instance().execute("UPDATE t SET s = ? WHERE v >= ?", {QStringLiteral("x"), 20});
        QVERIFY(r.is_ok());
        QCOMPARE(r.value().numRowsAffected(), 2);
    }

    void size_is_known_unlike_a_sqlite_cursor() {
        // QSqlQuery::size() returns -1 on SQLite. Buffering makes it real.
        auto r = Database::instance().execute("SELECT v FROM t");
        QVERIFY(r.is_ok());
        QCOMPARE(r.value().size(), 3);
    }

    void a_sql_null_is_distinguishable_from_zero() {
        // A NULL P&L means "not settled yet"; 0.0 means "settled flat". The UI
        // shows "--" for the first and "$0.00" for the second, so buffering
        // must not collapse them.
        QVERIFY(Database::instance().exec("CREATE TABLE n (a INTEGER, b REAL)").is_ok());
        QVERIFY(Database::instance().execute("INSERT INTO n (a, b) VALUES (?, NULL)", {1}).is_ok());
        QVERIFY(Database::instance().execute("INSERT INTO n (a, b) VALUES (?, ?)", {2, 0.0}).is_ok());

        auto r = Database::instance().execute("SELECT a, b FROM n ORDER BY a");
        QVERIFY(r.is_ok());
        QVERIFY(r.value().next());
        QVERIFY(r.value().isNull(1));            // NULL
        QVERIFY(r.value().next());
        QVERIFY(!r.value().isNull(1));           // 0.0 is a value, not NULL
        QCOMPARE(r.value().value(1).toDouble(), 0.0);
    }

    // ---- the property this class exists for ---------------------------
    void a_write_succeeds_while_an_earlier_result_is_still_held() {
        // The caller keeps the result alive, exactly as PaperExecutor does when
        // it iterates candidates and inserts a position per row. Because the
        // statement was released inside execute(), no read transaction is open
        // and the write needs no upgrade.
        auto held = Database::instance().execute("SELECT id, v FROM t");
        QVERIFY(held.is_ok());
        QVERIFY(held.value().next());

        auto w = Database::instance().execute("INSERT INTO t (v, s) VALUES (?, ?)",
                                              {777, QStringLiteral("during-read")});
        QVERIFY2(w.is_ok(), w.is_err() ? w.error().c_str() : "write failed mid-read");

        // The held result keeps its own snapshot and is still walkable.
        int remaining = 0;
        while (held.value().next())
            ++remaining;
        QVERIFY(remaining >= 1);

        auto check = Database::instance().execute("SELECT COUNT(*) FROM t WHERE v = 777");
        QVERIFY(check.is_ok());
        QVERIFY(check.value().next());
        QCOMPARE(check.value().value(0).toInt(), 1);
    }

    // ---- the A/B that actually gates the refactor ---------------------
    //
    // Both halves run in this process, on the SAME shared connection, with a
    // second connection holding the write lock. The only difference is whether
    // the earlier SELECT is still mid-result.
    //
    // Old API (Result<QSqlQuery>): callers iterated lazily, so the statement
    // sat mid-result while later code wrote -> read->write upgrade -> SQLite
    // returns SQLITE_BUSY immediately, without consulting busy_timeout.
    // New API: execute() reads every row before returning, so no statement is
    // mid-result and the write waits for the lock like any other writer.
    void a_midresult_cursor_blocks_a_write_but_a_buffered_result_does_not() {
        const QString other = QStringLiteral("tst_sqlresult_lockholder");
        auto with_lock_held = [&](auto&& body) {
            std::atomic<bool> holding{false};
            std::thread locker([&] {
                QSqlDatabase o = QSqlDatabase::addDatabase("QSQLITE", other);
                o.setDatabaseName(Database::instance().path());
                if (!o.open()) return;
                QSqlQuery q(o);
                q.exec("PRAGMA busy_timeout = 5000");
                q.exec("BEGIN IMMEDIATE");
                q.exec("INSERT INTO t (v, s) VALUES (4242, 'lock')");
                holding = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
                q.exec("COMMIT");
                o.close();
            });
            while (!holding) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            body();
            locker.join();
            QSqlDatabase::removeDatabase(other);
        };

        // (a) OLD SHAPE: a raw cursor left mid-result on the shared connection.
        bool old_failed = false;
        qint64 old_ms = -1;
        with_lock_held([&] {
            QSqlQuery lazy(Database::instance().connection());
            QVERIFY(lazy.exec("SELECT id, v FROM t"));
            QVERIFY(lazy.next());          // stopped MID-result, statement live
            QElapsedTimer t; t.start();
            auto w = Database::instance().execute("INSERT INTO t (v, s) VALUES (?, ?)",
                                                  {1, QStringLiteral("old")});
            old_ms = t.elapsed();
            old_failed = w.is_err();
            lazy.finish();
        });
        QVERIFY2(old_failed, "a write under a mid-result cursor was expected to fail");
        QVERIFY2(old_ms < 300, qPrintable(QStringLiteral("expected an INSTANT failure "
                 "(busy handler skipped), took %1ms").arg(old_ms)));

        // (b) NEW SHAPE: the same read through execute(), fully buffered.
        bool new_ok = false;
        qint64 new_ms = -1;
        with_lock_held([&] {
            auto held = Database::instance().execute("SELECT id, v FROM t");
            QVERIFY(held.is_ok());
            QVERIFY(held.value().next());  // mid-walk, but nothing is live
            QElapsedTimer t; t.start();
            auto w = Database::instance().execute("INSERT INTO t (v, s) VALUES (?, ?)",
                                                  {2, QStringLiteral("new")});
            new_ms = t.elapsed();
            new_ok = w.is_ok();
        });
        QVERIFY2(new_ok, "a write under a buffered result should succeed");
        QVERIFY2(new_ms >= 300, qPrintable(QStringLiteral("expected the write to WAIT "
                 "for the lock, took only %1ms").arg(new_ms)));
    }
};

QTEST_MAIN(SqlResultTest)
#include "tst_sql_result.moc"
