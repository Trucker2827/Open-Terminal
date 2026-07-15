#include "services/sandbox/TickTail.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <algorithm>

namespace openmarketterminal::services::sandbox {

namespace {

// Copy of automation::read_tail's exact semantics (src/cli/automation/
// AutomationState.cpp): read only the last max_bytes of a file; when the
// file is bigger than that, seek in and drop the leading partial line up to
// (and including) the first '\n'. Duplicated here rather than shared because
// AutomationState.cpp is compiled into the CLI targets, not into
// openterminal_core -- see the layering note atop TickTail.h.
QByteArray read_tail(const QString& path, qint64 max_bytes) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const qint64 size = f.size();
    if (size > max_bytes) {
        f.seek(size - max_bytes);
        QByteArray data = f.readAll();
        const int nl = data.indexOf('\n');
        return nl >= 0 ? data.mid(nl + 1) : QByteArray{};
    }
    return f.readAll();
}

} // namespace

// Copy of automation::latest_candidate's active+prev blending: when the
// active file is shorter than tail_bytes, fill the remaining budget from the
// tail of the ".1" rotation predecessor, placed BEFORE the active file's
// bytes so overall ordering stays oldest-to-newest. Promoted out of this
// file's anonymous namespace (Task 5) so PaperExecutor.cpp can reuse it to
// tail-scan scalp_decisions.jsonl instead of duplicating a third copy.
QByteArray read_tail_with_prev(const QString& path, qint64 tail_bytes) {
    const qint64 active_size = QFileInfo(path).size();
    const QByteArray active_tail = read_tail(path, tail_bytes);
    if (active_size < tail_bytes) {
        const QByteArray prev_tail = read_tail(path + QStringLiteral(".1"), tail_bytes - active_size);
        return prev_tail + active_tail;
    }
    return active_tail;
}

QVector<TickRow> ticks_since(const QString& ticks_path, const QString& symbol, qint64 since_ms,
                              qint64 tail_bytes, const QString& venue) {
    const QByteArray buffer = read_tail_with_prev(ticks_path, tail_bytes);
    const QString symbol_filter = symbol.trimmed().toUpper();
    const QString venue_filter = venue.trimmed().toLower();

    QVector<TickRow> rows;
    for (const QByteArray& raw : buffer.split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty())
            continue;

        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject())
            continue;

        const QJsonObject o = doc.object();
        const QString row_symbol = o.value(QStringLiteral("symbol")).toString().trimmed().toUpper();
        if (row_symbol != symbol_filter)
            continue;
        const QString row_venue = o.value(QStringLiteral("venue")).toString().trimmed().toLower();
        if (!venue_filter.isEmpty() && row_venue != venue_filter)
            continue;

        const double price = o.value(QStringLiteral("price")).toDouble();
        if (price <= 0)
            continue;

        bool ts_ok = false;
        const qint64 ts_ms = o.value(QStringLiteral("received_ts_ms")).toString().toLongLong(&ts_ok);
        if (!ts_ok || ts_ms <= 0)
            continue;

        if (ts_ms <= since_ms)
            continue;

        TickRow row;
        row.symbol = row_symbol;
        row.venue = row_venue;
        row.price = price;
        row.best_bid = o.value(QStringLiteral("best_bid")).toDouble();
        row.best_ask = o.value(QStringLiteral("best_ask")).toDouble();
        row.ts_ms = ts_ms;
        rows.append(row);
    }

    // File is append-ordered so rows should already be ascending by ts_ms;
    // stable_sort is cheap insurance against any out-of-order writers.
    std::stable_sort(rows.begin(), rows.end(),
                      [](const TickRow& a, const TickRow& b) { return a.ts_ms < b.ts_ms; });
    return rows;
}

} // namespace openmarketterminal::services::sandbox
