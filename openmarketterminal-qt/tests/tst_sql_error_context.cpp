// A database error must name the statement that failed.
//
// The production message was "database is locked Unable to fetch row" and
// nothing more, while `database is locked` fired thousands of times a day
// across several jobs. Without the statement there was no way to tell which
// query was losing.

#include <QtTest/QtTest>

#include "storage/sqlite/SqlErrorContext.h"

using namespace openmarketterminal::storage::sqlite;

class SqlErrorContextTest : public QObject {
    Q_OBJECT

private slots:
    void the_error_names_the_statement() {
        const QString out = sql_error_with_context(
            QStringLiteral("database is locked Unable to fetch row"),
            QStringLiteral("SELECT position_id FROM sandbox_position WHERE strategy_id = ?"));
        QVERIFY2(out.contains(QStringLiteral("database is locked")), qUtf8Printable(out));
        QVERIFY2(out.contains(QStringLiteral("sandbox_position")), qUtf8Printable(out));
    }

    void a_multiline_statement_becomes_one_line() {
        const QString out = sql_error_with_context(
            QStringLiteral("boom"),
            QStringLiteral("SELECT a\n  FROM t\n  WHERE b = ?"));
        QVERIFY2(!out.contains(QLatin1Char('\n')), qUtf8Printable(out));
        QVERIFY2(out.contains(QStringLiteral("SELECT a FROM t WHERE b = ?")), qUtf8Printable(out));
    }

    void a_long_statement_is_truncated_not_dropped() {
        const QString longsql = QStringLiteral("SELECT ") + QString(400, QLatin1Char('x'));
        const QString excerpt = sql_excerpt(longsql);
        QCOMPARE(excerpt.size(), kSqlExcerptMaxChars + 1);  // + the ellipsis
        QVERIFY(excerpt.endsWith(QStringLiteral("…")));
        QVERIFY2(excerpt.startsWith(QStringLiteral("SELECT ")), qUtf8Printable(excerpt));
    }

    void a_short_statement_is_untouched() {
        QCOMPARE(sql_excerpt(QStringLiteral("SELECT 1")), QStringLiteral("SELECT 1"));
    }

    void no_statement_means_no_dangling_separator() {
        const QString out = sql_error_with_context(QStringLiteral("no connection"), QString());
        QCOMPARE(out, QStringLiteral("no connection"));
        QVERIFY(!out.contains(QStringLiteral("sql:")));
    }

    void bound_values_are_never_added_by_this_helper() {
        // The helper only ever sees the statement. Placeholders must survive as
        // placeholders -- an error string reaches logs and job history, and
        // position/account values must not ride along.
        const QString out = sql_error_with_context(
            QStringLiteral("boom"),
            QStringLiteral("INSERT INTO sandbox_fill (position_id, qty) VALUES (?, ?)"));
        QVERIFY2(out.contains(QStringLiteral("VALUES (?, ?)")), qUtf8Printable(out));
    }
};

QTEST_MAIN(SqlErrorContextTest)
#include "tst_sql_error_context.moc"
