#pragma once

// Put the failing statement into the error.
//
// Database::execute() returned only query.lastError().text(), so a production
// failure read, in full:
//
//     sandbox tick failed: database is locked Unable to fetch row
//
// That names a symptom and nothing else. The statement was in scope at the
// point of failure and was discarded, which cost an entire investigation:
// `database is locked` on this machine fires thousands of times a day across
// several jobs, and there was no way to tell which query was losing.
//
// Bound parameter VALUES are deliberately never included -- those carry
// account and position data, and an error string ends up in logs, job history
// and the daemon's stderr. The statement text is schema, not data.

#include <QString>

namespace openmarketterminal::storage::sqlite {

// Longest statement excerpt carried in an error. Long enough to identify the
// query, short enough not to bury the driver's own message.
inline constexpr int kSqlExcerptMaxChars = 160;

// One line, whitespace collapsed, truncated with a trailing ellipsis.
QString sql_excerpt(const QString& sql);

// "<driver message> | sql: <excerpt>". Falls back to the driver message alone
// when there is no statement to report, so callers never emit a dangling
// separator.
QString sql_error_with_context(const QString& driver_message, const QString& sql);

}  // namespace openmarketterminal::storage::sqlite
