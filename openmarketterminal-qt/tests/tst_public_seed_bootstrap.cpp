#include "storage/PublicSeedBootstrap.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

using openmarketterminal::storage::PublicSeedBootstrap;

namespace {
void write_file(const QString& path, const QByteArray& data) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(data), data.size());
}

QJsonObject manifest_for(const QByteArray& database, const QString& table =
                         QStringLiteral("edge_prediction_raw_ticks")) {
    QJsonArray columns;
    for (const QString& column : {QStringLiteral("id"), QStringLiteral("symbol"),
             QStringLiteral("source"), QStringLiteral("price"),
             QStringLiteral("exchange_ts"), QStringLiteral("received_ts")})
        columns.append(column);
    return {{QStringLiteral("format"), QStringLiteral("openterminal-public-seed")},
            {QStringLiteral("policy_version"), 1},
            {QStringLiteral("database_file"), QStringLiteral("public-seed.sqlite")},
            {QStringLiteral("database_sha256"),
             QString::fromLatin1(QCryptographicHash::hash(database, QCryptographicHash::Sha256)
                                     .toHex())},
            {QStringLiteral("database_bytes"), database.size()},
            {QStringLiteral("tables"),
             QJsonObject{{table, QJsonObject{{QStringLiteral("columns"), columns},
                                             {QStringLiteral("rows"), 0}}}}}};
}

void make_seed(const QString& source, const QByteArray& database_contents,
               const QString& table = QStringLiteral("edge_prediction_raw_ticks")) {
    QDir().mkpath(source);
    const QString database_path = QDir(source).filePath(QStringLiteral("public-seed.sqlite"));
    const QString connection_name = QUuid::createUuid().toString();
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        database.setDatabaseName(database_path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        if (table == QStringLiteral("edge_prediction_raw_ticks")) {
            QVERIFY(query.exec(QStringLiteral(
                "CREATE TABLE edge_prediction_raw_ticks(id TEXT, symbol TEXT, source TEXT, "
                "price REAL, exchange_ts INTEGER, received_ts INTEGER)")));
        } else {
            QVERIFY(query.exec(QStringLiteral("CREATE TABLE kalshi_live_orders(id TEXT)")));
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection_name);
    QFile database_file(database_path);
    QVERIFY(database_file.open(QIODevice::ReadOnly));
    const QByteArray database = database_file.readAll();
    Q_UNUSED(database_contents);
    write_file(QDir(source).filePath(QStringLiteral("manifest.json")),
               QJsonDocument(manifest_for(database, table)).toJson(QJsonDocument::Compact));
}
} // namespace

class PublicSeedBootstrapTest final : public QObject {
    Q_OBJECT
private slots:
    void installs_separately_and_is_idempotent() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString source = root.filePath(QStringLiteral("source"));
        const QString data = root.filePath(QStringLiteral("data"));
        make_seed(source, QByteArray());
        QVERIFY(QDir().mkpath(data));
        write_file(QDir(data).filePath(QStringLiteral("openmarketterminal.db")), QByteArray("live"));

        const auto first = PublicSeedBootstrap::install_from_directory(source, data);
        QVERIFY2(first.ok, qUtf8Printable(first.message));
        QVERIFY(first.installed);
        QCOMPARE(QFileInfo(first.database_path).fileName(), QStringLiteral("public-seed.sqlite"));
        QFile live(QDir(data).filePath(QStringLiteral("openmarketterminal.db")));
        QVERIFY(live.open(QIODevice::ReadOnly));
        QCOMPARE(live.readAll(), QByteArray("live"));

        const auto second = PublicSeedBootstrap::install_from_directory(source, data);
        QVERIFY(second.ok);
        QVERIFY(!second.installed);
    }

    void rejects_non_allowlisted_table() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString source = root.filePath(QStringLiteral("source"));
        make_seed(source, QByteArray(), QStringLiteral("kalshi_live_orders"));
        const auto result = PublicSeedBootstrap::install_from_directory(
            source, root.filePath(QStringLiteral("data")));
        QVERIFY(!result.ok);
        QVERIFY(result.message.contains(QStringLiteral("not allowlisted")));
    }

    void rejects_checksum_mismatch_without_installing() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString source = root.filePath(QStringLiteral("source"));
        const QString data = root.filePath(QStringLiteral("data"));
        make_seed(source, QByteArray());
        write_file(QDir(source).filePath(QStringLiteral("public-seed.sqlite")), QByteArray("tampered"));
        const auto result = PublicSeedBootstrap::install_from_directory(source, data);
        QVERIFY(!result.ok);
        QVERIFY(!QFileInfo::exists(QDir(data).filePath(QStringLiteral("public-seed.sqlite"))));
    }
};

QTEST_MAIN(PublicSeedBootstrapTest)
#include "tst_public_seed_bootstrap.moc"
