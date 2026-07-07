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
QString daily_orders_path(const QString& profile);

QJsonObject read_json_object(const QString& path);
bool write_json_object(const QString& path, const QJsonObject& o, QString* error = nullptr);
bool append_jsonl(const QString& path, const QJsonObject& o, QString* error = nullptr);

// Rotating jsonl append: when the file reaches max_bytes, the previous
// generation (path + ".1") is dropped and the current file is renamed into
// its place before the new row starts a fresh file. Caps the active file's
// growth; only one prior generation is retained (older data is dropped by
// design -- archival, if ever needed, is a separate concern).
inline constexpr qint64 kRotateBytes = 64LL * 1024 * 1024;
bool append_jsonl_rotating(const QString& path, const QJsonObject& o,
                           qint64 max_bytes = kRotateBytes, QString* error = nullptr);

// Read only the last max_bytes of a jsonl file. When the file exceeds
// max_bytes, the result starts at the first complete line (a leading
// partial line, if any, is dropped).
inline constexpr qint64 kTailBytes = 512 * 1024;
QByteArray read_tail(const QString& path, qint64 max_bytes = kTailBytes);

// Candidate dedup: key is the spot journal row's "id" if present, else
// "<SYMBOL>|<ts_ms>" (scalp candidates are journaled at most once per
// second per symbol, so the pair is unique).
QString candidate_key(const QJsonObject& decision);
bool is_consumed(const QString& profile, const QString& key);
bool mark_consumed(const QString& profile, const QString& key, QString* error = nullptr);

QJsonObject latest_candidate(const QString& profile, const QString& symbol_filter,
                             int max_age_sec, QString* error = nullptr);

// Parses horizon strings like "15s", "60s", "1h", "4h", "1d" into seconds.
// Unparseable input (including empty) returns 0.
int horizon_seconds(const QString& horizon);

// Pure spot-candidate row filter: rejects horizons under 60s (e.g. the
// 15-second scalp-gate rows that share source='edge crypto-recommend')
// and applies the edge/confidence gate. edge_after_cost_fraction and
// min_edge_fraction are probability-edge fractions (model_prob - 0.5),
// not price-return bps.
bool spot_row_passes(const QString& horizon, double edge_after_cost_fraction,
                     double confidence, double min_edge_fraction, double min_confidence);

int submitted_today_count(const QString& profile);
// Dedicated daily live-order counter (authoritative over the orders jsonl
// tail scan, which can undercount when the journal is chatty). Records a
// live submission *attempt*, matching mark_consumed's conservative timing.
bool record_live_attempt(const QString& profile, QString* error = nullptr);

}  // namespace openmarketterminal::cli::automation
