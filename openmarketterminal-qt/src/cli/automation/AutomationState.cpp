#include "cli/automation/AutomationState.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

#include "cli/BridgeDiscoveryFile.h"

namespace openmarketterminal::cli::automation {

QString state_dir(const QString& profile) {
    const QString dir = profile_root_for(profile) + QStringLiteral("/daemon");
    QDir().mkpath(dir);
    return dir;
}

QString live_guard_path(const QString& profile) {
    return state_dir(profile) + QStringLiteral("/automation_live_guard.json");
}
QString decisions_path(const QString& profile) {
    return state_dir(profile) + QStringLiteral("/scalp_decisions.jsonl");
}
QString orders_path(const QString& profile) {
    return state_dir(profile) + QStringLiteral("/automation_orders.jsonl");
}
QString consumed_path(const QString& profile) {
    return state_dir(profile) + QStringLiteral("/automation_consumed.json");
}
QString daily_orders_path(const QString& profile) {
    return state_dir(profile) + QStringLiteral("/automation_daily_orders.json");
}

QJsonObject read_json_object(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    return pe.error == QJsonParseError::NoError && doc.isObject() ? doc.object() : QJsonObject{};
}

bool write_json_object(const QString& path, const QJsonObject& o, QString* error) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("could not write %1").arg(path);
        return false;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error) *error = QStringLiteral("could not commit %1").arg(path);
        return false;
    }
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

StateLock::StateLock(const QString& profile, int timeout_ms)
    : lock_(state_dir(profile) + QStringLiteral("/automation.lock")) {
    locked_ = lock_.tryLock(timeout_ms);
}

StateLock::~StateLock() {
    if (locked_)
        lock_.unlock();
}

bool StateLock::locked() const {
    return locked_;
}

bool append_jsonl(const QString& path, const QJsonObject& o, QString* error) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (error) *error = QStringLiteral("could not append %1").arg(path);
        return false;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    f.write("\n");
    return true;
}

bool append_jsonl_rotating(const QString& path, const QJsonObject& o,
                           qint64 max_bytes, QString* error) {
    if (QFileInfo::exists(path) && QFileInfo(path).size() >= max_bytes) {
        QFile::remove(path + QStringLiteral(".1"));
        if (!QFile::rename(path, path + QStringLiteral(".1"))) {
            if (error) *error = QStringLiteral("could not rotate %1").arg(path);
            return false;
        }
    }
    return append_jsonl(path, o, error);
}

QByteArray read_tail(const QString& path, qint64 max_bytes) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const qint64 size = f.size();
    if (size > max_bytes) {
        f.seek(size - max_bytes);
        QByteArray data = f.readAll();
        const int nl = data.indexOf('\n');
        return nl >= 0 ? data.mid(nl + 1) : QByteArray{};  // drop leading partial line
    }
    return f.readAll();
}

// Reads the active file's last tail_bytes; when the active file is smaller
// than tail_bytes (e.g. right after a rotation, Task 5), the remaining
// budget is filled from the previous generation's tail (path + ".1") so a
// row that rotated out just before the active file caught up does not fall
// into the blind spot. The previous generation's lines are placed FIRST in
// the returned buffer and the active file's LAST, so a newest-first
// reverse scan of the result still visits the active file's rows before
// the previous generation's -- the active file wins on ties. Shared by
// latest_candidate (decisions jsonl) and submitted_today_scan (orders
// jsonl) so the "just rotated" blind spot is closed identically for both.
static QByteArray scan_buffer_with_prev(const QString& path, qint64 tail_bytes) {
    const qint64 active_size = QFileInfo(path).size();
    const QByteArray active_tail = read_tail(path, tail_bytes);
    if (active_size < tail_bytes)
        return read_tail(path + QStringLiteral(".1"), tail_bytes - active_size) + active_tail;
    return active_tail;
}

QString candidate_key(const QJsonObject& decision) {
    const QString id = decision.value(QStringLiteral("id")).toString();
    if (!id.isEmpty())
        return id;
    return decision.value(QStringLiteral("symbol")).toString().trimmed().toUpper() +
           QLatin1Char('|') + decision.value(QStringLiteral("ts_ms")).toString();
}

bool is_consumed(const QString& profile, const QString& key) {
    return read_json_object(consumed_path(profile))
        .value(QStringLiteral("keys")).toObject().contains(key);
}

bool mark_consumed(const QString& profile, const QString& key, QString* error, int lock_timeout_ms) {
    StateLock lock(profile, lock_timeout_ms);
    if (!lock.locked()) {
        if (error) *error = QStringLiteral("state lock busy");
        return false;
    }
    QJsonObject doc = read_json_object(consumed_path(profile));
    QJsonObject keys = doc.value(QStringLiteral("keys")).toObject();
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addSecs(-48 * 3600);
    QJsonObject pruned;
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        const QDateTime ts = QDateTime::fromString(it.value().toString(), Qt::ISODateWithMs);
        if (ts.isValid() && ts >= cutoff)
            pruned.insert(it.key(), it.value());
    }
    pruned.insert(key, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    doc[QStringLiteral("keys")] = pruned;
    return write_json_object(consumed_path(profile), doc, error);
}

