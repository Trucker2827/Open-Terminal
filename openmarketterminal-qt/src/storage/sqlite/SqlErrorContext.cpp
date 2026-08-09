#include "storage/sqlite/SqlErrorContext.h"

namespace openmarketterminal::storage::sqlite {

QString sql_excerpt(const QString& sql) {
    const QString flat = sql.simplified();
    if (flat.size() <= kSqlExcerptMaxChars)
        return flat;
    return flat.left(kSqlExcerptMaxChars) + QStringLiteral("…");
}

QString sql_error_with_context(const QString& driver_message, const QString& sql) {
    const QString excerpt = sql_excerpt(sql);
    if (excerpt.isEmpty())
        return driver_message;
    return driver_message + QStringLiteral(" | sql: ") + excerpt;
}

}  // namespace openmarketterminal::storage::sqlite
