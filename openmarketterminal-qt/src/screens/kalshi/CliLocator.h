#pragma once

// Locates the bundled openterminalcli binary beside the GUI executable.
// Extracted from KalshiScreen::cli_path() so the probing — including the
// Windows ".exe" suffix — is unit-testable on any host: the suffix is a
// parameter instead of a compile-time branch inside the probe itself.

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace openmarketterminal::screens::kalshi {

// Executable suffix for the platform the GUI is running on.
inline QString cli_exe_suffix() {
#ifdef Q_OS_WIN
    return QStringLiteral(".exe");
#else
    return {};
#endif
}

// On-disk candidates for the bundled CLI, in probe order: directly beside
// the app executable, then three levels up (Contents/MacOS → beside the
// .app bundle on macOS).
inline QStringList cli_candidate_paths(const QString& app_dir, const QString& suffix) {
    const QString name = QStringLiteral("openterminalcli") + suffix;
    return {app_dir + QLatin1Char('/') + name,
        QDir::cleanPath(app_dir + QStringLiteral("/../../../") + name)};
}

// First candidate that exists and is executable; empty when none is —
// missing reads as missing, the caller reports "not found".
inline QString find_cli_beside_app(const QString& app_dir, const QString& suffix) {
    const QStringList candidates = cli_candidate_paths(app_dir, suffix);
    for (const QString& path : candidates)
        if (!path.isEmpty() && QFileInfo(path).isExecutable()) return path;
    return {};
}

}  // namespace openmarketterminal::screens::kalshi