QJsonObject latest_candidate(const QString& profile, const QString& symbol_filter,
                             int max_age_sec, QString* error) {
    const QString path = decisions_path(profile);
    const QString prev_path = path + QStringLiteral(".1");
    if (!QFile::exists(path) && !QFile::exists(prev_path)) {
        if (error) *error = QStringLiteral("no paper decisions yet; start automation and daemon first");
        return {};
    }
    // The active file's last kTailBytes are scanned for candidates. Right
    // after a rotation (Task 5) the active file can be much smaller than
    // kTailBytes, and a still-fresh candidate can be sitting in the
    // just-rotated previous generation (path + ".1"). When that happens,
    // the remaining budget is filled from that previous generation's tail
    // so the blind spot closes. The previous generation's lines are placed
    // FIRST in the scan buffer and the active file's LAST, so the
    // newest-first reverse scan below still visits the active file's rows
    // before the previous generation's -- the active file wins whenever
    // both contain a fresh match. Candidates older than this combined
    // window (active + one prior generation) remain invisible, but since
    // candidates expire after <= max_age_sec (<= 3600s) anyway, that bound
    // is acceptable.
    const QByteArray scan_buffer = scan_buffer_with_prev(path, kTailBytes);
    const QList<QByteArray> lines = scan_buffer.split('\n');
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const QString filter = symbol_filter.trimmed().toUpper();
    // Read the consumed set once instead of re-reading the file per row.
    const QJsonObject consumed_keys =
        read_json_object(consumed_path(profile)).value(QStringLiteral("keys")).toObject();
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QByteArray line = it->trimmed();
        if (line.isEmpty())
            continue;
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        const QJsonObject d = doc.object();
        const QString symbol = d.value(QStringLiteral("symbol")).toString().trimmed().toUpper();
        if (!filter.isEmpty() && symbol != filter)
            continue;
        if (d.value(QStringLiteral("verdict")).toString() != QLatin1String("PAPER TRADE CANDIDATE"))
            continue;
        if (d.value(QStringLiteral("action")).toString() != QLatin1String("PAPER_LIMIT_BUY_ONLY"))
            continue;
        bool ok = false;
        const qint64 ts_ms = d.value(QStringLiteral("ts_ms")).toString().toLongLong(&ok);
        if (!ok || ts_ms <= 0 || now_ms - ts_ms > static_cast<qint64>(max_age_sec) * 1000)
            continue;
        if (consumed_keys.contains(candidate_key(d)))
            continue;
        return d;
    }
    if (error) *error = QStringLiteral("no fresh approved paper candidate found");
    return {};
}

// Legacy tail scan of the orders jsonl: counts submitted orders whose "ts"
// falls on the given UTC day. Blind to lines that scrolled out of the
// combined active + previous-generation window, so it only serves as a
// fallback/seed for the dedicated daily counter file.
static int submitted_today_scan(const QString& profile, const QString& today) {
    int count = 0;
    for (const QByteArray& raw : scan_buffer_with_prev(orders_path(profile), kTailBytes).split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty())
            continue;
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        const QJsonObject o = doc.object();
        if (!o.value(QStringLiteral("submitted")).toBool())
            continue;
        if (o.value(QStringLiteral("ts")).toString().startsWith(today))
            ++count;
    }
    return count;
}

int horizon_seconds(const QString& horizon) {
    const QString h = horizon.trimmed().toLower();
    if (h.isEmpty())
        return 0;
    bool ok = false;
    const double n = h.left(h.size() - 1).toDouble(&ok);
    if (!ok || n <= 0)
        return 0;
    switch (h.back().toLatin1()) {
        case 's': return static_cast<int>(n);
        case 'm': return static_cast<int>(n * 60);
        case 'h': return static_cast<int>(n * 3600);
        case 'd': return static_cast<int>(n * 86400);
        default: return 0;
    }
}

bool spot_row_passes(const QString& horizon, double edge_after_cost_fraction,
                     double confidence, double min_edge_fraction, double min_confidence) {
    if (horizon_seconds(horizon) < 60)
        return false;
    return edge_after_cost_fraction >= min_edge_fraction && confidence >= min_confidence;
}

int submitted_today_count(const QString& profile) {
    const QString today = QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate);
    // The dedicated daily counter is authoritative when it exists and is
    // fresh (dated today): unlike the tail scan, it is never blind to
    // entries that scrolled out of the 512 KB window on a chatty journal.
    // Absent or stale (previous day's) counters fall back to the legacy
    // tail scan so behavior is unchanged for profiles that predate Task 4.
    const QString counter_path = daily_orders_path(profile);
    if (QFile::exists(counter_path)) {
        const QJsonObject counter = read_json_object(counter_path);
        if (counter.value(QStringLiteral("date")).toString() == today)
            return counter.value(QStringLiteral("count")).toInt();
    }
    return submitted_today_scan(profile, today);
}

bool record_live_attempt(const QString& profile, QString* error, int lock_timeout_ms) {
    StateLock lock(profile, lock_timeout_ms);
    if (!lock.locked()) {
        if (error) *error = QStringLiteral("state lock busy");
        return false;
    }
    const QString today = QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate);
    const QString path = daily_orders_path(profile);
    QJsonObject doc = read_json_object(path);
    // On the first counter write of a day (absent file or stale date), seed
    // from the journal scan so live orders recorded before the counter
    // existed still count against the daily cap. Without this, deploy-day
    // attempt N+1 would restart the cap at 1.
    int count = doc.value(QStringLiteral("date")).toString() == today
                    ? doc.value(QStringLiteral("count")).toInt()
                    : submitted_today_scan(profile, today);
    doc[QStringLiteral("date")] = today;
    doc[QStringLiteral("count")] = ++count;
    return write_json_object(path, doc, error);
}

}  // namespace openmarketterminal::cli::automation
