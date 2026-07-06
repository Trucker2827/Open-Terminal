#include "storage/LocalDataLake.h"

#include "core/profile/ProfilePaths.h"
#include "storage/repositories/EdgePredictionModelRepository.h"
#include "storage/repositories/TradeAuditRepository.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
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
    int files = 0;
    qint64 last_modified = 0;
    if (dir.exists()) {
        const auto entries = dir.entryInfoList(QStringList{QStringLiteral("*.jsonl")},
                                               QDir::Files | QDir::Readable,
                                               QDir::Name);
        for (const QFileInfo& info : entries) {
            ++files;
            bytes += info.size();
            last_modified = std::max<qint64>(last_modified, info.lastModified().toMSecsSinceEpoch());
        }
    }
    return QJsonObject{{"dataset", dataset},
                       {"path", dir.absolutePath()},
                       {"files", files},
                       {"bytes", QString::number(bytes)},
                       {"last_modified_ms", QString::number(last_modified)}};
}

QJsonObject source_doc(const QString& source, const QString& details) {
    return QJsonObject{{"source", source}, {"details", details}};
}

bool replace_jsonl_file(const QString& path, const QJsonArray& rows, QString* error) {
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
        f.write(QJsonDocument(row).toJson(QJsonDocument::Compact));
        f.write("\n");
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
    for (const QString& name : {QStringLiteral("raw_ticks"),
                               QStringLiteral("market_snapshots"),
                               QStringLiteral("model_outputs"),
                               QStringLiteral("decision_journal"),
                               QStringLiteral("broker_events"),
                               QStringLiteral("news_research")}) {
        datasets.append(dataset_status(root, name));
    }
    const QFileInfo manifest_info(manifest_path());
    return QJsonObject{{"root", root},
                       {"exists", QFileInfo::exists(root)},
                       {"manifest", manifest_path()},
                       {"manifest_exists", manifest_info.exists()},
                       {"datasets", datasets}};
}

bool LocalDataLake::append_jsonl(const QString& dataset, const QJsonObject& row, QString* error) {
    if (!ensure(error))
        return false;
    QFile f(dataset_file(dataset));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("failed to open %1: %2").arg(f.fileName(), f.errorString());
        return false;
    }
    QJsonObject copy = row;
    if (!copy.contains(QStringLiteral("_lake_ingested_at")))
        copy["_lake_ingested_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    f.write(QJsonDocument(copy).toJson(QJsonDocument::Compact));
    f.write("\n");
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
