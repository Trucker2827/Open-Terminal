#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace openmarketterminal::cli::automation {

// All state lives under <profile root>/daemon. Task 1: profile is accepted but
// resolution still matches the daemon's profile_root_for(); see AutomationState.cpp.
QString state_dir(const QString& profile);
QString live_guard_path(const QString& profile);
QString decisions_path(const QString& profile);
QString orders_path(const QString& profile);
QString consumed_path(const QString& profile);

QJsonObject read_json_object(const QString& path);
bool write_json_object(const QString& path, const QJsonObject& o, QString* error = nullptr);
bool append_jsonl(const QString& path, const QJsonObject& o, QString* error = nullptr);

// Read only the last max_bytes of a jsonl file. When the file exceeds
// max_bytes, the result starts at the first complete line (a leading
// partial line, if any, is dropped).
inline constexpr qint64 kTailBytes = 512 * 1024;
QByteArray read_tail(const QString& path, qint64 max_bytes = kTailBytes);

QJsonObject latest_candidate(const QString& profile, const QString& symbol_filter,
                             int max_age_sec, QString* error = nullptr);
int submitted_today_count(const QString& profile);

}  // namespace openmarketterminal::cli::automation
