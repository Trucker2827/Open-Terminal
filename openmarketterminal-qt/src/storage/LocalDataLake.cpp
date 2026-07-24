#include "storage/LocalDataLake.h"

#include "core/profile/ProfilePaths.h"
#include "storage/repositories/EdgePredictionModelRepository.h"
#include "storage/repositories/TradeAuditRepository.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>

#include <algorithm>

namespace openmarketterminal::storage {

namespace {

QString sanitize_name(QString value) {
    value = value.trimmed().toLower();
    value.replace('\\', '_');
    value.replace('/', '_');
    value.replace(' ', '_');
    if (value.isEmpty())
        value = QStringLiteral("default");
    return value;
}

QJsonObject dataset_status(const QString& root, const QString& dataset) {
    const QDir dir(root + QDir::separator() + dataset);
    qint64 bytes = 0;
    qint64 orphaned_temp_bytes = 0;
    int files = 0;
    int orphaned_temp_files = 0;
    qint64 last_modified = 0;
    if (dir.exists()) {
        const auto entries = dir.entryInfoList(QDir::Files | QDir::Readable, QDir::Name);
        for (const QFileInfo& info : entries) {
            const QString name = info.fileName();
            if (name.endsWith(QStringLiteral(".jsonl"))) {
                ++files;
                bytes += info.size();
                last_modified = std::max<qint64>(last_modified, info.lastModified().toMSecsSinceEpoch());
            } else if (name.contains(QStringLiteral(".jsonl.")) &&
                       !name.endsWith(QStringLiteral(".jsonl.lock"))) {
                // QSaveFile uses this naming shape for interrupted replacements.
                // Do not delete evidence automatically; surface it for an explicit
                // operator cleanup after the writer has been made serial.
                ++orphaned_temp_files;
                orphaned_temp_bytes += info.size();
            }
        }
    }
    return QJsonObject{{"dataset", dataset},
                       {"path", dir.absolutePath()},
                       {"files", files},
                       {"bytes", QString::number(bytes)},
                       {"orphaned_temp_files", orphaned_temp_files},
                       {"orphaned_temp_bytes", QString::number(orphaned_temp_bytes)},
                       {"total_bytes", QString::number(bytes + orphaned_temp_bytes)},
                       {"last_modified_ms", QString::number(last_modified)}};
}

constexpr qint64 kInterruptedArtifactMinimumAgeMs = 5 * 60 * 1000;

struct InterruptedArtifactScan {
    struct Candidate {
        QString path;
        qint64 bytes = 0;
    };
    QVector<Candidate> candidates;
    int recent_files = 0;
    qint64 recent_bytes = 0;
};

InterruptedArtifactScan interrupted_artifact_scan(const QString& root) {
    InterruptedArtifactScan scan;
    if (!QFileInfo::exists(root))
        return scan;

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    QDirIterator it(root, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo info(it.next());
        const QString name = info.fileName();
        if (!name.contains(QStringLiteral(".jsonl.")) ||
            name.endsWith(QStringLiteral(".jsonl.lock")))
            continue;
        if (now_ms - info.lastModified().toMSecsSinceEpoch() < kInterruptedArtifactMinimumAgeMs) {
            ++scan.recent_files;
            scan.recent_bytes += info.size();
            continue;
        }
        scan.candidates.append({info.absoluteFilePath(), info.size()});
    }
    return scan;
}

QString interrupted_artifact_target_path(const QString& artifact_path) {
    const int marker = artifact_path.lastIndexOf(QStringLiteral(".jsonl."));
    return marker < 0 ? QString() : artifact_path.left(marker + QStringLiteral(".jsonl").size());
}

QJsonArray cleanup_preview_rows(const QVector<InterruptedArtifactScan::Candidate>& candidates) {
    QJsonArray rows;
    // Keep the safe preview useful in a terminal. The count and byte total
    // remain exact; this is just a representative, inspectable sample.
    constexpr int kPreviewLimit = 20;
    for (int index = 0; index < candidates.size() && index < kPreviewLimit; ++index) {
        const auto& candidate = candidates.at(index);
        rows.append(QJsonObject{{"path", candidate.path}, {"bytes", QString::number(candidate.bytes)}});
    }
    return rows;
}

QJsonObject source_doc(const QString& source, const QString& details) {
    return QJsonObject{{"source", source}, {"details", details}};
}

bool replace_jsonl_file(const QString& path, const QJsonArray& rows, QString* error) {
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(5 * 60 * 1000);
    if (!lock.tryLock(5000)) {
        if (error)
            *error = QStringLiteral("data lake write lock unavailable for %1: %2")
                         .arg(path, lock.error());
        return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("failed to open %1: %2").arg(path, f.errorString());
        return false;
    }
    const QString ingested_at = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    for (const auto& v : rows) {
        if (!v.isObject())
            continue;
        QJsonObject row = v.toObject();
        if (!row.contains(QStringLiteral("_lake_ingested_at")))
            row["_lake_ingested_at"] = ingested_at;
        const QByteArray encoded = QJsonDocument(row).toJson(QJsonDocument::Compact) + '\n';
        if (f.write(encoded) != encoded.size()) {
            f.cancelWriting();
            if (error)
                *error = QStringLiteral("failed to write %1: %2").arg(path, f.errorString());
            return false;
        }
    }
    if (!f.commit()) {
        if (error)
            *error = QStringLiteral("failed to commit %1: %2").arg(path, f.errorString());
        return false;
    }
    return true;
}

} // namespace

LocalDataLake& LocalDataLake::instance() {
    static LocalDataLake lake;
    return lake;
}

QString LocalDataLake::root_dir() const {
    return ProfilePaths::profile_root() + QStringLiteral("/data/lake");
}

QString LocalDataLake::dataset_dir(const QString& dataset) const {
    return root_dir() + QDir::separator() + sanitize_name(dataset);
}

QString LocalDataLake::dataset_file(const QString& dataset, const QString& partition) const {
    const QString p = partition.trimmed().isEmpty()
                          ? QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"))
                          : sanitize_name(partition);
    return dataset_dir(dataset) + QDir::separator() + p + QStringLiteral(".jsonl");
}

QString LocalDataLake::manifest_path() const {
    return root_dir() + QStringLiteral("/manifest.json");
}

bool LocalDataLake::ensure(QString* error) const {
    const QString root = root_dir();
    if (ProfilePaths::profile_root().isEmpty()) {
        if (error)
            *error = QStringLiteral("profile root is not available");
        return false;
    }
    if (!QDir().mkpath(root)) {
        if (error)
            *error = QStringLiteral("failed to create %1").arg(root);
        return false;
    }
    const QStringList datasets = {
        QStringLiteral("raw_ticks"),
        QStringLiteral("market_snapshots"),
        QStringLiteral("model_outputs"),
        QStringLiteral("decision_journal"),
        QStringLiteral("broker_events"),
        QStringLiteral("news_research")
    };
    for (const QString& dataset : datasets) {
        if (!QDir().mkpath(dataset_dir(dataset))) {
            if (error)
                *error = QStringLiteral("failed to create %1").arg(dataset_dir(dataset));
            return false;
        }
    }

    if (QFileInfo::exists(manifest_path()))
        return true;

    QLockFile manifest_lock(manifest_path() + QStringLiteral(".lock"));
    manifest_lock.setStaleLockTime(5 * 60 * 1000);
    if (!manifest_lock.tryLock(5000)) {
        if (error)
            *error = QStringLiteral("data lake manifest lock unavailable: %1").arg(manifest_lock.error());
        return false;
    }
    // Another collector may have completed initialization while this process
    // was waiting for the lock.
    if (QFileInfo::exists(manifest_path()))
        return true;

    QSaveFile manifest_file(manifest_path());
    if (!manifest_file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("failed to write manifest: %1").arg(manifest_file.errorString());
        return false;
    }
    manifest_file.write(QJsonDocument(manifest()).toJson(QJsonDocument::Indented));
    if (!manifest_file.commit()) {
        if (error)
            *error = QStringLiteral("failed to commit manifest: %1").arg(manifest_file.errorString());
        return false;
    }
    return true;
}

QJsonObject LocalDataLake::manifest() const {
    QJsonArray datasets;
    datasets.append(QJsonObject{{"name", "raw_ticks"},
                                {"format", "jsonl"},
                                {"grain", "one tick per line"},
                                {"primary_time", "received_ts"},
                                {"duckdb_hint", "read_json_auto('raw_ticks/*.jsonl')"}});
    datasets.append(QJsonObject{{"name", "market_snapshots"},
                                {"format", "jsonl"},
                                {"grain", "one prediction market snapshot per line"},
                                {"primary_time", "observed_at"},
                                {"duckdb_hint", "read_json_auto('market_snapshots/*.jsonl')"}});
    datasets.append(QJsonObject{{"name", "model_outputs"},
                                {"format", "jsonl"},
                                {"grain", "one horizon model output per line"},
                                {"primary_time", "as_of"},
                                {"duckdb_hint", "read_json_auto('model_outputs/*.jsonl')"}});
    datasets.append(QJsonObject{{"name", "decision_journal"},
                                {"format", "jsonl"},
                                {"grain", "one gated decision per line"},
                                {"primary_time", "decision_ts"}});
    datasets.append(QJsonObject{{"name", "broker_events"},
                                {"format", "jsonl"},
                                {"grain", "one order/account event per line"},
                                {"primary_time", "event_ts"}});
    datasets.append(QJsonObject{{"name", "news_research"},
                                {"format", "jsonl"},
                                {"grain", "one article/research item per line"},
                                {"primary_time", "observed_at"}});

    return QJsonObject{{"version", 1},
                       {"profile_root", ProfilePaths::profile_root()},
                       {"root", root_dir()},
                       {"format", "jsonl"},
                       {"columnar_next", "install duckdb or add Arrow/Parquet to convert these files without changing collectors"},
                       {"sources", QJsonArray{source_doc(QStringLiteral("sqlite"), QStringLiteral("existing local app store")),
                                              source_doc(QStringLiteral("daemon"), QStringLiteral("managed collectors append here"))}},
                       {"datasets", datasets},
                       {"updated_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
}

QJsonObject LocalDataLake::status() const {
    const QString root = root_dir();
    QJsonArray datasets;
    qint64 active_bytes = 0;
    qint64 orphaned_temp_bytes = 0;
    int orphaned_temp_files = 0;
    for (const QString& name : {QStringLiteral("raw_ticks"),
                               QStringLiteral("market_snapshots"),
                               QStringLiteral("model_outputs"),
                               QStringLiteral("decision_journal"),
                               QStringLiteral("broker_events"),
                               QStringLiteral("news_research")}) {
        const QJsonObject row = dataset_status(root, name);
        active_bytes += row.value("bytes").toString().toLongLong();
        orphaned_temp_bytes += row.value("orphaned_temp_bytes").toString().toLongLong();
        orphaned_temp_files += row.value("orphaned_temp_files").toInt();
        datasets.append(row);
    }
    const QFileInfo manifest_info(manifest_path());
    return QJsonObject{{"root", root},
                       {"exists", QFileInfo::exists(root)},
                       {"manifest", manifest_path()},
                       {"manifest_exists", manifest_info.exists()},
                       {"active_bytes", QString::number(active_bytes)},
                       {"orphaned_temp_files", orphaned_temp_files},
                       {"orphaned_temp_bytes", QString::number(orphaned_temp_bytes)},
                       {"total_bytes", QString::number(active_bytes + orphaned_temp_bytes)},
                       {"storage_warning", orphaned_temp_files > 0
                           ? QStringLiteral("interrupted replacement artifacts detected; use an explicit cleanup command after review")
                           : QString()},
                       {"datasets", datasets}};
}

QJsonObject LocalDataLake::cleanup_interrupted_artifacts(bool confirmed, QString* error) const {
    const InterruptedArtifactScan scan = interrupted_artifact_scan(root_dir());
    qint64 candidate_bytes = 0;
    for (const auto& candidate : scan.candidates)
        candidate_bytes += candidate.bytes;

    QJsonObject out{{"root", root_dir()},
                    {"confirmed", confirmed},
                    {"minimum_artifact_age_seconds", kInterruptedArtifactMinimumAgeMs / 1000},
                    {"candidate_files", scan.candidates.size()},
                    {"candidate_bytes", QString::number(candidate_bytes)},
                    {"recent_files_skipped", scan.recent_files},
                    {"recent_bytes_skipped", QString::number(scan.recent_bytes)},
                    {"preview", cleanup_preview_rows(scan.candidates)}};
    if (!confirmed) {
        out["deleted_files"] = 0;
        out["deleted_bytes"] = QStringLiteral("0");
        out["next_command"] = QStringLiteral("data lake cleanup-interrupted --yes");
        out["rule"] = QStringLiteral("preview only; no files were deleted without --yes");
        return out;
    }

    int deleted_files = 0;
    int locked_files = 0;
    int failed_files = 0;
    qint64 deleted_bytes = 0;
    QJsonArray failures;
    for (const auto& candidate : scan.candidates) {
        const QString target_path = interrupted_artifact_target_path(candidate.path);
        if (target_path.isEmpty()) {
            ++failed_files;
            failures.append(QJsonObject{{"path", candidate.path}, {"error", "invalid artifact name"}});
            continue;
        }
        QLockFile lock(target_path + QStringLiteral(".lock"));
        lock.setStaleLockTime(5 * 60 * 1000);
        if (!lock.tryLock(0)) {
            ++locked_files;
            continue;
        }
        if (!QFile::remove(candidate.path)) {
            ++failed_files;
            failures.append(QJsonObject{{"path", candidate.path},
                                        {"error", QStringLiteral("failed to remove artifact")}});
            continue;
        }
        ++deleted_files;
        deleted_bytes += candidate.bytes;
    }
    out["deleted_files"] = deleted_files;
    out["deleted_bytes"] = QString::number(deleted_bytes);
    out["locked_files_skipped"] = locked_files;
    out["failed_files"] = failed_files;
    if (!failures.isEmpty())
        out["failures"] = failures;
    out["ok"] = failed_files == 0;
    if (failed_files > 0 && error)
        *error = QStringLiteral("some interrupted lake artifacts could not be removed");
    return out;
}

bool LocalDataLake::append_jsonl(const QString& dataset, const QJsonObject& row, QString* error) {
    if (!ensure(error))
        return false;
    const QString path = dataset_file(dataset);
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(5 * 60 * 1000);
    if (!lock.tryLock(5000)) {
        if (error)
            *error = QStringLiteral("data lake write lock unavailable for %1: %2").arg(path, lock.error());
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("failed to open %1: %2").arg(f.fileName(), f.errorString());
        return false;
    }
    QJsonObject copy = row;
    if (!copy.contains(QStringLiteral("_lake_ingested_at")))
        copy["_lake_ingested_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QByteArray encoded = QJsonDocument(copy).toJson(QJsonDocument::Compact) + '\n';
    if (f.write(encoded) != encoded.size()) {
        if (error)
            *error = QStringLiteral("failed to write %1: %2").arg(f.fileName(), f.errorString());
        return false;
    }
    return true;
}

int LocalDataLake::append_jsonl(const QString& dataset, const QJsonArray& rows, QString* error) {
    int written = 0;
    for (const auto& v : rows) {
        if (!v.isObject())
            continue;
        if (!append_jsonl(dataset, v.toObject(), error))
            return -1;
        ++written;
    }
    return written;
}

bool LocalDataLake::replace_jsonl(const QString& dataset,
                                  const QJsonArray& rows,
                                  const QString& partition,
                                  QString* error) {
    if (!ensure(error))
        return false;
    return replace_jsonl_file(dataset_file(dataset, partition), rows, error);
}

bool LocalDataLake::append_market_snapshot(const EdgePredictionMarketSnapshot& snapshot,
                                           const QJsonObject& extra,
                                           QString* error) {
    QJsonObject row = edge_prediction_market_snapshot_to_json(snapshot);
    for (auto it = extra.begin(); it != extra.end(); ++it)
        row[it.key()] = it.value();
    return append_jsonl(QStringLiteral("market_snapshots"), row, error);
}

bool LocalDataLake::append_broker_event(const TradeAuditRow& row, QString* error) {
    return append_jsonl(QStringLiteral("broker_events"), trade_audit_row_to_json(row), error);
}

int LocalDataLake::mirror_edge_prediction_store(const QString& symbol, QString* error) {
    if (!ensure(error))
        return -1;

    const QString sym = symbol.trimmed().isEmpty() ? QStringLiteral("BTC") : symbol.trimmed().toUpper();
    auto ticks = EdgePredictionModelRepository::instance().list_raw_ticks(sym, 5000);
    if (ticks.is_err()) {
        if (error)
            *error = QString::fromStdString(ticks.error());
        return -1;
    }
    QJsonArray tick_rows;
    for (const auto& tick : ticks.value()) {
        tick_rows.append(edge_prediction_raw_tick_to_json(tick));
    }
    if (!replace_jsonl_file(dataset_file(QStringLiteral("raw_ticks")), tick_rows, error))
        return -1;

    auto outputs = EdgePredictionModelRepository::instance().list_model_outputs(sym, 0, 1000);
    if (outputs.is_err()) {
        if (error)
            *error = QString::fromStdString(outputs.error());
        return -1;
    }
    QJsonArray output_rows;
    for (const auto& output : outputs.value()) {
        output_rows.append(edge_prediction_model_output_to_json(output));
    }
    if (!replace_jsonl_file(dataset_file(QStringLiteral("model_outputs")), output_rows, error))
        return -1;
    return tick_rows.size() + output_rows.size();
}

} // namespace openmarketterminal::storage
