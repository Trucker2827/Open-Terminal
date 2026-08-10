#include "storage/sqlite/SqlResult.h"

namespace openmarketterminal::storage::sqlite {

SqlResult::SqlResult(QSqlQuery& query) {
    // Capture the metadata first: numRowsAffected/lastInsertId are only
    // meaningful before the statement is walked or released.
    rows_affected_ = query.numRowsAffected();
    last_insert_id_ = query.lastInsertId();
    record_ = query.record();

    const int columns = record_.count();
    while (query.next()) {
        QVariantList row;
        row.reserve(columns);
        for (int i = 0; i < columns; ++i)
            row.append(query.value(i));
        rows_.append(row);
    }
    // Release the statement now rather than waiting for the caller to drop the
    // result. This is the line the whole class exists for: while it is live,
    // SQLite holds a read transaction on the connection and any write must
    // upgrade it.
    query.finish();
}

bool SqlResult::next() {
    if (at_ + 1 >= rows_.size()) {
        at_ = rows_.size();  // AfterLastRow — further next() calls stay false
        return false;
    }
    ++at_;
    return true;
}

bool SqlResult::first() {
    if (rows_.isEmpty())
        return false;
    at_ = 0;
    return true;
}

QVariant SqlResult::value(int index) const {
    if (!isValid() || index < 0 || index >= rows_.at(at_).size())
        return {};
    return rows_.at(at_).at(index);
}

QVariant SqlResult::value(const QString& name) const {
    const int index = record_.indexOf(name);
    return index < 0 ? QVariant{} : value(index);
}

}  // namespace openmarketterminal::storage::sqlite
