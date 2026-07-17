#pragma once
#include <QString>
#include <QStringList>
#include <QJsonObject>
namespace openmarketterminal::cli {
// Run the daemon for `profile`. Blocks in the event loop until SIGTERM/SIGINT
// or a fatal init error. Returns a process exit code (0 clean, 3 already-owned,
// 7 init failure). status()/stop() are the management subcommands.
int serve_run(const QString& profile);
int serve_status(const QString& profile, bool json);
int serve_stop(const QString& profile);
int daemon_command(const QString& profile, bool json, QStringList args);
int sync_command(const QString& profile, bool json, QStringList args);

// Pure watchdog predicates kept public for deterministic regression tests.
bool kalshi_event_stream_needs_recovery(bool workload_active, bool connected,
                                        int subscribed_assets, qint64 event_age_ms,
                                        qint64 stale_after_ms);
bool kalshi_universe_request_timed_out(bool pending, qint64 request_age_ms,
                                      qint64 timeout_ms);

// Pure daemon-job-spec -> CLI-args builder, kept public for deterministic
// regression tests.
QStringList command_for_job_kind(const QString& kind, const QJsonObject& spec);
}
