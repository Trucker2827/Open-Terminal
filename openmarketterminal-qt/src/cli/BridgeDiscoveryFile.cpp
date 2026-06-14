#include "cli/BridgeDiscoveryFile.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#  include <windows.h>
#else
#  include <csignal>
#endif

namespace openmarketterminal::cli {

// Mirror of AppPaths::root() — keep identical to src/core/config/AppPaths.cpp.
static QString app_root() {
#if defined(Q_OS_WIN)
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + "/org.openterminal.OpenTerminal";
#elif defined(Q_OS_MACOS)
    return QDir::homePath() + "/Library/Application Support/org.openterminal.OpenTerminal";
#else
    return QDir::homePath() + "/.local/share/org.openterminal.OpenTerminal";
#endif
}

QString profile_root_for(const QString& profile) {
    return (profile.isEmpty() || profile == "default")
               ? app_root()
               : app_root() + "/profiles/" + profile;
}

QString bridge_file_path(const QString& profile_root) {
    return profile_root + "/bridge.json";
}

bool write_bridge_file(const QString& profile_root, const BridgeInfo& info) {
    QDir().mkpath(profile_root);
    QJsonObject o;
    o["schema"] = 1;
    o["endpoint"] = info.endpoint;
    o["token"] = info.token;
    o["pid"] = info.pid;
    o["started_at"] = info.started_at;
    const QString path = bridge_file_path(profile_root);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    f.close();
    return QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
}

bool remove_bridge_file(const QString& profile_root) {
    const QString path = bridge_file_path(profile_root);
    if (!QFile::exists(path))
        return true;
    return QFile::remove(path);
}

std::optional<BridgeInfo> read_bridge_file(const QString& profile_root) {
    QFile f(bridge_file_path(profile_root));
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return std::nullopt;
    const QJsonObject o = doc.object();
    if (!o.contains("endpoint") || !o.contains("token"))
        return std::nullopt;
    BridgeInfo info;
    info.endpoint = o.value("endpoint").toString();
    info.token = o.value("token").toString();
    info.pid = static_cast<qint64>(o.value("pid").toDouble());
    info.started_at = o.value("started_at").toString();
    if (info.endpoint.isEmpty() || info.token.isEmpty())
        return std::nullopt;
    return info;
}

bool is_pid_alive(qint64 pid) {
    if (pid <= 0)
        return false;
#ifdef Q_OS_WIN
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0; GetExitCodeProcess(h, &code); CloseHandle(h);
    return code == STILL_ACTIVE;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

} // namespace openmarketterminal::cli
