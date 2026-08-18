#include "storage/PublicSeedBootstrap.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

namespace openmarketterminal::storage {
namespace {

constexpr auto kDatabaseName = "public-seed.sqlite";
constexpr auto kManifestName = "manifest.json";

const QMap<QString, QStringList>& allowlist() {
    static const QMap<QString, QStringList> value{
        {QStringLiteral("edge_prediction_raw_ticks"),
         {QStringLiteral("id"), QStringLiteral("symbol"), QStringLiteral("source"),
          QStringLiteral("price"), QStringLiteral("exchange_ts"), QStringLiteral("received_ts")}},
        {QStringLiteral("edge_prediction_market_snapshots"),
         {QStringLiteral("id"), QStringLiteral("venue"), QStringLiteral("symbol"),
          QStringLiteral("horizon"), QStringLiteral("market_id"), QStringLiteral("question"),
          QStringLiteral("yes_price"), QStringLiteral("no_price"), QStringLiteral("spread_cost"),
          QStringLiteral("liquidity_score"), QStringLiteral("seconds_left"),
          QStringLiteral("observed_at")}},
        {QStringLiteral("market_data"),
         {QStringLiteral("symbol"), QStringLiteral("exchange"), QStringLiteral("interval"),
          QStringLiteral("timestamp_ms"), QStringLiteral("open"), QStringLiteral("high"),
          QStringLiteral("low"), QStringLiteral("close"), QStringLiteral("volume"),
          QStringLiteral("oi")}},
    };
    return value;
}

QByteArray file_sha256(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    return hash.result().toHex();
}

bool copy_atomic(const QString& source, const QString& destination) {
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly))
        return false;
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly))
        return false;
    while (!input.atEnd()) {
        const QByteArray chunk = input.read(1024 * 1024);
        if (chunk.isEmpty() && input.error() != QFileDevice::NoError)
            return false;
        if (output.write(chunk) != chunk.size())
            return false;
    }
    return output.commit();
}

bool validate_manifest(const QJsonObject& manifest, QString* error) {
    if (manifest.value(QStringLiteral("format")).toString() !=
        QStringLiteral("openterminal-public-seed")) {
        *error = QStringLiteral("unrecognized public seed format");
        return false;
    }
    if (manifest.value(QStringLiteral("policy_version")).toInt() != 1) {
        *error = QStringLiteral("unsupported public seed policy version");
        return false;
    }
    if (manifest.value(QStringLiteral("database_file")).toString() !=
        QString::fromLatin1(kDatabaseName)) {
        *error = QStringLiteral("public seed database filename mismatch");
        return false;
    }
    const QJsonObject tables = manifest.value(QStringLiteral("tables")).toObject();
    if (tables.isEmpty()) {
        *error = QStringLiteral("public seed contains no allowlisted tables");
        return false;
    }
    for (auto it = tables.begin(); it != tables.end(); ++it) {
        if (!allowlist().contains(it.key())) {
            *error = QStringLiteral("public seed table is not allowlisted: %1").arg(it.key());
            return false;
        }
        const QJsonArray columns = it.value().toObject().value(QStringLiteral("columns")).toArray();
        QStringList actual;
        for (const auto& column : columns)
            actual.append(column.toString());
        if (actual != allowlist().value(it.key())) {
            *error = QStringLiteral("public seed columns are not allowlisted for %1").arg(it.key());
            return false;
        }
    }
    const QString checksum = manifest.value(QStringLiteral("database_sha256")).toString();
    if (checksum.size() != 64) {
        *error = QStringLiteral("public seed checksum is missing or invalid");
        return false;
    }
    return true;
}

