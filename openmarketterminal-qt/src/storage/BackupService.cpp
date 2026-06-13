#include "storage/BackupService.h"

#include "core/config/AppPaths.h"
#include "core/logging/Logger.h"
#include "core/profile/ProfilePaths.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include <miniz.h>

#include <atomic>
#include <cstring>

namespace openmarketterminal {

namespace {
constexpr auto TAG = "Backup";
constexpr const char* kManifestName = "manifest.json";
constexpr const char* kMarkerName = ".restore_pending";
constexpr const char* kStageDir = "_restore_pending";

QString ts_now() {
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
}

// Recursively copy a directory tree (files + subdirs). Missing source = no-op.
bool copy_tree(const QString& src, const QString& dest) {
    QDir sdir(src);
    if (!sdir.exists())
        return true;  // nothing to copy
    if (!QDir().mkpath(dest))
        return false;
    QDirIterator it(src, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString rel = sdir.relativeFilePath(it.filePath());
        const QString target = dest + "/" + rel;
        if (it.fileInfo().isDir()) {
            if (!QDir().mkpath(target))
                return false;
        } else {
            QDir().mkpath(QFileInfo(target).path());
            QFile::remove(target);
            if (!QFile::copy(it.filePath(), target))
                return false;
        }
    }
    return true;
}

// Consistent snapshot of a live SQLite DB via VACUUM INTO — a reader-only op that
// is safe while the app holds the DB open (WAL). Missing source = skip. Falls
// back to a raw copy only if VACUUM fails.
bool snapshot_db(const QString& src, const QString& dest) {
    if (!QFileInfo::exists(src))
        return true;  // db not created yet
    QDir().mkpath(QFileInfo(dest).path());
    QFile::remove(dest);  // VACUUM INTO requires the target not to exist
    static std::atomic<quint64> ctr{0};
    const QString conn = QStringLiteral("omt_backup_%1").arg(ctr.fetch_add(1));
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(src);
        if (db.open()) {
            QSqlQuery q(db);
            QString d = dest;
            d.replace(QLatin1Char('\''), QLatin1String("''"));
            ok = q.exec("VACUUM INTO '" + d + "'");
            if (!ok)
                LOG_WARN(TAG, QString("VACUUM INTO failed for %1: %2 — falling back to file copy")
                                  .arg(src, q.lastError().text()));
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    if (ok)
        return true;
    QFile::remove(dest);
    return QFile::copy(src, dest);  // best-effort fallback
}

// Max applied migration version from the schema_version table, or -1 if unknown.
int read_schema_version(const QString& db_path) {
    if (!QFileInfo::exists(db_path))
        return -1;
    static std::atomic<quint64> ctr{0};
    const QString conn = QStringLiteral("omt_schema_%1").arg(ctr.fetch_add(1));
    int v = -1;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(db_path);
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(QStringLiteral("SELECT MAX(version) FROM schema_version")) && q.next())
                v = q.value(0).toInt();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return v;
}

// Zip every file under src_dir into a single archive at zip_path (entry names are
// paths relative to src_dir). Returns false on any failure.
bool zip_dir(const QString& src_dir, const QString& zip_path) {
    QFile::remove(zip_path);
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, zip_path.toUtf8().constData(), 0))
        return false;
    bool ok = true;
    const QDir base(src_dir);
    QDirIterator it(src_dir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString rel = base.relativeFilePath(it.filePath());
        if (!mz_zip_writer_add_file(&zip, rel.toUtf8().constData(), it.filePath().toUtf8().constData(),
                                    nullptr, 0, static_cast<mz_uint>(6) /* deflate level */)) {
            ok = false;
            break;
        }
    }
    if (ok)
        ok = mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    if (!ok)
        QFile::remove(zip_path);
    return ok;
}

