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

QJsonObject latest_candidate(const QString& profile, const QString& symbol_filter,
                             int max_age_sec, QString* error) {
    const QString path = decisions_path(profile);
    if (!QFile::exists(path)) {
        if (error) *error = QStringLiteral("no paper decisions yet; start automation and daemon first");
        return {};
    }
    // Only the last kTailBytes of the file are scanned, so candidates older
    // than that window are invisible here. Acceptable because candidates
    // expire in <= max_age_sec (<= 3600s) anyway, and Task 5 caps file size
    // via rotation so the window always covers recent activity.
    const QList<QByteArray> lines = read_tail(path, kTailBytes).split('\n');
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const QString filter = symbol_filter.trimmed().toUpper();
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
        return d;
    }
    if (error) *error = QStringLiteral("no fresh approved paper candidate found");
    return {};
}

int submitted_today_count(const QString& profile) {
    const QString today = QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate);
    int count = 0;
    for (const QByteArray& raw : read_tail(orders_path(profile), kTailBytes).split('\n')) {
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

}  // namespace openmarketterminal::cli::automation