bool validate_database(const QString& path, const QJsonObject& manifest, QString* error) {
    const QString connection_name = QStringLiteral("public-seed-%1")
                                        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool valid = true;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        database.setDatabaseName(path);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!database.open()) {
            *error = QStringLiteral("public seed is not a readable SQLite database");
            valid = false;
        } else {
            const QJsonObject manifest_tables = manifest.value(QStringLiteral("tables")).toObject();
            const QStringList expected_list = manifest_tables.keys();
            const QSet<QString> expected(expected_list.begin(), expected_list.end());
            const QStringList table_list = database.tables(QSql::Tables);
            const QSet<QString> actual(table_list.begin(), table_list.end());
            if (actual != expected) {
                *error = QStringLiteral("public seed database contains unexpected tables");
                valid = false;
            }
            for (auto it = manifest_tables.begin(); valid && it != manifest_tables.end(); ++it) {
                const QSqlRecord record = database.record(it.key());
                QStringList columns;
                for (int index = 0; index < record.count(); ++index)
                    columns.append(record.fieldName(index));
                if (columns != allowlist().value(it.key())) {
                    *error = QStringLiteral("public seed database columns are not allowlisted for %1")
                                 .arg(it.key());
                    valid = false;
                    break;
                }
                QString escaped_table = it.key();
                escaped_table.replace(QChar('"'), QStringLiteral("\"\""));
                QSqlQuery count(database);
                if (!count.exec(QStringLiteral("SELECT COUNT(*) FROM \"%1\"").arg(escaped_table)) ||
                    !count.next() || count.value(0).toLongLong() !=
                        it.value().toObject().value(QStringLiteral("rows")).toInteger()) {
                    *error = QStringLiteral("public seed row count differs for %1").arg(it.key());
                    valid = false;
                }
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connection_name);
    return valid;
}

} // namespace

PublicSeedInstallResult PublicSeedBootstrap::install_from_directory(
    const QString& source_directory, const QString& data_directory) {
    const QDir source(source_directory);
    const QString source_db = source.filePath(QString::fromLatin1(kDatabaseName));
    const QString source_manifest = source.filePath(QString::fromLatin1(kManifestName));
    if (!QFileInfo::exists(source_db) && !QFileInfo::exists(source_manifest))
        return {true, false, QStringLiteral("no bundled public seed"), {}};
    if (!QFileInfo(source_db).isFile() || !QFileInfo(source_manifest).isFile())
        return {false, false, QStringLiteral("public seed bundle is incomplete"), {}};

    QFile manifest_file(source_manifest);
    if (!manifest_file.open(QIODevice::ReadOnly))
        return {false, false, QStringLiteral("cannot read public seed manifest"), {}};
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(manifest_file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
        return {false, false, QStringLiteral("invalid public seed manifest"), {}};
    const QJsonObject manifest = document.object();
    QString validation_error;
    if (!validate_manifest(manifest, &validation_error))
        return {false, false, validation_error, {}};
    const QByteArray expected = manifest.value(QStringLiteral("database_sha256"))
                                    .toString().toLatin1().toLower();
    if (file_sha256(source_db) != expected)
        return {false, false, QStringLiteral("public seed checksum mismatch"), {}};
    if (!validate_database(source_db, manifest, &validation_error))
        return {false, false, validation_error, {}};

    if (!QDir().mkpath(data_directory))
        return {false, false, QStringLiteral("cannot create public seed data directory"), {}};
    const QString target_db = QDir(data_directory).filePath(QString::fromLatin1(kDatabaseName));
    const QString target_manifest =
        QDir(data_directory).filePath(QStringLiteral("public-seed-manifest.json"));
    if (QFileInfo::exists(target_db)) {
        if (file_sha256(target_db) == expected)
            return {true, false, QStringLiteral("public seed already installed"), target_db};
        return {false, false, QStringLiteral("refusing to overwrite existing public seed"),
                target_db};
    }
    if (!copy_atomic(source_db, target_db))
        return {false, false, QStringLiteral("cannot install public seed database"), {}};
    if (file_sha256(target_db) != expected) {
        QFile::remove(target_db);
        return {false, false, QStringLiteral("installed public seed checksum mismatch"), {}};
    }
    if (!copy_atomic(source_manifest, target_manifest)) {
        QFile::remove(target_db);
        return {false, false, QStringLiteral("cannot install public seed manifest"), {}};
    }
    QFile::setPermissions(target_db, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                     QFileDevice::ReadGroup | QFileDevice::ReadOther);
    return {true, true, QStringLiteral("public seed installed"), target_db};
}

PublicSeedInstallResult PublicSeedBootstrap::install_bundled_seed(
    const QString& data_directory) {
    const QString executable = QCoreApplication::applicationDirPath();
    const QStringList candidates{
        QDir(executable).filePath(QStringLiteral("resources/public_seed")),
        QDir(executable).filePath(QStringLiteral("../Resources/resources/public_seed")),
        QDir(executable).filePath(QStringLiteral("../resources/public_seed")),
    };
    for (const QString& candidate : candidates) {
        if (!QDir(candidate).exists())
            continue;
        return install_from_directory(candidate, data_directory);
    }
    return {true, false, QStringLiteral("no bundled public seed"), {}};
}

} // namespace openmarketterminal::storage