// Extract a backup .zip into dest_dir. Rejects entries with path-traversal
// ("..") or absolute names (zip-slip protection on untrusted input).
bool unzip_to(const QString& zip_path, const QString& dest_dir) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zip_path.toUtf8().constData(), 0))
        return false;
    bool ok = true;
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; ++i) {
        if (mz_zip_reader_is_file_a_directory(&zip, i))
            continue;
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            ok = false;
            break;
        }
        const QString name = QString::fromUtf8(st.m_filename);
        if (name.contains(QLatin1String("..")) || name.startsWith(QLatin1Char('/')) ||
            name.contains(QLatin1Char('\\'))) {
            LOG_WARN(TAG, "Rejected unsafe backup entry name: " + name);
            ok = false;
            break;
        }
        const QString out = dest_dir + "/" + name;
        QDir().mkpath(QFileInfo(out).path());
        if (!mz_zip_reader_extract_to_file(&zip, i, out.toUtf8().constData(), 0)) {
            ok = false;
            break;
        }
    }
    mz_zip_reader_end(&zip);
    return ok;
}
}  // namespace

BackupService& BackupService::instance() {
    static BackupService s;
    return s;
}

Result<QString> BackupService::export_backup(const QString& dest_parent_dir) {
    if (dest_parent_dir.trimmed().isEmpty())
        return Result<QString>::err("No destination folder selected");

    const QString main_db = AppPaths::data() + "/openmarketterminal.db";
    const QString cache_db = AppPaths::data() + "/cache.db";
    const QString ws_db = ProfilePaths::workspace_db();
    const QString stamp = ts_now();

    // 1) Assemble a consistent snapshot in a temp staging dir.
    const QString tmp = QDir(dest_parent_dir).filePath(".omt-backup-tmp-" + stamp);
    QDir(tmp).removeRecursively();
    if (!QDir().mkpath(tmp + "/data"))
        return Result<QString>::err("Could not write to the destination folder (is it writable?)");

    if (!snapshot_db(main_db, tmp + "/data/openmarketterminal.db")) {  // must succeed
        QDir(tmp).removeRecursively();
        return Result<QString>::err("Failed to snapshot the main database");
    }
    snapshot_db(cache_db, tmp + "/data/cache.db");  // best-effort (regenerable cache)
    snapshot_db(ws_db, tmp + "/workspace.db");       // best-effort (workspace state)
    copy_tree(ProfilePaths::layouts_dir(), tmp + "/layouts");
    copy_tree(AppPaths::workspaces(), tmp + "/workspaces");
    copy_tree(AppPaths::files(), tmp + "/files");

    QJsonObject m;
    m["app"] = "OpenMarketTerminal";
    m["format"] = 1;
    m["app_version"] = QCoreApplication::applicationVersion();
    m["created_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    m["schema_version"] = read_schema_version(main_db);
    m["note"] = "Local backup of your OpenMarketTerminal data. Secrets (broker / API keys) live in "
                "your OS keychain and are NOT included — re-enter them after restoring. This data is "
                "UNENCRYPTED (portfolios, watchlists, notes) — store it somewhere safe.";
    QFile mf(tmp + "/" + kManifestName);
    if (mf.open(QIODevice::WriteOnly)) {
        mf.write(QJsonDocument(m).toJson());
        mf.close();
    }

    // 2) Pack the staging dir into a single .zip and discard the staging dir.
    const QString zip_path = QDir(dest_parent_dir).filePath("OpenMarketTerminal-Backup-" + stamp + ".zip");
    const bool zipped = zip_dir(tmp, zip_path);
    QDir(tmp).removeRecursively();
    if (!zipped)
        return Result<QString>::err("Failed to create the backup .zip");

    LOG_INFO(TAG, "Exported backup to " + zip_path);
    return Result<QString>::ok(zip_path);
}

Result<void> BackupService::stage_import(const QString& backup_zip) {
    if (!QFileInfo::exists(backup_zip))
        return Result<void>::err("Backup file not found");

    const QString root = ProfilePaths::profile_root();
    const QString stage = root + "/" + kStageDir;
    QDir(stage).removeRecursively();

    // 1) Extract the .zip into the staging dir (path-traversal safe).
    if (!QDir().mkpath(stage))
        return Result<void>::err("Could not create the staging folder");
    if (!unzip_to(backup_zip, stage)) {
        QDir(stage).removeRecursively();
        return Result<void>::err("Could not read the backup .zip (is the file complete and valid?)");
    }

    // 2) Validate the manifest.
    QFile mf(QDir(stage).filePath(kManifestName));
    if (!mf.open(QIODevice::ReadOnly)) {
        QDir(stage).removeRecursively();
        return Result<void>::err("That file is not an OpenMarketTerminal backup (no manifest.json)");
    }
    const QJsonObject m = QJsonDocument::fromJson(mf.readAll()).object();
    mf.close();
    if (m.value("app").toString() != QLatin1String("OpenMarketTerminal")) {
        QDir(stage).removeRecursively();
        return Result<void>::err("That file is not an OpenMarketTerminal backup");
    }

    // 3) Schema gate: never restore a backup made by a NEWER build into an older one.
    const int backup_schema = m.value("schema_version").toInt(-1);
    const int cur_schema = read_schema_version(AppPaths::data() + "/openmarketterminal.db");
    if (backup_schema > 0 && cur_schema > 0 && backup_schema > cur_schema) {
        QDir(stage).removeRecursively();
        return Result<void>::err(
            "This backup was made by a newer version of OpenMarketTerminal (database schema " +
            std::to_string(backup_schema) + " > " + std::to_string(cur_schema) +
            "). Update the app first, then restore.");
    }

    // 4) Write the marker so the restore is applied on the next launch.
    QFile marker(root + "/" + kMarkerName);
    if (!marker.open(QIODevice::WriteOnly)) {
        QDir(stage).removeRecursively();
        return Result<void>::err("Could not write the restore marker");
    }
    marker.write(QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8());
    marker.close();
    LOG_INFO(TAG, "Staged restore from " + backup_zip + " — will apply on next launch");
    return Result<void>::ok();
}

Result<void> BackupService::apply_pending_restore() {
    const QString root = ProfilePaths::profile_root();
    const QString marker = root + "/" + kMarkerName;
    const QString stage = root + "/" + kStageDir;
    if (!QFileInfo::exists(marker))
        return Result<void>::ok();  // nothing pending
    if (!QFileInfo::exists(stage)) {
        QFile::remove(marker);
        return Result<void>::ok();
    }

    LOG_INFO(TAG, "Applying staged data restore before opening databases");

    // Wipe stale WAL/SHM so they aren't replayed over the restored DB files.
    for (const char* w : {"data/openmarketterminal.db-wal", "data/openmarketterminal.db-shm",
                          "data/cache.db-wal", "data/cache.db-shm", "workspace.db-wal", "workspace.db-shm"})
        QFile::remove(root + "/" + QString::fromLatin1(w));

    // Move current data aside (reversible) into pre-restore-<ts>/.
    const QString undo = root + "/pre-restore-" + ts_now();
    QDir().mkpath(undo);
    const QStringList items = {QStringLiteral("data/openmarketterminal.db"), QStringLiteral("data/cache.db"),
                               QStringLiteral("workspace.db"),               QStringLiteral("layouts"),
                               QStringLiteral("workspaces"),                 QStringLiteral("files")};
    for (const QString& rel : items) {
        const QString live = root + "/" + rel;
        if (QFileInfo::exists(live)) {
            QDir().mkpath(QFileInfo(undo + "/" + rel).path());
            QDir().rename(live, undo + "/" + rel);  // files and dirs
        }
    }

    // Swap the staged backup into place (manifest.json lands in root, harmless).
    if (!copy_tree(stage, root)) {
        return Result<void>::err("Restore failed while copying files — your previous data is in " +
                                 undo.toStdString());
    }

    QDir(stage).removeRecursively();
    QFile::remove(marker);
    QFile::remove(root + "/" + kManifestName);  // tidy the copied manifest
    LOG_INFO(TAG, "Restore complete. Previous data preserved in " + undo);
    return Result<void>::ok();
}

}  // namespace openmarketterminal
