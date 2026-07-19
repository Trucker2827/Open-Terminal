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
bool kalshi_planner_process_timed_out(bool active, qint64 process_age_ms,
                                      qint64 timeout_ms);
bool kalshi_non_execution_process_timed_out(bool active, qint64 process_age_ms,
                                            qint64 timeout_ms);
qint64 kalshi_event_cycle_delay_ms(bool live_session_active, bool paper_active,
                                   qint64 elapsed_ms);
// The event engine operates on the nearest live crypto settlement cohorts. Keep
// the child planner bounded enough to finish before another quote snapshot ages
// out; this is deliberately separate from the broader manual CLI planner.
QStringList kalshi_event_planner_args();
// Only independent exchange ticks are eligible to keep the Kalshi planner's
// spot reference fresh. Persistence is rate-limited so the daemon gets a
// current executable reference without turning the feature store into a raw
// websocket dump.
bool kalshi_should_persist_independent_spot_tick(const QString& source, double price,
                                                 qint64 received_ts_ms,
                                                 qint64 last_persisted_ts_ms,
                                                 qint64 minimum_interval_ms);

// Pure daemon-job-spec -> CLI-args builder, kept public for deterministic
// regression tests.
QStringList command_for_job_kind(const QString& kind, const QJsonObject& spec);
}
