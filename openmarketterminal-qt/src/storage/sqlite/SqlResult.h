#pragma once

// A fully-read query result. No live cursor, ever.
//
// Database::execute() used to hand back the QSqlQuery itself, so every caller
// held an open statement for as long as it kept the Result alive. SQLite keeps
// a read transaction open on a connection while any statement on it is live,
// and a write issued in that state must upgrade that read transaction --
// which SQLite refuses with SQLITE_BUSY *immediately, without consulting
// busy_timeout*, because waiting could deadlock. Measured: a write with a
// cursor open failed in 0.00s, the same write after the cursor closed waited
// the full 5.18s and would have succeeded had the lock freed.
//
// In production that was `sandbox tick failed: database is locked` on
// INSERT INTO sandbox_position, at up to 89% of runs.
//
// Three narrower fixes failed. Draining each loop, then calling finish() on
// individual statements, both left the failure in place -- with 408 execute()
// call sites, any one still holding a statement re-opens the read
// transaction. Giving writes their own connection did fix the lock error but
// introduced a WORSE, silent bug: a row written on the write connection was
// invisible to a read connection pinned to an older WAL snapshot, leaving a
// maker quote stuck in `pending_fill` with nothing in any log.
//
// Buffering removes the cursor instead of working around it. One connection,
// so no cross-connection snapshot can go stale; no live statement, so no
// upgrade can occur. The method surface deliberately mirrors QSqlQuery so
// existing callers read unchanged.

#include <QSqlQuery>
#include <QSqlRecord>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVector>

namespace openmarketterminal::storage::sqlite {

class SqlResult {
  public:
    SqlResult() = default;

    /// Reads every row out of `query`, then lets it go. After this returns,
    /// no statement is live for this result.
    explicit SqlResult(QSqlQuery& query);

    /// Advance to the next row. Mirrors QSqlQuery::next(), including starting
    /// "before the first row" so `while (r.next())` walks the whole result.
    bool next();

    /// Position on the first row. False when the result is empty.
    bool first();

    /// Current row's column `index`. Invalid QVariant when out of range or
    /// when positioned before the first row -- same shape as QSqlQuery.
    QVariant value(int index) const;

    /// Current row's column by name.
    QVariant value(const QString& name) const;

    /// True when the column holds SQL NULL. Distinct from a zero or empty
    /// value -- callers use this to tell "no P&L yet" from "P&L of 0.00".
    bool isNull(int index) const { return value(index).isNull(); }

    int numRowsAffected() const { return rows_affected_; }
    QVariant lastInsertId() const { return last_insert_id_; }
    QSqlRecord record() const { return record_; }

    /// Row count. Unlike QSqlQuery on SQLite (which returns -1), this is
    /// always known, because every row has already been read.
    int size() const { return static_cast<int>(rows_.size()); }
    bool isEmpty() const { return rows_.isEmpty(); }

    /// True when positioned on a real row.
    bool isValid() const { return at_ >= 0 && at_ < rows_.size(); }
    int at() const { return at_; }

  private:
    QVector<QVariantList> rows_;
    QSqlRecord record_;
    QVariant last_insert_id_;
    int rows_affected_ = -1;
    int at_ = -1;  // QSql::BeforeFirstRow
};

}  // namespace openmarketterminal::storage::sqlite
