#pragma once
#include <QString>
#include <QStringList>
namespace openmarketterminal::cli {
// Run the daemon for `profile`. Blocks in the event loop until SIGTERM/SIGINT
// or a fatal init error. Returns a process exit code (0 clean, 3 already-owned,
// 7 init failure). status()/stop() are the management subcommands.
int serve_run(const QString& profile);
int serve_status(const QString& profile, bool json);
int serve_stop(const QString& profile);
int daemon_command(const QString& profile, bool json, QStringList args);
int sync_command(const QString& profile, bool json, QStringList args);
}
