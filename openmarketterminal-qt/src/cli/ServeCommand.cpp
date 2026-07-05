#include "cli/ServeCommand.h"
#include "cli/BridgeDiscoveryFile.h"
#include "core/headless/HeadlessRuntime.h"
#include "mcp/TerminalMcpBridge.h"
#include "mcp/McpProvider.h"
#include "mcp/McpTypes.h"
#include "mcp/tools/SettingsGate.h"
#include "storage/repositories/EdgePredictionModelRepository.h"
#include "storage/sqlite/Database.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QSocketNotifier>
#include <QThread>
#include <QJsonObject>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QTimer>
#include <QUuid>
#include <QVariant>
#include <QXmlStreamWriter>
#include <cstdio>
#include <csignal>
#include <algorithm>
#include <functional>
#include <optional>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/types.h>
#endif

namespace openmarketterminal::cli {
namespace {
#ifndef _WIN32
int g_sigfd[2] = {-1, -1};                       // self-pipe: handler writes, notifier reads
void on_signal(int) { char c = 1; ssize_t n = ::write(g_sigfd[1], &c, 1); (void)n; }
#endif

QString daemon_slug(QString s) {
    s = s.trimmed();
    QString out;
    for (const QChar ch : s) {
        if (ch.isLetterOrNumber() || ch == '-' || ch == '_' || ch == '.')
            out += ch;
        else if (!out.endsWith('-'))
            out += '-';
    }
    while (out.endsWith('-')) out.chop(1);
    return out.isEmpty() ? QStringLiteral("default") : out;
}

QString daemon_label(const QString& profile) {
    return QStringLiteral("org.openterminal.cli.daemon.%1").arg(daemon_slug(profile));
}

QString daemon_plist_path(const QString& profile) {
    return QDir::homePath() + QStringLiteral("/Library/LaunchAgents/%1.plist").arg(daemon_label(profile));
}

QString daemon_logs_dir(const QString& profile) {
    return profile_root_for(profile) + QStringLiteral("/logs");
}

QString daemon_state_dir(const QString& profile) {
    return profile_root_for(profile) + QStringLiteral("/daemon");
}

QString daemon_jobs_path(const QString& profile) {
    return daemon_state_dir(profile) + QStringLiteral("/jobs.json");
}

QString daemon_history_db_path(const QString& profile) {
    return daemon_state_dir(profile) + QStringLiteral("/runs.sqlite");
}

QString daemon_job_log_path(const QString& profile) {
    return daemon_logs_dir(profile) + QStringLiteral("/daemon.jobs.log");
}

QString now_utc() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QDateTime parse_utc(const QString& iso) {
    QDateTime dt = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(iso, Qt::ISODate);
    return dt.isValid() ? dt.toUTC() : dt;
}

QStringList json_array_to_strings(const QJsonArray& arr) {
    QStringList out;
    for (const auto& v : arr)
        out << v.toString();
    return out;
}

QJsonArray strings_to_json_array(const QStringList& list) {
    QJsonArray arr;
    for (const auto& s : list)
        arr.append(s);
    return arr;
}

QString tail_text(const QString& path, int lines) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QStringList all = QString::fromUtf8(f.readAll()).split('\n');
    while (!all.isEmpty() && all.last().isEmpty())
        all.removeLast();
    if (lines > 0 && all.size() > lines)
        all = all.mid(all.size() - lines);
    return all.join('\n');
}

void append_job_log(const QString& profile, const QString& line) {
    QDir().mkpath(daemon_logs_dir(profile));
    QFile f(daemon_job_log_path(profile));
    if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << now_utc() << " " << line << "\n";
    }
}

[[maybe_unused]] QString launch_domain() {
#if defined(Q_OS_MACOS)
    return QStringLiteral("gui/%1").arg(static_cast<unsigned long>(::getuid()));
#else
    return {};
#endif
}

struct ProcessResult {
    int exit_code = -1;
    QString out;
    QString err;
};

ProcessResult run_process(const QString& program, const QStringList& args, int timeout_ms = 10000) {
    QProcess p;
    p.start(program, args);
    if (!p.waitForStarted(3000))
        return {-1, {}, QStringLiteral("could not start %1").arg(program)};
    if (!p.waitForFinished(timeout_ms)) {
        p.kill();
        p.waitForFinished(1000);
        return {-1, QString::fromUtf8(p.readAllStandardOutput()),
                QStringLiteral("%1 timed out").arg(program)};
    }
    return {p.exitCode(), QString::fromUtf8(p.readAllStandardOutput()),
            QString::fromUtf8(p.readAllStandardError())};
}

QJsonObject empty_jobs_doc() {
    return QJsonObject{{"schema", 1}, {"jobs", QJsonArray{}}};
}

int default_job_timeout_sec(const QString& kind, int every_sec = 0) {
    const QString k = kind.trimmed().toLower();
    int base = 300;
    if (k == "health-check")
        base = 30;
    else if (k == "notify")
        base = 60;
    else if (k == "brief" || k == "risk" || k == "thesis" || k == "radar" || k == "ai")
        base = 180;
    else if (k == "notebook" || k == "paper-strategy")
        base = 600;
    if (every_sec > 0)
        base = std::min(base, std::max(30, every_sec - 1));
    return base;
}

int job_timeout_sec(const QJsonObject& job) {
    const int explicit_timeout = job.value("timeout_sec").toInt(0);
    if (explicit_timeout > 0)
        return explicit_timeout;
    return default_job_timeout_sec(job.value("kind").toString(),
                                   job.value("interval_sec").toInt(0));
}

QJsonObject load_jobs_doc(const QString& profile) {
    QFile f(daemon_jobs_path(profile));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return empty_jobs_doc();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return empty_jobs_doc();
    QJsonObject o = doc.object();
    if (!o.value("jobs").isArray())
        o["jobs"] = QJsonArray{};
    o["schema"] = 1;
    return o;
}

bool save_jobs_doc(const QString& profile, const QJsonObject& doc, QString* error = nullptr) {
    QDir().mkpath(daemon_state_dir(profile));
    QSaveFile f(daemon_jobs_path(profile));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("could not write jobs file");
        return false;
    }
    f.write(QJsonDocument(doc).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error) *error = QStringLiteral("could not commit jobs file");
        return false;
    }
    QFile::setPermissions(daemon_jobs_path(profile), QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

int find_job_index(const QJsonArray& jobs, const QString& id_or_name) {
    for (int i = 0; i < jobs.size(); ++i) {
        const QJsonObject j = jobs.at(i).toObject();
        if (j.value("id").toString() == id_or_name ||
            j.value("name").toString().compare(id_or_name, Qt::CaseInsensitive) == 0)
            return i;
    }
    for (int i = 0; i < jobs.size(); ++i) {
        const QJsonObject j = jobs.at(i).toObject();
        if (j.value("id").toString().contains(id_or_name, Qt::CaseInsensitive) ||
            j.value("name").toString().contains(id_or_name, Qt::CaseInsensitive))
            return i;
    }
    return -1;
}

QString compact_tail(const QString& s, int max_chars = 2000) {
    QString out = s.trimmed();
    if (out.size() > max_chars)
        out = out.right(max_chars);
    return out;
}

struct DaemonHistoryDb {
    QString connection_name;
    QSqlDatabase db;
    QString error;

    explicit DaemonHistoryDb(const QString& profile)
        : connection_name(QStringLiteral("daemon-history-%1-%2")
                              .arg(daemon_slug(profile), QUuid::createUuid().toString(QUuid::Id128))) {
        QDir().mkpath(daemon_state_dir(profile));
        db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        db.setDatabaseName(daemon_history_db_path(profile));
        if (!db.open()) {
            error = db.lastError().text();
            return;
        }
        QSqlQuery pragma(db);
        pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
        pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
        QSqlQuery q(db);
        if (!q.exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS daemon_job_runs ("
                "run_id TEXT PRIMARY KEY,"
                "job_id TEXT NOT NULL,"
                "job_name TEXT,"
                "kind TEXT,"
                "trigger TEXT,"
                "command TEXT,"
                "status TEXT,"
                "exit_code INTEGER,"
                "started_at TEXT,"
                "finished_at TEXT,"
                "duration_ms INTEGER,"
                "timeout_sec INTEGER,"
                "pid INTEGER,"
                "daemon_pid INTEGER,"
                "stdout_tail TEXT,"
                "stderr_tail TEXT,"
                "error TEXT"
                ")"))) {
            error = q.lastError().text();
            return;
        }
        q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_daemon_job_runs_started "
                              "ON daemon_job_runs(started_at DESC)"));
        q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_daemon_job_runs_job "
                              "ON daemon_job_runs(job_id, started_at DESC)"));
        q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_daemon_job_runs_status "
                              "ON daemon_job_runs(status, started_at DESC)"));
    }

    ~DaemonHistoryDb() {
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection_name);
    }

    bool ok() const { return db.isOpen() && error.isEmpty(); }
};

void bind_run_identity(QSqlQuery& q, const QJsonObject& job) {
    q.bindValue(QStringLiteral(":job_id"), job.value("id").toString());
    q.bindValue(QStringLiteral(":job_name"), job.value("name").toString());
    q.bindValue(QStringLiteral(":kind"), job.value("kind").toString());
    q.bindValue(QStringLiteral(":command"), json_array_to_strings(job.value("command").toArray()).join(' '));
    q.bindValue(QStringLiteral(":timeout_sec"), job_timeout_sec(job));
    q.bindValue(QStringLiteral(":daemon_pid"),
                static_cast<qlonglong>(QCoreApplication::applicationPid()));
}

QString record_job_run_start(const QString& profile,
                             const QJsonObject& job,
                             const QString& trigger,
                             const QString& started_at = now_utc()) {
    DaemonHistoryDb h(profile);
    if (!h.ok()) {
        append_job_log(profile, QStringLiteral("history-error phase=start error=\"%1\"").arg(h.error));
        return {};
    }
    const QString run_id = QStringLiteral("run_%1").arg(QUuid::createUuid().toString(QUuid::Id128).left(16));
    QSqlQuery q(h.db);
    q.prepare(QStringLiteral(
        "INSERT INTO daemon_job_runs "
        "(run_id, job_id, job_name, kind, trigger, command, status, started_at, timeout_sec, daemon_pid) "
        "VALUES (:run_id, :job_id, :job_name, :kind, :trigger, :command, 'running', :started_at, :timeout_sec, :daemon_pid)"));
    q.bindValue(QStringLiteral(":run_id"), run_id);
    bind_run_identity(q, job);
    q.bindValue(QStringLiteral(":trigger"), trigger);
    q.bindValue(QStringLiteral(":started_at"), started_at);
    if (!q.exec()) {
        append_job_log(profile, QStringLiteral("history-error phase=start id=%1 error=\"%2\"")
                                    .arg(job.value("id").toString(), q.lastError().text()));
        return {};
    }
    return run_id;
}

void record_job_run_finish(const QString& profile,
                           const QString& run_id,
                           const QString& status,
                           int exit_code,
                           const QString& out,
                           const QString& err,
                           const QString& error_text = {}) {
    if (run_id.isEmpty())
        return;
    DaemonHistoryDb h(profile);
    if (!h.ok()) {
        append_job_log(profile, QStringLiteral("history-error phase=finish run=%1 error=\"%2\"").arg(run_id, h.error));
        return;
    }
    const QString finished = now_utc();
    QSqlQuery started_q(h.db);
    started_q.prepare(QStringLiteral("SELECT started_at FROM daemon_job_runs WHERE run_id=:run_id"));
    started_q.bindValue(QStringLiteral(":run_id"), run_id);
    qint64 duration_ms = -1;
    if (started_q.exec() && started_q.next()) {
        const QDateTime started = parse_utc(started_q.value(0).toString());
        const QDateTime finished_dt = parse_utc(finished);
        if (started.isValid() && finished_dt.isValid())
            duration_ms = started.msecsTo(finished_dt);
    }
    QSqlQuery q(h.db);
    q.prepare(QStringLiteral(
        "UPDATE daemon_job_runs SET "
        "status=:status, exit_code=:exit_code, finished_at=:finished_at, duration_ms=:duration_ms, "
        "stdout_tail=:stdout_tail, stderr_tail=:stderr_tail, error=:error "
        "WHERE run_id=:run_id"));
    q.bindValue(QStringLiteral(":status"), status);
    q.bindValue(QStringLiteral(":exit_code"), exit_code);
    q.bindValue(QStringLiteral(":finished_at"), finished);
    q.bindValue(QStringLiteral(":duration_ms"), duration_ms >= 0 ? QVariant(duration_ms) : QVariant());
    q.bindValue(QStringLiteral(":stdout_tail"), compact_tail(out));
    q.bindValue(QStringLiteral(":stderr_tail"), compact_tail(err));
    q.bindValue(QStringLiteral(":error"), error_text);
    q.bindValue(QStringLiteral(":run_id"), run_id);
    if (!q.exec()) {
        append_job_log(profile, QStringLiteral("history-error phase=finish run=%1 error=\"%2\"")
                                    .arg(run_id, q.lastError().text()));
    }
}

QStringList command_for_job_kind(const QString& kind, const QJsonObject& spec) {
    const QString k = kind.trimmed().toLower();
    if (k == "command")
        return json_array_to_strings(spec.value("command").toArray());
    if (k == "brief" || k == "risk" || k == "thesis" || k == "radar")
        return {k, spec.value("target").toString()};
    if (k == "ai") {
        const QString workflow = spec.value("workflow").toString("brief").trimmed().toLower();
        return {workflow, spec.value("target").toString()};
    }
    if (k == "notebook") {
        QStringList args{QStringLiteral("notebook"), QStringLiteral("run"), spec.value("selector").toString()};
        const int cell = spec.value("cell").toInt(0);
        if (cell > 0)
            args << QStringLiteral("--cell") << QString::number(cell);
        return args;
    }
    if (k == "paper-strategy") {
        QStringList args{QStringLiteral("strategy"), QStringLiteral("paper-run"),
                         spec.value("strategy").toString("meanrev")};
        const QString symbols = spec.value("symbols").toString();
        if (!symbols.isEmpty())
            args << QStringLiteral("--symbols") << symbols;
        const int max_iters = spec.value("max_iters").toInt(1);
        args << QStringLiteral("--max-iters") << QString::number(max_iters);
        args << QStringLiteral("--interval-sec") << QString::number(spec.value("interval_sec").toInt(0));
        return args;
    }
    if (k == "notify") {
        QStringList args{QStringLiteral("notify"), QStringLiteral("send")};
        const QString provider = spec.value("provider").toString();
        if (!provider.isEmpty())
            args << QStringLiteral("--provider") << provider;
        else
            args << QStringLiteral("--all");
        args << QStringLiteral("--title") << spec.value("title").toString()
             << QStringLiteral("--message") << spec.value("message").toString()
             << QStringLiteral("--level") << spec.value("level").toString("info")
             << QStringLiteral("--yes");
        return args;
    }
    if (k == "health-check")
        return {QStringLiteral("daemon"), QStringLiteral("health")};
    return {};
}

QJsonObject make_job(const QString& kind,
                     const QString& name,
                     const QJsonObject& spec,
                     int every_sec,
                     int timeout_sec,
                     bool enabled = true) {
    QStringList command = command_for_job_kind(kind, spec);
    const QString id = QStringLiteral("job_%1").arg(QUuid::createUuid().toString(QUuid::Id128).left(12));
    const QString created = now_utc();
    const int effective_timeout = timeout_sec > 0 ? timeout_sec : default_job_timeout_sec(kind, every_sec);
    return QJsonObject{{"id", id},
                       {"name", name.trimmed().isEmpty() ? kind + QStringLiteral(" ") + id.right(4) : name.trimmed()},
                       {"kind", kind},
                       {"enabled", enabled},
                       {"schedule", every_sec > 0 ? QStringLiteral("interval") : QStringLiteral("manual")},
                       {"interval_sec", every_sec},
                       {"timeout_sec", effective_timeout},
                       {"next_run_at", every_sec > 0 ? created : QString()},
                       {"running", false},
                       {"run_count", 0},
                       {"fail_count", 0},
                       {"created_at", created},
                       {"updated_at", created},
                       {"spec", spec},
                       {"command", strings_to_json_array(command)}};
}

ProcessResult run_job_once_sync(const QString& profile, const QJsonObject& job, int timeout_ms = 0) {
    QStringList args{QStringLiteral("--profile"), profile};
    args << json_array_to_strings(job.value("command").toArray());
    if (timeout_ms <= 0)
        timeout_ms = std::max(1, job_timeout_sec(job)) * 1000;
    return run_process(QCoreApplication::applicationFilePath(), args, timeout_ms);
}

bool job_has_current_failure(const QJsonObject& job) {
    if (job.value("running").toBool())
        return false;
    const QString status = job.value("last_status").toString().trimmed().toLower();
    return status == QStringLiteral("failed") ||
           status == QStringLiteral("timeout") ||
           status == QStringLiteral("stale-timeout");
}

QJsonObject jobs_summary(const QString& profile) {
    const QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
    int enabled = 0, running = 0, failed = 0, failed_history = 0, interval = 0, stale = 0;
    QJsonArray by_kind;
    QJsonObject counts;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const auto& v : jobs) {
        const QJsonObject j = v.toObject();
        if (j.value("enabled").toBool()) ++enabled;
        if (j.value("running").toBool()) ++running;
        if (j.value("fail_count").toInt() > 0) ++failed_history;
        if (job_has_current_failure(j)) ++failed;
        if (j.value("schedule").toString() == QStringLiteral("interval")) ++interval;
        if (j.value("running").toBool()) {
            const QDateTime started = parse_utc(j.value("last_started_at").toString());
            if (started.isValid() && started.addSecs(job_timeout_sec(j) + 15) < now)
                ++stale;
        }
        const QString kind = j.value("kind").toString("unknown");
        counts[kind] = counts.value(kind).toInt() + 1;
    }
    for (auto it = counts.begin(); it != counts.end(); ++it)
        by_kind.append(QJsonObject{{"kind", it.key()}, {"count", it.value().toInt()}});
    return QJsonObject{{"total", jobs.size()},
                       {"enabled", enabled},
                       {"running", running},
                       {"failed", failed},
                       {"failed_history", failed_history},
                       {"stale", stale},
                       {"interval", interval},
                       {"by_kind", by_kind}};
}

bool launchd_loaded(const QString& profile) {
#if defined(Q_OS_MACOS)
    const ProcessResult r = run_process(QStringLiteral("launchctl"),
                                        {QStringLiteral("print"), launch_domain() + "/" + daemon_label(profile)},
                                        5000);
    return r.exit_code == 0;
#else
    Q_UNUSED(profile);
    return false;
#endif
}

void write_plist_key_string(QXmlStreamWriter& w, const QString& key, const QString& value) {
    w.writeTextElement(QStringLiteral("key"), key);
    w.writeTextElement(QStringLiteral("string"), value);
}

void write_plist_key_bool(QXmlStreamWriter& w, const QString& key, bool value) {
    w.writeTextElement(QStringLiteral("key"), key);
    w.writeEmptyElement(value ? QStringLiteral("true") : QStringLiteral("false"));
}

[[maybe_unused]] bool write_daemon_plist(const QString& profile, QString* error) {
    const QString path = daemon_plist_path(profile);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QDir().mkpath(daemon_logs_dir(profile));

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("could not write LaunchAgent plist: ") + path;
        return false;
    }

    QXmlStreamWriter w(&f);
    w.setAutoFormatting(true);
    w.writeStartDocument(QStringLiteral("1.0"));
    w.writeDTD(QStringLiteral("<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                              "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"));
    w.writeStartElement(QStringLiteral("plist"));
    w.writeAttribute(QStringLiteral("version"), QStringLiteral("1.0"));
    w.writeStartElement(QStringLiteral("dict"));

    write_plist_key_string(w, QStringLiteral("Label"), daemon_label(profile));
    w.writeTextElement(QStringLiteral("key"), QStringLiteral("ProgramArguments"));
    w.writeStartElement(QStringLiteral("array"));
    w.writeTextElement(QStringLiteral("string"), QCoreApplication::applicationFilePath());
    w.writeTextElement(QStringLiteral("string"), QStringLiteral("--profile"));
    w.writeTextElement(QStringLiteral("string"), profile);
    w.writeTextElement(QStringLiteral("string"), QStringLiteral("serve"));
    w.writeEndElement();

    write_plist_key_bool(w, QStringLiteral("RunAtLoad"), true);
    w.writeTextElement(QStringLiteral("key"), QStringLiteral("KeepAlive"));
    w.writeStartElement(QStringLiteral("dict"));
    write_plist_key_bool(w, QStringLiteral("Crashed"), true);
    w.writeEndElement();
    write_plist_key_string(w, QStringLiteral("WorkingDirectory"), QCoreApplication::applicationDirPath());
    write_plist_key_string(w, QStringLiteral("ProcessType"), QStringLiteral("Background"));
    write_plist_key_string(w, QStringLiteral("StandardOutPath"),
                           daemon_logs_dir(profile) + QStringLiteral("/daemon.out.log"));
    write_plist_key_string(w, QStringLiteral("StandardErrorPath"),
                           daemon_logs_dir(profile) + QStringLiteral("/daemon.err.log"));

    w.writeEndElement(); // dict
    w.writeEndElement(); // plist
    w.writeEndDocument();
    f.close();
    return true;
}

QJsonObject daemon_status_object(const QString& profile) {
    auto info = read_bridge_file(profile_root_for(profile));
    const bool running = info && is_pid_alive(info->pid) && info->kind == QStringLiteral("daemon");
    QJsonObject o{{"profile", profile},
                  {"label", daemon_label(profile)},
                  {"plist", daemon_plist_path(profile)},
                  {"installed", QFileInfo::exists(daemon_plist_path(profile))},
                  {"loaded", launchd_loaded(profile)},
                  {"running", running}};
    if (info && is_pid_alive(info->pid)) {
        o["owner_kind"] = info->kind;
        o["pid"] = info->pid;
        o["endpoint"] = info->endpoint;
    }
    return o;
}

bool wait_for_daemon_running(const QString& profile, int timeout_ms) {
    const int sleep_ms = 250;
    const int tries = std::max(1, timeout_ms / sleep_ms);
    for (int i = 0; i < tries; ++i) {
        if (daemon_status_object(profile).value("running").toBool())
            return true;
        QThread::msleep(sleep_ms);
    }
    return daemon_status_object(profile).value("running").toBool();
}

QJsonObject daemon_health_object(const QString& profile) {
    QJsonObject o = daemon_status_object(profile);
    auto info = read_bridge_file(profile_root_for(profile));
    if (info && is_pid_alive(info->pid)) {
        const QDateTime started = parse_utc(info->started_at);
        if (started.isValid())
            o["uptime_sec"] = started.secsTo(QDateTime::currentDateTimeUtc());
    }
    o["version"] = QCoreApplication::applicationVersion();
    o["jobs"] = jobs_summary(profile);
    o["logs"] = QJsonObject{{"stdout", daemon_logs_dir(profile) + QStringLiteral("/daemon.out.log")},
                            {"stderr", daemon_logs_dir(profile) + QStringLiteral("/daemon.err.log")},
                            {"jobs", daemon_job_log_path(profile)}};
    o["capabilities"] = QJsonArray{"health", "readiness", "logs", "jobs", "monitors", "notify",
                                   "paper-strategy", "ai-automation", "audit"};
    return o;
}

int emit_daemon_health(const QString& profile, bool json) {
    const QJsonObject o = daemon_health_object(profile);
    if (json) {
        std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("profile      %s\n", qUtf8Printable(profile));
    std::printf("running      %s\n", o.value("running").toBool() ? "yes" : "no");
    std::printf("installed    %s\n", o.value("installed").toBool() ? "yes" : "no");
    if (o.contains("pid"))
        std::printf("pid          %lld\n", static_cast<long long>(o.value("pid").toDouble()));
    if (o.contains("uptime_sec"))
        std::printf("uptime       %llds\n", static_cast<long long>(o.value("uptime_sec").toDouble()));
    const QJsonObject j = o.value("jobs").toObject();
    std::printf("jobs         total=%d enabled=%d running=%d failed=%d stale=%d history=%d interval=%d\n",
                j.value("total").toInt(), j.value("enabled").toInt(), j.value("running").toInt(),
                j.value("failed").toInt(), j.value("stale").toInt(),
                j.value("failed_history").toInt(), j.value("interval").toInt());
    std::printf("endpoint     %s\n", qUtf8Printable(o.value("endpoint").toString("(none)")));
    return 0;
}

int emit_daemon_logs(const QString& profile, bool json, QStringList args) {
    QString channel = QStringLiteral("stderr");
    int lines = 80;
    for (int i = 0; i < args.size(); ++i) {
        const QString token = args.at(i);
        if (token == "--lines" && i + 1 < args.size()) {
            bool ok = false;
            lines = args.at(++i).toInt(&ok);
            if (!ok || lines <= 0) {
                std::fprintf(stderr, "--lines requires a positive integer\n");
                return 2;
            }
        } else if (token == "stderr" || token == "err" || token == "stdout" || token == "out" ||
                   token == "jobs" || token == "job") {
            channel = (token == "out") ? QStringLiteral("stdout")
                      : (token == "err") ? QStringLiteral("stderr")
                      : (token == "job") ? QStringLiteral("jobs")
                                         : token;
        } else {
            std::fprintf(stderr, "usage: daemon logs [stderr|stdout|jobs] [--lines N]\n");
            return 2;
        }
    }
    const QString path = channel == "stdout" ? daemon_logs_dir(profile) + QStringLiteral("/daemon.out.log")
                       : channel == "jobs" ? daemon_job_log_path(profile)
                                           : daemon_logs_dir(profile) + QStringLiteral("/daemon.err.log");
    const QString text = tail_text(path, lines);
    if (json) {
        std::printf("%s\n", QJsonDocument(QJsonObject{{"profile", profile},
                                                      {"channel", channel},
                                                      {"path", path},
                                                      {"lines", lines},
                                                      {"text", text}})
                                .toJson(QJsonDocument::Compact)
                                .constData());
    } else {
        if (text.isEmpty())
            std::printf("no log data: %s\n", qUtf8Printable(path));
        else
            std::printf("%s\n", qUtf8Printable(text));
    }
    return 0;
}

int emit_daemon_audit(const QString& profile, bool json) {
    QJsonObject o{{"profile", profile},
                  {"plist", daemon_plist_path(profile)},
                  {"jobs_file", daemon_jobs_path(profile)},
                  {"history_db", daemon_history_db_path(profile)},
                  {"logs_dir", daemon_logs_dir(profile)},
                  {"api_keys_leave_machine", false},
                  {"unattended_live_trading", false},
                  {"daemon_bridge_destructive_tools", false},
                  {"daemon_settings_write_tools", false}};
    o["allowed_job_kinds"] = QJsonArray{"command", "brief", "risk", "thesis", "radar", "ai", "notebook",
                                        "paper-strategy", "notify", "health-check"};
    o["guardrails"] = QJsonArray{"LaunchAgent is per-user, not root",
                                 "jobs run as the selected local profile",
                                 "live deployment remains GUI-gated",
                                 "secrets are not written to daemon job specs"};
    o["status"] = daemon_status_object(profile);
    if (json) {
        std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
    } else {
        std::printf("daemon audit  profile=%s\n", qUtf8Printable(profile));
        std::printf("live trading unattended: no\n");
        std::printf("destructive bridge tools: no\n");
        std::printf("settings writes from daemon bridge: no\n");
        std::printf("job store: %s\n", qUtf8Printable(daemon_jobs_path(profile)));
        std::printf("run history: %s\n", qUtf8Printable(daemon_history_db_path(profile)));
        std::printf("plist: %s\n", qUtf8Printable(daemon_plist_path(profile)));
    }
    return 0;
}

int emit_daemon_status(const QString& profile, bool json) {
    const QJsonObject o = daemon_status_object(profile);
    if (json) {
        std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
        return o.value("installed").toBool() || o.value("running").toBool() ? 0 : 3;
    }
    std::printf("profile    %s\n", qUtf8Printable(profile));
    std::printf("label      %s\n", qUtf8Printable(o.value("label").toString()));
    std::printf("plist      %s\n", qUtf8Printable(o.value("plist").toString()));
    std::printf("installed  %s\n", o.value("installed").toBool() ? "yes" : "no");
    std::printf("loaded     %s\n", o.value("loaded").toBool() ? "yes" : "no");
    std::printf("running    %s\n", o.value("running").toBool() ? "yes" : "no");
    if (o.contains("owner_kind")) {
        std::printf("owner      %s pid=%lld endpoint=%s\n",
                    qUtf8Printable(o.value("owner_kind").toString()),
                    static_cast<long long>(o.value("pid").toDouble()),
                    qUtf8Printable(o.value("endpoint").toString()));
    }
    return o.value("installed").toBool() || o.value("running").toBool() ? 0 : 3;
}

int daemon_start_impl(const QString& profile, bool json) {
#if !defined(Q_OS_MACOS)
    Q_UNUSED(profile); Q_UNUSED(json);
    std::fprintf(stderr, "daemon install/start is currently supported on macOS launchd only\n");
    return 2;
#else
    auto emit_started = [&](bool already_running, const QString& warning = {}) {
        if (json) {
            QJsonObject o = daemon_status_object(profile);
            o["started"] = !already_running;
            o["already_running"] = already_running;
            if (!warning.isEmpty())
                o["warning"] = warning;
            std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
        } else {
            if (!warning.isEmpty())
                std::fprintf(stderr, "warning: %s\n", qUtf8Printable(warning));
            std::printf("%s for profile '%s'\n",
                        already_running ? "daemon already running" : "daemon started",
                        qUtf8Printable(profile));
        }
        return 0;
    };
    const QString plist = daemon_plist_path(profile);
    if (!QFileInfo::exists(plist)) {
        std::fprintf(stderr, "daemon is not installed for profile '%s'; run daemon install first\n",
                     qUtf8Printable(profile));
        return 3;
    }
    if (!launchd_loaded(profile)) {
        const ProcessResult boot = run_process(QStringLiteral("launchctl"),
                                               {QStringLiteral("bootstrap"), launch_domain(), plist},
                                               10000);
        if (boot.exit_code != 0 && !boot.err.contains(QStringLiteral("already"), Qt::CaseInsensitive)) {
            std::fprintf(stderr, "launchctl bootstrap failed: %s\n", qUtf8Printable(boot.err.trimmed()));
            return 7;
        }
        if (wait_for_daemon_running(profile, 5000))
            return emit_started(false);
    }
    if (daemon_status_object(profile).value("running").toBool())
        return emit_started(true);
    const ProcessResult kick = run_process(QStringLiteral("launchctl"),
                                           {QStringLiteral("kickstart"), QStringLiteral("-k"),
                                            launch_domain() + "/" + daemon_label(profile)},
                                           10000);
    if (kick.exit_code != 0) {
        if (wait_for_daemon_running(profile, 15000)) {
            const QString detail = kick.err.trimmed().isEmpty()
                                       ? QStringLiteral("no launchctl error text")
                                       : kick.err.trimmed();
            const QString warning = QStringLiteral("launchctl kickstart reported an error after the daemon became reachable: %1")
                                        .arg(detail);
            return emit_started(false, warning);
        }
        std::fprintf(stderr, "launchctl kickstart failed: %s\n", qUtf8Printable(kick.err.trimmed()));
        return 7;
    }
    if (!wait_for_daemon_running(profile, 15000)) {
        std::fprintf(stderr, "daemon start returned but the local bridge did not become reachable\n");
        return 7;
    }
    return emit_started(false);
#endif
}

int daemon_stop_impl(const QString& profile, bool json, bool quiet = false) {
#if !defined(Q_OS_MACOS)
    Q_UNUSED(json);
    Q_UNUSED(quiet);
    return serve_stop(profile);
#else
    bool stopped = false;
    if (launchd_loaded(profile)) {
        const ProcessResult r = run_process(QStringLiteral("launchctl"),
                                            {QStringLiteral("bootout"), launch_domain() + "/" + daemon_label(profile)},
                                            10000);
        if (r.exit_code != 0 && !r.err.contains(QStringLiteral("No such process"), Qt::CaseInsensitive)) {
            std::fprintf(stderr, "launchctl bootout failed: %s\n", qUtf8Printable(r.err.trimmed()));
            return 7;
        }
        stopped = true;
    } else {
        auto info = read_bridge_file(profile_root_for(profile));
        if (info && is_pid_alive(info->pid) && info->kind == QStringLiteral("daemon")) {
            const int rc = serve_stop(profile);
            if (rc != 0) return rc;
            stopped = true;
        }
    }
    if (quiet) {
        return 0;
    }
    if (json) {
        QJsonObject o = daemon_status_object(profile);
        o["stopped"] = stopped;
        std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
    } else if (stopped) {
        std::printf("daemon stopped for profile '%s'\n", qUtf8Printable(profile));
    } else {
        std::fprintf(stderr, "daemon is not loaded/running for profile '%s'\n", qUtf8Printable(profile));
        return 3;
    }
    return 0;
#endif
}

} // namespace

void start_daemon_job_scheduler(const QString& profile);

int serve_run(const QString& profile) {
    const QString root = profile_root_for(profile);
    // Single-owner: refuse if a live instance (GUI or daemon) already owns it.
    if (auto info = read_bridge_file(root); info && is_pid_alive(info->pid)) {
        std::fprintf(stderr, "An instance already owns profile '%s' (%s, pid %lld) at %s\n",
                     qUtf8Printable(profile), qUtf8Printable(info->kind),
                     static_cast<long long>(info->pid), qUtf8Printable(info->endpoint));
        return 3;
    }

    headless::HeadlessRuntime rt;
    if (auto r = rt.init(profile); !r.ok) {
        std::fprintf(stderr, "daemon init failed: %s\n", qUtf8Printable(r.error));
        return 7;
    }

    // Read-only over the bridge: deny destructive AND settings-write regardless
    // of toggles (writes/destructive over a long-lived daemon need the revocable
    // -token design — deferred). Reads + the >=Verified floor unchanged.
    // NOTE: this gate covers only McpProvider (internal) tool calls. If a future
    // task initializes McpService (external MCP servers) in the daemon, those
    // calls would bypass this checker and need their own read-only gate.
    mcp::McpProvider::instance().set_auth_checker(
        [](const QString& tool, const QJsonObject& args, mcp::AuthLevel required, bool is_destructive) {
            if (required >= mcp::AuthLevel::Verified) return false;
            if (tool == "submit_order") {
                // Normalize identically to the handler so a case/whitespace variant
                // can't take a different branch than the handler will.
                const QString mode = args.value("mode").toString().trimmed().toLower();
                if (mode == "paper") return true;                // reach the handler; it enforces the toggle + executes
                return mcp::cli_trading_allowed() && mcp::cli_live_armed();  // live: reach the handler only when armed (handler enforces the full stack)
            }
            // Fast-live carve-out (Phase D) — IDENTICAL predicate in all three
            // hosts. The fast-live tool set is reachable ONLY when fully armed:
            // base trading + base live arm + the SECOND fast arm. Raw live_* are
            // NOT in this set, so they fall through to the destructive denial
            // below and stay denied. (When the fast tools are built they must NOT
            // be classified live-execution, or the AI-facing hosts that deny those
            // would block them before this gate fires.)
            if (mcp::is_fast_live_tool(tool))
                return mcp::cli_trading_allowed() && mcp::cli_live_armed() && mcp::cli_fast_live_armed();
            if (is_destructive) return false;            // daemon MVP: no writes/destructive
            if (mcp::is_settings_write_tool(tool)) return false;
            return true;
        });

#ifndef _WIN32
    // Clean shutdown on SIGTERM/SIGINT via the self-pipe trick (async-signal-safe).
    // Install the handlers BEFORE bridge.start() so a signal arriving during/after
    // start() routes through the notifier (clean stop) rather than killing the
    // process with a stale bridge.json on disk. The pipe + std::signal install do
    // not depend on the bridge; the QSocketNotifier needs qApp, which exists.
    // O_CLOEXEC keeps the self-pipe fds out of any child processes the daemon spawns.
    bool sig_ready = false;
#  if defined(__linux__)
    sig_ready = (::pipe2(g_sigfd, O_CLOEXEC) == 0);       // Linux: atomic CLOEXEC
#  endif
    if (!sig_ready && ::pipe(g_sigfd) == 0) {             // macOS fallback: pipe + fcntl
        ::fcntl(g_sigfd[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(g_sigfd[1], F_SETFD, FD_CLOEXEC);
        sig_ready = true;
    }
    if (sig_ready) {
        auto* sn = new QSocketNotifier(g_sigfd[0], QSocketNotifier::Read, qApp);
        QObject::connect(sn, &QSocketNotifier::activated, qApp, []() { QCoreApplication::quit(); });
        std::signal(SIGTERM, on_signal);
        std::signal(SIGINT, on_signal);
    }
#endif

    auto& bridge = mcp::TerminalMcpBridge::instance();
    bridge.set_owner_kind("daemon");
    if (!bridge.start()) {                                 // binds 127.0.0.1 + writes bridge.json(kind=daemon)
        std::fprintf(stderr, "daemon: failed to start the bridge\n");
        return 7;
    }
    std::fprintf(stderr, "openterminalcli serve: %s (profile '%s', pid %lld). Ctrl-C / SIGTERM to stop.\n",
                 qUtf8Printable(bridge.endpoint()), qUtf8Printable(profile),
                 static_cast<long long>(QCoreApplication::applicationPid()));
    start_daemon_job_scheduler(profile);

    const int rc = QCoreApplication::exec();              // feeds/subscriptions live here
    bridge.stop();                                        // removes bridge.json
    return rc;
}

int serve_status(const QString& profile, bool json) {
    auto info = read_bridge_file(profile_root_for(profile));
    const bool live = info && is_pid_alive(info->pid);
    if (json) {
        QJsonObject o{{"running", live}};
        if (live) { o["endpoint"]=info->endpoint; o["pid"]=info->pid; o["kind"]=info->kind; }
        std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
    } else if (live) {
        std::printf("running  kind=%s  endpoint=%s  pid=%lld\n",
                    qUtf8Printable(info->kind), qUtf8Printable(info->endpoint),
                    static_cast<long long>(info->pid));
    } else {
        std::fprintf(stderr, "no running instance for profile '%s'\n", qUtf8Printable(profile));
    }
    return live ? 0 : 3;
}

int serve_stop(const QString& profile) {
    auto info = read_bridge_file(profile_root_for(profile));
    if (!info || !is_pid_alive(info->pid)) {
        std::fprintf(stderr, "no running instance for profile '%s'\n", qUtf8Printable(profile));
        return 3;
    }
    if (info->kind != "daemon") {
        std::fprintf(stderr, "owner is a %s, not a daemon — refusing to stop it (quit it directly)\n",
                     qUtf8Printable(info->kind));
        return 3;
    }
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(info->pid));
    if (!process) {
        std::fprintf(stderr, "failed to open daemon pid %lld for termination\n",
                     static_cast<long long>(info->pid));
        return 7;
    }
    if (!TerminateProcess(process, 0)) {
        CloseHandle(process);
        std::fprintf(stderr, "failed to terminate daemon pid %lld\n", static_cast<long long>(info->pid));
        return 7;
    }
    WaitForSingleObject(process, 5000);
    CloseHandle(process);
    remove_bridge_file(profile_root_for(profile));
    std::printf("daemon pid %lld stopped\n", static_cast<long long>(info->pid));
    return 0;
#else
    if (::kill(static_cast<pid_t>(info->pid), SIGTERM) != 0) {
        std::fprintf(stderr, "failed to signal daemon pid %lld\n", static_cast<long long>(info->pid));
        return 7;
    }
    // Grace: wait up to ~5s for clean exit; escalate to SIGKILL if still alive.
    for (int i = 0; i < 50 && is_pid_alive(info->pid); ++i)
        QThread::msleep(100);
    if (is_pid_alive(info->pid)) {
        std::fprintf(stderr, "daemon pid %lld did not exit on SIGTERM; sending SIGKILL\n",
                     static_cast<long long>(info->pid));
        ::kill(static_cast<pid_t>(info->pid), SIGKILL);
        for (int i = 0; i < 20 && is_pid_alive(info->pid); ++i) QThread::msleep(100);
        // A SIGKILLed daemon can't run its aboutToQuit cleanup, so the stale
        // bridge.json may remain; remove it so the next attach/serve is clean.
        remove_bridge_file(profile_root_for(profile));
        std::printf("daemon pid %lld force-stopped (SIGKILL)\n", static_cast<long long>(info->pid));
        return 0;
    }
    std::printf("sent SIGTERM to daemon pid %lld\n", static_cast<long long>(info->pid));
    return 0;
#endif
}

int emit_jobs_list(const QString& profile, bool json) {
    const QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
    if (json) {
        std::printf("%s\n", QJsonDocument(QJsonObject{{"profile", profile}, {"jobs", jobs}})
                                .toJson(QJsonDocument::Compact)
                                .constData());
        return 0;
    }
    if (jobs.isEmpty()) {
        std::printf("no daemon jobs\n");
        return 0;
    }
    std::printf("%-16s %-22s %-15s %-8s %-12s %-8s %-8s %s\n",
                "id", "name", "kind", "enabled", "last", "fails", "timeout", "command");
    for (const auto& v : jobs) {
        const QJsonObject j = v.toObject();
        const QString timeout_label = QStringLiteral("%1s").arg(job_timeout_sec(j));
        std::printf("%-16s %-22s %-15s %-8s %-12s %-8d %-8s %s\n",
                    qUtf8Printable(j.value("id").toString()),
                    qUtf8Printable(j.value("name").toString().left(22)),
                    qUtf8Printable(j.value("kind").toString()),
                    j.value("enabled").toBool() ? "yes" : "no",
                    qUtf8Printable(j.value("last_status").toString("-")),
                    j.value("fail_count").toInt(),
                    qUtf8Printable(timeout_label),
                    qUtf8Printable(json_array_to_strings(j.value("command").toArray()).join(' ')));
    }
    return 0;
}

QString duration_label(qint64 duration_ms) {
    if (duration_ms < 0)
        return QStringLiteral("-");
    if (duration_ms < 1000)
        return QStringLiteral("%1ms").arg(duration_ms);
    return QStringLiteral("%1.%2s").arg(duration_ms / 1000).arg((duration_ms % 1000) / 100);
}

QJsonObject history_row_json(QSqlQuery& q) {
    return QJsonObject{{"run_id", q.value(0).toString()},
                       {"job_id", q.value(1).toString()},
                       {"job_name", q.value(2).toString()},
                       {"kind", q.value(3).toString()},
                       {"trigger", q.value(4).toString()},
                       {"status", q.value(5).toString()},
                       {"exit_code", q.value(6).isNull() ? QJsonValue() : QJsonValue(q.value(6).toInt())},
                       {"started_at", q.value(7).toString()},
                       {"finished_at", q.value(8).toString()},
                       {"duration_ms", q.value(9).isNull() ? QJsonValue() : QJsonValue(static_cast<qint64>(q.value(9).toLongLong()))},
                       {"timeout_sec", q.value(10).isNull() ? QJsonValue() : QJsonValue(q.value(10).toInt())},
                       {"command", q.value(11).toString()},
                       {"error", q.value(12).toString()}};
}

bool consume_history_common(QStringList& args, int* limit, QString* selector) {
    *limit = 20;
    for (int i = 0; i < args.size(); ++i) {
        const QString token = args.at(i);
        if (token == QStringLiteral("--limit")) {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "--limit requires a value\n");
                return false;
            }
            bool ok = false;
            const int value = args.at(i + 1).toInt(&ok);
            if (!ok || value <= 0) {
                std::fprintf(stderr, "--limit requires a positive integer\n");
                return false;
            }
            *limit = value;
            args.removeAt(i + 1);
            args.removeAt(i);
            --i;
            if (*limit <= 0) {
                std::fprintf(stderr, "--limit requires a positive integer\n");
                return false;
            }
        }
    }
    *selector = args.join(' ').trimmed();
    return true;
}

void bind_history_selector(QSqlQuery& q, const QString& selector, const QString& job_id = {}) {
    const QString needle = QStringLiteral("%") + selector + QStringLiteral("%");
    q.bindValue(QStringLiteral(":job_id"), job_id.isEmpty() ? selector : job_id);
    q.bindValue(QStringLiteral(":needle"), needle);
}

int emit_jobs_history(const QString& profile, bool json, QStringList args, bool failures_only = false) {
    int limit = 20;
    QString selector;
    if (!consume_history_common(args, &limit, &selector))
        return 2;

    QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
    QString selected_job_id;
    if (!selector.isEmpty()) {
        const int idx = find_job_index(jobs, selector);
        if (idx >= 0)
            selected_job_id = jobs.at(idx).toObject().value("id").toString();
    }

    DaemonHistoryDb h(profile);
    if (!h.ok()) {
        std::fprintf(stderr, "history db unavailable: %s\n", qUtf8Printable(h.error));
        return 7;
    }
    QString sql = QStringLiteral(
        "SELECT run_id, job_id, job_name, kind, trigger, status, exit_code, started_at, finished_at, "
        "duration_ms, timeout_sec, command, error FROM daemon_job_runs");
    QStringList where;
    if (failures_only)
        where << QStringLiteral("status IN ('failed','timeout','stale-timeout')");
    if (!selector.isEmpty())
        where << QStringLiteral("(job_id=:job_id OR job_name LIKE :needle OR run_id LIKE :needle)");
    if (!where.isEmpty())
        sql += QStringLiteral(" WHERE ") + where.join(QStringLiteral(" AND "));
    sql += QStringLiteral(" ORDER BY started_at DESC LIMIT :limit");

    QSqlQuery q(h.db);
    q.prepare(sql);
    if (!selector.isEmpty())
        bind_history_selector(q, selector, selected_job_id);
    q.bindValue(QStringLiteral(":limit"), limit);
    if (!q.exec()) {
        std::fprintf(stderr, "history query failed: %s\n", qUtf8Printable(q.lastError().text()));
        return 7;
    }

    QJsonArray rows;
    if (!json) {
        std::printf("%-20s %-22s %-10s %-14s %-8s %-8s %s\n",
                    "started", "job", "trigger", "status", "exit", "duration", "run");
    }
    while (q.next()) {
        const QJsonObject row = history_row_json(q);
        rows.append(row);
        if (!json) {
            const QString started = row.value("started_at").toString().left(20);
            const QString job_name = row.value("job_name").toString().left(22);
            const QString trigger = row.value("trigger").toString().left(10);
            const QString status = row.value("status").toString().left(14);
            const QString exit_label = row.value("exit_code").isUndefined() || row.value("exit_code").isNull()
                                           ? QStringLiteral("-")
                                           : QString::number(row.value("exit_code").toInt());
            const QString dur = row.value("duration_ms").isUndefined() || row.value("duration_ms").isNull()
                                    ? QStringLiteral("-")
                                    : duration_label(static_cast<qint64>(row.value("duration_ms").toDouble()));
            std::printf("%-20s %-22s %-10s %-14s %-8s %-8s %s\n",
                        qUtf8Printable(started),
                        qUtf8Printable(job_name),
                        qUtf8Printable(trigger),
                        qUtf8Printable(status),
                        qUtf8Printable(exit_label),
                        qUtf8Printable(dur),
                        qUtf8Printable(row.value("run_id").toString()));
        }
    }
    if (json)
        std::printf("%s\n", QJsonDocument(QJsonObject{{"profile", profile}, {"runs", rows}})
                                .toJson(QJsonDocument::Compact)
                                .constData());
    else if (rows.isEmpty())
        std::printf("no job run history\n");
    return 0;
}

int emit_jobs_stats(const QString& profile, bool json, QStringList args) {
    int limit = 0;
    QString selector;
    if (!consume_history_common(args, &limit, &selector))
        return 2;
    Q_UNUSED(limit);

    QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
    QString selected_job_id;
    if (!selector.isEmpty()) {
        const int idx = find_job_index(jobs, selector);
        if (idx >= 0)
            selected_job_id = jobs.at(idx).toObject().value("id").toString();
    }

    DaemonHistoryDb h(profile);
    if (!h.ok()) {
        std::fprintf(stderr, "history db unavailable: %s\n", qUtf8Printable(h.error));
        return 7;
    }
    QString sql = QStringLiteral(
        "SELECT COUNT(*), "
        "SUM(CASE WHEN status='ok' THEN 1 ELSE 0 END), "
        "SUM(CASE WHEN status IN ('failed','timeout','stale-timeout') THEN 1 ELSE 0 END), "
        "SUM(CASE WHEN status='timeout' THEN 1 ELSE 0 END), "
        "SUM(CASE WHEN status='stale-timeout' THEN 1 ELSE 0 END), "
        "AVG(duration_ms), MAX(started_at) FROM daemon_job_runs");
    if (!selector.isEmpty())
        sql += QStringLiteral(" WHERE (job_id=:job_id OR job_name LIKE :needle OR run_id LIKE :needle)");
    QSqlQuery q(h.db);
    q.prepare(sql);
    if (!selector.isEmpty())
        bind_history_selector(q, selector, selected_job_id);
    if (!q.exec() || !q.next()) {
        std::fprintf(stderr, "stats query failed: %s\n", qUtf8Printable(q.lastError().text()));
        return 7;
    }
    const int total = q.value(0).toInt();
    const int ok = q.value(1).toInt();
    const int failed = q.value(2).toInt();
    const int timeout = q.value(3).toInt();
    const int stale = q.value(4).toInt();
    const double avg_ms = q.value(5).toDouble();
    const QString last = q.value(6).toString();
    QJsonObject out{{"profile", profile},
                    {"selector", selector},
                    {"total", total},
                    {"ok", ok},
                    {"failed", failed},
                    {"timeout", timeout},
                    {"stale_timeout", stale},
                    {"avg_duration_ms", q.value(5).isNull() ? QJsonValue() : QJsonValue(avg_ms)},
                    {"last_started_at", last}};
    if (json) {
        std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
    } else {
        std::printf("runs         %d\n", total);
        std::printf("ok           %d\n", ok);
        std::printf("failed       %d\n", failed);
        std::printf("timeout      %d\n", timeout);
        std::printf("stale        %d\n", stale);
        std::printf("avg duration %s\n", qUtf8Printable(q.value(5).isNull()
                                                        ? QStringLiteral("-")
                                                        : duration_label(static_cast<qint64>(avg_ms))));
        std::printf("last start   %s\n", qUtf8Printable(last.isEmpty() ? QStringLiteral("-") : last));
    }
    return 0;
}

int jobs_save_update(const QString& profile, const QJsonArray& jobs) {
    QJsonObject doc = load_jobs_doc(profile);
    doc["jobs"] = jobs;
    doc["updated_at"] = now_utc();
    QString error;
    if (!save_jobs_doc(profile, doc, &error)) {
        std::fprintf(stderr, "%s\n", qUtf8Printable(error));
        return 7;
    }
    return 0;
}

bool consume_value(QStringList& args, int& i, const QString& flag, QString* out) {
    if (i + 1 >= args.size()) {
        std::fprintf(stderr, "%s requires a value\n", qUtf8Printable(flag));
        return false;
    }
    *out = args.takeAt(i + 1);
    args.removeAt(i);
    --i;
    return true;
}

bool consume_int(QStringList& args, int& i, const QString& flag, int* out) {
    QString raw;
    if (!consume_value(args, i, flag, &raw))
        return false;
    bool ok = false;
    const int n = raw.toInt(&ok);
    if (!ok || n < 0) {
        std::fprintf(stderr, "%s requires a non-negative integer\n", qUtf8Printable(flag));
        return false;
    }
    *out = n;
    return true;
}

struct DaemonReadinessOptions {
    QString symbol = QStringLiteral("BTC");
    int tick_stale_sec = 45;
    int market_stale_sec = 300;
    int model_output_stale_sec = 600;
    int train_stale_sec = 86400;
    int min_samples = 30;
    int min_fresh_sources = 2;
};

int readiness_rank(const QString& status) {
    if (status == QStringLiteral("not_safe"))
        return 3;
    if (status == QStringLiteral("degraded"))
        return 2;
    if (status == QStringLiteral("watch"))
        return 1;
    return 0;
}

QString merge_readiness_status(QString current, const QString& next) {
    return readiness_rank(next) > readiness_rank(current) ? next : current;
}

QString readiness_overall_label(const QString& status) {
    if (status == QStringLiteral("not_safe"))
        return QStringLiteral("NOT SAFE");
    if (status == QStringLiteral("degraded"))
        return QStringLiteral("DEGRADED");
    if (status == QStringLiteral("watch"))
        return QStringLiteral("WATCH");
    return QStringLiteral("READY");
}

QString age_label_ms(qint64 age_ms) {
    if (age_ms < 0)
        return QStringLiteral("-");
    const qint64 sec = age_ms / 1000;
    if (sec < 90)
        return QStringLiteral("%1s").arg(sec);
    const qint64 min = sec / 60;
    if (min < 90)
        return QStringLiteral("%1m").arg(min);
    const qint64 hour = min / 60;
    if (hour < 48)
        return QStringLiteral("%1h").arg(hour);
    return QStringLiteral("%1d").arg(hour / 24);
}

bool init_readiness_runtime(const QString& profile, QString* error) {
    static headless::HeadlessRuntime rt;
    auto r = rt.init(profile);
    if (!r.ok) {
        if (error)
            *error = r.error;
        return false;
    }
    return true;
}

QJsonObject readiness_check(QString key,
                            QString label,
                            QString status,
                            QString detail,
                            const QStringList& reasons = {},
                            const QJsonObject& metrics = {}) {
    return QJsonObject{{"key", key},
                       {"label", label},
                       {"status", status},
                       {"detail", detail},
                       {"reasons", QJsonArray::fromStringList(reasons)},
                       {"metrics", metrics}};
}

qint64 latest_market_snapshot_ms(const QString& symbol, int* count, QString* error) {
    auto r = Database::instance().execute(
        QStringLiteral("SELECT COUNT(*), MAX(observed_at) FROM edge_prediction_market_snapshots WHERE symbol=?"),
        {symbol.trimmed().toUpper()});
    if (r.is_err()) {
        if (error)
            *error = QString::fromStdString(r.error());
        return 0;
    }
    auto& q = r.value();
    if (!q.next()) {
        if (count)
            *count = 0;
        return 0;
    }
    if (count)
        *count = q.value(0).toInt();
    return q.value(1).toLongLong();
}

QJsonObject build_daemon_readiness(const QString& profile, const DaemonReadinessOptions& opt) {
    const qint64 now = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    QString overall = QStringLiteral("ready");
    QJsonArray checks;
    QStringList blockers;
    QStringList warnings;

    auto add_check = [&](const QJsonObject& check) {
        const QString status = check.value(QStringLiteral("status")).toString(QStringLiteral("ready"));
        overall = merge_readiness_status(overall, status);
        const QJsonArray reasons = check.value(QStringLiteral("reasons")).toArray();
        for (const auto& v : reasons) {
            const QString reason = v.toString();
            if (reason.isEmpty())
                continue;
            if (status == QStringLiteral("not_safe"))
                blockers << reason;
            else if (status != QStringLiteral("ready"))
                warnings << reason;
        }
        checks.append(check);
    };

    const QJsonObject health = daemon_health_object(profile);
    const QJsonObject jobs = health.value(QStringLiteral("jobs")).toObject();
    QStringList scheduler_reasons;
    QString scheduler_status = QStringLiteral("ready");
    if (!health.value(QStringLiteral("running")).toBool()) {
        scheduler_status = QStringLiteral("not_safe");
        scheduler_reasons << QStringLiteral("daemon is not running");
    }
    if (jobs.value(QStringLiteral("enabled")).toInt() <= 0) {
        scheduler_status = merge_readiness_status(scheduler_status, QStringLiteral("degraded"));
        scheduler_reasons << QStringLiteral("no enabled daemon jobs");
    }
    if (jobs.value(QStringLiteral("failed")).toInt() > 0 || jobs.value(QStringLiteral("stale")).toInt() > 0) {
        scheduler_status = merge_readiness_status(scheduler_status, QStringLiteral("not_safe"));
        scheduler_reasons << QStringLiteral("one or more daemon jobs currently failed or stale");
    } else if (jobs.value(QStringLiteral("failed_history")).toInt() > 0) {
        scheduler_status = merge_readiness_status(scheduler_status, QStringLiteral("watch"));
        scheduler_reasons << QStringLiteral("daemon has acknowledged historical job failures");
    }
    add_check(readiness_check(QStringLiteral("daemon"),
                              QStringLiteral("Daemon Scheduler"),
                              scheduler_status,
                              health.value(QStringLiteral("running")).toBool()
                                  ? QStringLiteral("daemon is running")
                                  : QStringLiteral("daemon process is unavailable"),
                              scheduler_reasons,
                              QJsonObject{{"running", health.value(QStringLiteral("running")).toBool()},
                                          {"installed", health.value(QStringLiteral("installed")).toBool()},
                                          {"jobs", jobs}}));

    QString init_error;
    const bool db_ready = init_readiness_runtime(profile, &init_error);
    if (!db_ready) {
        add_check(readiness_check(QStringLiteral("profile_db"),
                                  QStringLiteral("Local Profile DB"),
                                  QStringLiteral("not_safe"),
                                  QStringLiteral("profile database could not be opened"),
                                  {init_error}));
        return QJsonObject{{"profile", profile},
                           {"symbol", opt.symbol},
                           {"overall", readiness_overall_label(overall)},
                           {"status", overall},
                           {"trade_gate", QStringLiteral("NO TRADE")},
                           {"blockers", QJsonArray::fromStringList(blockers)},
                           {"warnings", QJsonArray::fromStringList(warnings)},
                           {"checks", checks},
                           {"generated_at", now_utc()}};
    }

    auto ticks_result = EdgePredictionModelRepository::instance().list_raw_ticks(opt.symbol, 1000);
    QStringList tick_reasons;
    QString tick_status = QStringLiteral("ready");
    QJsonObject latest_by_source;
    qint64 freshest_direct = 0;
    int fresh_direct_sources = 0;
    int advisory_fresh = 0;
    const QStringList direct_sources{QStringLiteral("coinbase"),
                                     QStringLiteral("kraken"),
                                     QStringLiteral("binance"),
                                     QStringLiteral("binanceus")};
    if (ticks_result.is_err()) {
        tick_status = QStringLiteral("not_safe");
        tick_reasons << QStringLiteral("raw tick store is unavailable: ") + QString::fromStdString(ticks_result.error());
    } else {
        QHash<QString, EdgePredictionRawTick> latest;
        for (const auto& t : ticks_result.value()) {
            if (t.source.endsWith(QStringLiteral("-1m-close")))
                continue;
            if (!latest.contains(t.source) || t.received_ts > latest.value(t.source).received_ts)
                latest.insert(t.source, t);
        }
        for (auto it = latest.cbegin(); it != latest.cend(); ++it) {
            const qint64 age_ms = it.value().received_ts > 0 ? std::max<qint64>(0, now - it.value().received_ts) : -1;
            latest_by_source.insert(it.key(), QJsonObject{{"price", it.value().price},
                                                          {"age_ms", age_ms},
                                                          {"age", age_label_ms(age_ms)},
                                                          {"received_ts", QString::number(it.value().received_ts)}});
            if (direct_sources.contains(it.key())) {
                freshest_direct = std::max(freshest_direct, it.value().received_ts);
                if (age_ms >= 0 && age_ms <= static_cast<qint64>(opt.tick_stale_sec) * 1000)
                    ++fresh_direct_sources;
            } else if (it.key() == QStringLiteral("bitcointicker") &&
                       age_ms >= 0 && age_ms <= static_cast<qint64>(opt.tick_stale_sec) * 1000) {
                ++advisory_fresh;
            }
        }
        const qint64 direct_age_ms = freshest_direct > 0 ? std::max<qint64>(0, now - freshest_direct) : -1;
        if (freshest_direct <= 0) {
            tick_status = QStringLiteral("not_safe");
            tick_reasons << QStringLiteral("no live direct exchange tick has been stored");
        } else if (direct_age_ms > static_cast<qint64>(opt.tick_stale_sec) * 1000) {
            tick_status = QStringLiteral("not_safe");
            tick_reasons << QStringLiteral("freshest direct exchange tick is stale");
        }
        if (fresh_direct_sources < opt.min_fresh_sources) {
            tick_status = merge_readiness_status(tick_status, QStringLiteral("degraded"));
            tick_reasons << QStringLiteral("fewer than required live direct exchange sources are fresh");
        }
    }
    add_check(readiness_check(QStringLiteral("ticks"),
                              QStringLiteral("Live BTC Ticks"),
                              tick_status,
                              QStringLiteral("%1 fresh direct source(s), %2 advisory source(s)")
                                  .arg(fresh_direct_sources)
                                  .arg(advisory_fresh),
                              tick_reasons,
                              QJsonObject{{"symbol", opt.symbol},
                                          {"tick_stale_sec", opt.tick_stale_sec},
                                          {"min_fresh_sources", opt.min_fresh_sources},
                                          {"fresh_direct_sources", fresh_direct_sources},
                                          {"fresh_advisory_sources", advisory_fresh},
                                          {"latest_by_source", latest_by_source}}));

    int snapshot_count = 0;
    QString snapshot_error;
    const qint64 latest_snapshot = latest_market_snapshot_ms(opt.symbol, &snapshot_count, &snapshot_error);
    QString market_status = QStringLiteral("ready");
    QStringList market_reasons;
    const qint64 snapshot_age_ms = latest_snapshot > 0 ? std::max<qint64>(0, now - latest_snapshot) : -1;
    if (!snapshot_error.isEmpty()) {
        market_status = QStringLiteral("degraded");
        market_reasons << QStringLiteral("prediction-market snapshot store is unavailable: ") + snapshot_error;
    } else if (snapshot_count <= 0) {
        market_status = QStringLiteral("degraded");
        market_reasons << QStringLiteral("no prediction-market snapshots have been stored yet");
    } else if (snapshot_age_ms > static_cast<qint64>(opt.market_stale_sec) * 1000) {
        market_status = QStringLiteral("degraded");
        market_reasons << QStringLiteral("latest prediction-market snapshot is stale");
    }
    add_check(readiness_check(QStringLiteral("prediction_markets"),
                              QStringLiteral("Prediction Market Snapshots"),
                              market_status,
                              snapshot_count > 0
                                  ? QStringLiteral("latest snapshot age %1").arg(age_label_ms(snapshot_age_ms))
                                  : QStringLiteral("no local market snapshots"),
                              market_reasons,
                              QJsonObject{{"snapshot_count", snapshot_count},
                                          {"latest_observed_at", QString::number(latest_snapshot)},
                                          {"age_ms", snapshot_age_ms},
                                          {"market_stale_sec", opt.market_stale_sec}}));

    auto models_result = EdgePredictionModelRepository::instance().list_models(opt.symbol);
    auto outputs_result = EdgePredictionModelRepository::instance().list_model_outputs(opt.symbol, now, 64);
    const QStringList horizons{QStringLiteral("5m"), QStringLiteral("15m"), QStringLiteral("1h"), QStringLiteral("daily")};
    QJsonArray model_rows;
    QString model_status = QStringLiteral("ready");
    QStringList model_reasons;
    if (models_result.is_err()) {
        model_status = QStringLiteral("not_safe");
        model_reasons << QStringLiteral("model store is unavailable: ") + QString::fromStdString(models_result.error());
    } else {
        QHash<QString, EdgePredictionModelRecord> by_horizon;
        for (const auto& m : models_result.value())
            by_horizon.insert(m.horizon, m);
        for (const auto& h : horizons) {
            QJsonObject row{{"horizon", h}};
            if (!by_horizon.contains(h)) {
                model_status = QStringLiteral("not_safe");
                model_reasons << QStringLiteral("missing trained model for ") + h;
                row["status"] = QStringLiteral("missing");
                model_rows.append(row);
                continue;
            }
            const auto m = by_horizon.value(h);
            const qint64 trained_age = m.trained_at > 0 ? std::max<qint64>(0, now - m.trained_at) : -1;
            QString row_status = QStringLiteral("ready");
            if (m.sample_count < opt.min_samples) {
                row_status = QStringLiteral("not_safe");
                model_status = QStringLiteral("not_safe");
                model_reasons << QStringLiteral("%1 model has only %2 sample(s)").arg(h).arg(m.sample_count);
            } else if (trained_age > static_cast<qint64>(opt.train_stale_sec) * 1000) {
                row_status = QStringLiteral("degraded");
                model_status = merge_readiness_status(model_status, QStringLiteral("degraded"));
                model_reasons << QStringLiteral("%1 model training is stale").arg(h);
            }
            row["status"] = row_status;
            row["sample_count"] = m.sample_count;
            row["brier_score"] = m.brier_score;
            row["trained_at"] = QString::number(m.trained_at);
            row["trained_age_ms"] = trained_age;
            model_rows.append(row);
        }
    }
    add_check(readiness_check(QStringLiteral("models"),
                              QStringLiteral("Horizon Models"),
                              model_status,
                              QStringLiteral("requires %1 settled sample(s) per horizon").arg(opt.min_samples),
                              model_reasons,
                              QJsonObject{{"symbol", opt.symbol},
                                          {"min_samples", opt.min_samples},
                                          {"train_stale_sec", opt.train_stale_sec},
                                          {"horizons", model_rows}}));

    QString output_status = QStringLiteral("ready");
    QStringList output_reasons;
    QJsonArray output_rows;
    if (outputs_result.is_err()) {
        output_status = QStringLiteral("degraded");
        output_reasons << QStringLiteral("model output snapshots are unavailable: ") + QString::fromStdString(outputs_result.error());
    } else {
        QHash<QString, EdgePredictionModelOutput> latest_output;
        for (const auto& o : outputs_result.value()) {
            if (!latest_output.contains(o.horizon) || o.as_of > latest_output.value(o.horizon).as_of)
                latest_output.insert(o.horizon, o);
        }
        for (const auto& h : horizons) {
            QJsonObject row{{"horizon", h}};
            if (!latest_output.contains(h)) {
                output_status = merge_readiness_status(output_status, QStringLiteral("degraded"));
                output_reasons << QStringLiteral("no published model output for ") + h;
                row["status"] = QStringLiteral("missing");
                output_rows.append(row);
                continue;
            }
            const auto o = latest_output.value(h);
            const qint64 age_ms = o.as_of > 0 ? std::max<qint64>(0, now - o.as_of) : -1;
            QString row_status = QStringLiteral("ready");
            if (age_ms > static_cast<qint64>(opt.model_output_stale_sec) * 1000) {
                row_status = QStringLiteral("degraded");
                output_status = merge_readiness_status(output_status, QStringLiteral("degraded"));
                output_reasons << QStringLiteral("%1 model output is stale").arg(h);
            }
            if (o.readiness != QStringLiteral("ready")) {
                row_status = merge_readiness_status(row_status, QStringLiteral("degraded"));
                output_status = merge_readiness_status(output_status, QStringLiteral("degraded"));
                output_reasons << QStringLiteral("%1 model output readiness is %2").arg(h, o.readiness);
            }
            row["status"] = row_status;
            row["readiness"] = o.readiness;
            row["probability"] = o.probability;
            row["confidence"] = o.confidence;
            row["sample_count"] = o.sample_count;
            row["as_of"] = QString::number(o.as_of);
            row["age_ms"] = age_ms;
            output_rows.append(row);
        }
    }
    add_check(readiness_check(QStringLiteral("model_outputs"),
                              QStringLiteral("Published Model Outputs"),
                              output_status,
                              QStringLiteral("latest probability snapshots by horizon"),
                              output_reasons,
                              QJsonObject{{"model_output_stale_sec", opt.model_output_stale_sec},
                                          {"outputs", output_rows}}));

    const QString trade_gate = overall == QStringLiteral("ready") ? QStringLiteral("TRADE GATE READY")
                              : overall == QStringLiteral("watch") ? QStringLiteral("WATCH")
                              : overall == QStringLiteral("degraded") ? QStringLiteral("NO TRADE: DEGRADED")
                                                                        : QStringLiteral("NO TRADE: NOT SAFE");
    warnings.removeDuplicates();
    blockers.removeDuplicates();
    return QJsonObject{{"profile", profile},
                       {"symbol", opt.symbol},
                       {"overall", readiness_overall_label(overall)},
                       {"status", overall},
                       {"trade_gate", trade_gate},
                       {"blockers", QJsonArray::fromStringList(blockers)},
                       {"warnings", QJsonArray::fromStringList(warnings)},
                       {"checks", checks},
                       {"thresholds", QJsonObject{{"tick_stale_sec", opt.tick_stale_sec},
                                                  {"market_stale_sec", opt.market_stale_sec},
                                                  {"model_output_stale_sec", opt.model_output_stale_sec},
                                                  {"train_stale_sec", opt.train_stale_sec},
                                                  {"min_samples", opt.min_samples},
                                                  {"min_fresh_sources", opt.min_fresh_sources}}},
                       {"generated_at", now_utc()}};
}

bool parse_readiness_options(QStringList& args, DaemonReadinessOptions* opt) {
    for (int i = 0; i < args.size(); ++i) {
        const QString flag = args.at(i);
        if (flag == QStringLiteral("--symbol")) {
            QString value;
            if (!consume_value(args, i, flag, &value))
                return false;
            opt->symbol = value.trimmed().isEmpty() ? QStringLiteral("BTC") : value.trimmed().toUpper();
        } else if (flag == QStringLiteral("--tick-stale-sec")) {
            if (!consume_int(args, i, flag, &opt->tick_stale_sec))
                return false;
        } else if (flag == QStringLiteral("--market-stale-sec")) {
            if (!consume_int(args, i, flag, &opt->market_stale_sec))
                return false;
        } else if (flag == QStringLiteral("--model-stale-sec")) {
            if (!consume_int(args, i, flag, &opt->model_output_stale_sec))
                return false;
        } else if (flag == QStringLiteral("--train-stale-sec")) {
            if (!consume_int(args, i, flag, &opt->train_stale_sec))
                return false;
        } else if (flag == QStringLiteral("--min-samples")) {
            if (!consume_int(args, i, flag, &opt->min_samples))
                return false;
        } else if (flag == QStringLiteral("--min-fresh-sources")) {
            if (!consume_int(args, i, flag, &opt->min_fresh_sources))
                return false;
        }
    }
    if (opt->tick_stale_sec < 1 || opt->market_stale_sec < 1 ||
        opt->model_output_stale_sec < 1 || opt->train_stale_sec < 1 ||
        opt->min_samples < 1 || opt->min_fresh_sources < 1) {
        std::fprintf(stderr, "readiness thresholds must be positive integers\n");
        return false;
    }
    if (!args.isEmpty()) {
        std::fprintf(stderr, "unknown args: %s\n", qUtf8Printable(args.join(' ')));
        return false;
    }
    return true;
}

int emit_daemon_readiness(const QString& profile, bool json, QStringList args) {
    DaemonReadinessOptions opt;
    if (!parse_readiness_options(args, &opt)) {
        std::fprintf(stderr,
                     "usage: daemon readiness [--symbol BTC] [--tick-stale-sec N] "
                     "[--market-stale-sec N] [--model-stale-sec N] [--train-stale-sec N] "
                     "[--min-samples N] [--min-fresh-sources N]\n");
        return 2;
    }

    const QJsonObject out = build_daemon_readiness(profile, opt);
    if (json) {
        std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("readiness    %s\n", qUtf8Printable(out.value("overall").toString()));
    std::printf("trade gate   %s\n", qUtf8Printable(out.value("trade_gate").toString()));
    std::printf("profile      %s\n", qUtf8Printable(profile));
    std::printf("symbol       %s\n", qUtf8Printable(opt.symbol));
    const QJsonArray checks = out.value("checks").toArray();
    std::printf("\n%-24s %-10s %s\n", "CHECK", "STATUS", "DETAIL");
    for (const auto& v : checks) {
        const QJsonObject c = v.toObject();
        std::printf("%-24s %-10s %s\n",
                    qUtf8Printable(c.value("label").toString().left(24)),
                    qUtf8Printable(c.value("status").toString().toUpper().left(10)),
                    qUtf8Printable(c.value("detail").toString()));
        const QJsonArray reasons = c.value("reasons").toArray();
        for (const auto& r : reasons)
            if (!r.toString().isEmpty())
                std::printf("  - %s\n", qUtf8Printable(r.toString()));
    }
    const QJsonArray blockers = out.value("blockers").toArray();
    if (!blockers.isEmpty()) {
        std::printf("\nBLOCKERS\n");
        for (const auto& b : blockers)
            std::printf("- %s\n", qUtf8Printable(b.toString()));
    }
    const QJsonArray warnings = out.value("warnings").toArray();
    if (!warnings.isEmpty()) {
        std::printf("\nWARNINGS\n");
        for (const auto& w : warnings)
            std::printf("- %s\n", qUtf8Printable(w.toString()));
    }
    return out.value("status").toString() == QStringLiteral("not_safe") ? 4 : 0;
}

void reconcile_stale_running_jobs(const QString& profile, const QDateTime& now) {
    QJsonObject doc = load_jobs_doc(profile);
    QJsonArray jobs = doc.value("jobs").toArray();
    bool changed = false;
    for (int i = 0; i < jobs.size(); ++i) {
        QJsonObject job = jobs.at(i).toObject();
        if (!job.value("running").toBool())
            continue;
        const QDateTime started = parse_utc(job.value("last_started_at").toString());
        if (!started.isValid())
            continue;
        const int timeout = job_timeout_sec(job);
        if (started.addSecs(timeout + 15) > now)
            continue;
        QString run_id = job.value("current_run_id").toString();
        if (run_id.isEmpty())
            run_id = record_job_run_start(profile, job, QStringLiteral("scheduled"), started.toString(Qt::ISODateWithMs));
        record_job_run_finish(profile, run_id, QStringLiteral("stale-timeout"), -1, {}, {},
                              QStringLiteral("job exceeded timeout or daemon restarted while it was running"));
        job["running"] = false;
        job["last_status"] = QStringLiteral("stale-timeout");
        job["last_exit_code"] = -1;
        job["last_error"] = QStringLiteral("job exceeded timeout or daemon restarted while it was running");
        job["fail_count"] = job.value("fail_count").toInt() + 1;
        job["updated_at"] = now_utc();
        job["current_run_id"] = QString();
        if (job.value("interval_sec").toInt(0) > 0)
            job["next_run_at"] = now.toString(Qt::ISODateWithMs);
        jobs.replace(i, job);
        changed = true;
        append_job_log(profile, QStringLiteral("scheduled-stale id=%1 timeout=%2s")
                                    .arg(job.value("id").toString())
                                    .arg(timeout));
    }
    if (changed)
        jobs_save_update(profile, jobs);
}

QJsonObject parse_job_spec(QString kind,
                           QStringList args,
                           QString* name,
                           int* every_sec,
                           int* timeout_sec,
                           bool* enabled) {
    kind = kind.trimmed().toLower();
    QJsonObject spec;
    *name = {};
    *every_sec = 0;
    *timeout_sec = 0;
    *enabled = true;

    for (int i = 0; i < args.size(); ++i) {
        const QString flag = args.at(i);
        if (flag == "--name") {
            if (!consume_value(args, i, flag, name)) return {};
        } else if (flag == "--every-sec" || flag == "--interval-sec") {
            if (!consume_int(args, i, flag, every_sec)) return {};
        } else if (flag == "--timeout-sec") {
            if (!consume_int(args, i, flag, timeout_sec)) return {};
            if (*timeout_sec < 1) {
                std::fprintf(stderr, "--timeout-sec requires a positive integer\n");
                return {};
            }
        } else if (flag == "--disabled") {
            args.removeAt(i--);
            *enabled = false;
        }
    }

    if (kind == "command") {
        const int sep = args.indexOf(QStringLiteral("--"));
        QStringList command = sep >= 0 ? args.mid(sep + 1) : args;
        if (command.isEmpty()) {
            std::fprintf(stderr, "usage: daemon jobs add command [--name N] [--every-sec N] [--timeout-sec N] -- <cli args...>\n");
            return {};
        }
        spec["command"] = strings_to_json_array(command);
        if (name->isEmpty()) *name = QStringLiteral("command ") + command.join(' ').left(32);
        return spec;
    }

    auto take_named = [&](const QString& flag, QString key) -> bool {
        for (int i = 0; i < args.size(); ++i) {
            if (args.at(i) == flag) {
                QString val;
                if (!consume_value(args, i, flag, &val)) return false;
                spec[key] = val;
                return true;
            }
        }
        return false;
    };
    auto take_named_int = [&](const QString& flag, QString key) -> bool {
        for (int i = 0; i < args.size(); ++i) {
            if (args.at(i) == flag) {
                int val = 0;
                if (!consume_int(args, i, flag, &val)) return false;
                spec[key] = val;
                return true;
            }
        }
        return false;
    };

    if (kind == "brief" || kind == "risk" || kind == "thesis" || kind == "radar") {
        QString target;
        if (!take_named("--target", "target"))
            target = args.join(' ').trimmed();
        else
            target = spec.value("target").toString();
        if (target.isEmpty()) {
            std::fprintf(stderr, "usage: daemon jobs add %s <target> [--every-sec N]\n", qUtf8Printable(kind));
            return {};
        }
        spec["target"] = target;
        if (name->isEmpty()) *name = kind + QStringLiteral(" ") + target;
        return spec;
    }
    if (kind == "ai") {
        QString workflow;
        if (!take_named("--workflow", "workflow")) {
            workflow = args.isEmpty() ? QStringLiteral("brief") : args.takeFirst();
            spec["workflow"] = workflow;
        }
        QString target;
        if (!take_named("--target", "target"))
            target = args.join(' ').trimmed();
        else
            target = spec.value("target").toString();
        if (target.isEmpty()) {
            std::fprintf(stderr, "usage: daemon jobs add ai <brief|risk|thesis|radar> <target> [--every-sec N]\n");
            return {};
        }
        spec["target"] = target;
        if (name->isEmpty()) *name = spec.value("workflow").toString("brief") + QStringLiteral(" ") + target;
        return spec;
    }
    if (kind == "notebook") {
        take_named_int("--cell", "cell");
        QString selector;
        if (!take_named("--selector", "selector"))
            selector = args.join(' ').trimmed();
        else
            selector = spec.value("selector").toString();
        if (selector.isEmpty()) {
            std::fprintf(stderr, "usage: daemon jobs add notebook <id-title-or-path> [--cell N] [--every-sec N]\n");
            return {};
        }
        spec["selector"] = selector;
        if (name->isEmpty()) *name = QStringLiteral("notebook ") + selector;
        return spec;
    }
    if (kind == "paper-strategy" || kind == "paper") {
        kind = QStringLiteral("paper-strategy");
        QString strategy;
        if (!take_named("--strategy", "strategy")) {
            strategy = args.isEmpty() ? QStringLiteral("meanrev") : args.takeFirst();
            spec["strategy"] = strategy;
        }
        take_named("--symbols", "symbols");
        take_named_int("--max-iters", "max_iters");
        take_named_int("--interval-sec", "interval_sec");
        if (name->isEmpty()) *name = QStringLiteral("paper ") + spec.value("strategy").toString("meanrev");
        return spec;
    }
    if (kind == "notify") {
        take_named("--provider", "provider");
        take_named("--level", "level");
        take_named("--title", "title");
        take_named("--message", "message");
        if (spec.value("title").toString().isEmpty()) {
            spec["title"] = args.isEmpty() ? QStringLiteral("OpenTerminal daemon") : args.takeFirst();
        }
        if (spec.value("message").toString().isEmpty()) {
            spec["message"] = args.join(' ').trimmed();
        }
        if (spec.value("message").toString().isEmpty()) {
            std::fprintf(stderr, "usage: daemon jobs add notify --title T --message M [--provider P]\n");
            return {};
        }
        if (name->isEmpty()) *name = QStringLiteral("notify ") + spec.value("title").toString();
        return spec;
    }
    if (kind == "health-check") {
        if (name->isEmpty()) *name = QStringLiteral("daemon health");
        return spec;
    }

    std::fprintf(stderr, "unknown daemon job kind: %s\n", qUtf8Printable(kind));
    return {};
}

int daemon_jobs_command(const QString& profile, bool json, QStringList args) {
    const QString sub = args.isEmpty() ? QStringLiteral("list") : args.takeFirst().trimmed().toLower();
    if (sub == "list" || sub == "ls")
        return emit_jobs_list(profile, json);
    if (sub == "history" || sub == "hist" || sub == "runs")
        return emit_jobs_history(profile, json, args);
    if (sub == "failures" || sub == "failed")
        return emit_jobs_history(profile, json, args, true);
    if (sub == "stats" || sub == "metrics")
        return emit_jobs_stats(profile, json, args);
    if (sub == "add" || sub == "create") {
        if (args.isEmpty()) {
        std::fprintf(stderr, "usage: daemon jobs add <command|brief|ai|notebook|paper-strategy|notify|health-check> ... [--timeout-sec N]\n");
            return 2;
        }
        QString kind = args.takeFirst().trimmed().toLower();
        QString name;
        int every_sec = 0;
        int timeout_sec = 0;
        bool enabled = true;
        QJsonObject spec = parse_job_spec(kind, args, &name, &every_sec, &timeout_sec, &enabled);
        if (spec.isEmpty() && kind != "health-check")
            return 2;
        if (kind == "paper") kind = QStringLiteral("paper-strategy");
        QJsonObject doc = load_jobs_doc(profile);
        QJsonArray jobs = doc.value("jobs").toArray();
        QJsonObject job = make_job(kind, name, spec, every_sec, timeout_sec, enabled);
        jobs.append(job);
        const int rc = jobs_save_update(profile, jobs);
        if (rc != 0) return rc;
        if (json) {
            std::printf("%s\n", QJsonDocument(QJsonObject{{"job", job}}).toJson(QJsonDocument::Compact).constData());
        } else {
            std::printf("added %s  %s\n", qUtf8Printable(job.value("id").toString()),
                        qUtf8Printable(job.value("name").toString()));
        }
        return 0;
    }
    if (sub == "show") {
        if (args.isEmpty()) { std::fprintf(stderr, "usage: daemon jobs show <id-or-name>\n"); return 2; }
        QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
        const int idx = find_job_index(jobs, args.join(' '));
        if (idx < 0) { std::fprintf(stderr, "job not found\n"); return 3; }
        const QJsonObject job = jobs.at(idx).toObject();
        if (json) std::printf("%s\n", QJsonDocument(job).toJson(QJsonDocument::Compact).constData());
        else std::printf("%s\n", QJsonDocument(job).toJson(QJsonDocument::Indented).constData());
        return 0;
    }
    if (sub == "remove" || sub == "rm" || sub == "delete") {
        if (args.isEmpty()) { std::fprintf(stderr, "usage: daemon jobs remove <id-or-name>\n"); return 2; }
        QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
        const int idx = find_job_index(jobs, args.join(' '));
        if (idx < 0) { std::fprintf(stderr, "job not found\n"); return 3; }
        const QJsonObject removed = jobs.at(idx).toObject();
        jobs.removeAt(idx);
        const int rc = jobs_save_update(profile, jobs);
        if (rc != 0) return rc;
        if (json) std::printf("%s\n", QJsonDocument(QJsonObject{{"removed", removed}}).toJson(QJsonDocument::Compact).constData());
        else std::printf("removed %s\n", qUtf8Printable(removed.value("id").toString()));
        return 0;
    }
    if (sub == "enable" || sub == "disable") {
        if (args.isEmpty()) { std::fprintf(stderr, "usage: daemon jobs %s <id-or-name>\n", qUtf8Printable(sub)); return 2; }
        QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
        const int idx = find_job_index(jobs, args.join(' '));
        if (idx < 0) { std::fprintf(stderr, "job not found\n"); return 3; }
        QJsonObject job = jobs.at(idx).toObject();
        job["enabled"] = (sub == "enable");
        job["updated_at"] = now_utc();
        jobs.replace(idx, job);
        const int rc = jobs_save_update(profile, jobs);
        if (rc != 0) return rc;
        if (json) std::printf("%s\n", QJsonDocument(QJsonObject{{"job", job}}).toJson(QJsonDocument::Compact).constData());
        else std::printf("%s %s\n", qUtf8Printable(sub), qUtf8Printable(job.value("id").toString()));
        return 0;
    }
    if (sub == "run") {
        if (args.isEmpty()) { std::fprintf(stderr, "usage: daemon jobs run <id-or-name>\n"); return 2; }
        QJsonObject doc = load_jobs_doc(profile);
        QJsonArray jobs = doc.value("jobs").toArray();
        const int idx = find_job_index(jobs, args.join(' '));
        if (idx < 0) { std::fprintf(stderr, "job not found\n"); return 3; }
        QJsonObject job = jobs.at(idx).toObject();
        const QString run_id = record_job_run_start(profile, job, QStringLiteral("manual"));
        append_job_log(profile, QStringLiteral("manual-start id=%1 name=\"%2\"").arg(job.value("id").toString(), job.value("name").toString()));
        const ProcessResult r = run_job_once_sync(profile, job);
        const QString status = r.exit_code == 0 ? QStringLiteral("ok") : QStringLiteral("failed");
        job["last_run_at"] = now_utc();
        job["last_exit_code"] = r.exit_code;
        job["last_status"] = status;
        job["last_output_tail"] = compact_tail(r.out + "\n" + r.err);
        job["running"] = false;
        job["current_run_id"] = QString();
        job["updated_at"] = now_utc();
        job["run_count"] = job.value("run_count").toInt() + 1;
        if (r.exit_code != 0) job["fail_count"] = job.value("fail_count").toInt() + 1;
        jobs.replace(idx, job);
        jobs_save_update(profile, jobs);
        record_job_run_finish(profile, run_id, status, r.exit_code, r.out, r.err,
                              r.exit_code == 0 ? QString() : compact_tail(r.err));
        append_job_log(profile, QStringLiteral("manual-finish id=%1 exit=%2").arg(job.value("id").toString()).arg(r.exit_code));
        if (json) std::printf("%s\n", QJsonDocument(QJsonObject{{"job", job}, {"stdout", r.out}, {"stderr", r.err}}).toJson(QJsonDocument::Compact).constData());
        else {
            std::printf("%s", qUtf8Printable(r.out));
            if (!r.err.isEmpty()) std::fprintf(stderr, "%s", qUtf8Printable(r.err));
        }
        return r.exit_code == 0 ? 0 : 5;
    }
    if (sub == "repair" || sub == "clear-running") {
        QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
        for (int i = 0; i < jobs.size(); ++i) {
            QJsonObject job = jobs.at(i).toObject();
            if (job.value("running").toBool()) {
                job["running"] = false;
                job["last_status"] = QStringLiteral("repaired");
                job["updated_at"] = now_utc();
                jobs.replace(i, job);
            }
        }
        return jobs_save_update(profile, jobs);
    }
    if (sub == "clear-failures" || sub == "clear-fails" || sub == "ack" || sub == "acknowledge") {
        const bool force = args.removeAll(QStringLiteral("--force")) > 0;
        const bool all = args.removeAll(QStringLiteral("--all")) > 0 || args.isEmpty();
        const QString selector = args.join(' ').trimmed();
        QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
        const int target_idx = all ? -1 : find_job_index(jobs, selector);
        if (!all && target_idx < 0) {
            std::fprintf(stderr, "job not found\n");
            return 3;
        }
        int cleared = 0;
        int skipped = 0;
        QJsonArray changed_jobs;
        for (int i = 0; i < jobs.size(); ++i) {
            QJsonObject job = jobs.at(i).toObject();
            const bool selected = all || target_idx == i;
            if (!selected)
                continue;
            if (job.value("fail_count").toInt() <= 0)
                continue;
            if (!force && job_has_current_failure(job)) {
                ++skipped;
                continue;
            }
            job["fail_count"] = 0;
            if (!job_has_current_failure(job))
                job["last_error"] = QString();
            job["updated_at"] = now_utc();
            jobs.replace(i, job);
            changed_jobs.append(job);
            ++cleared;
        }
        if (!all && selector.isEmpty()) {
            std::fprintf(stderr, "usage: daemon jobs clear-failures [--all|<id-or-name>] [--force]\n");
            return 2;
        }
        const int rc = jobs_save_update(profile, jobs);
        if (rc != 0)
            return rc;
        append_job_log(profile, QStringLiteral("failures-cleared count=%1 skipped=%2 force=%3")
                                    .arg(cleared)
                                    .arg(skipped)
                                    .arg(force ? "yes" : "no"));
        if (json) {
            std::printf("%s\n", QJsonDocument(QJsonObject{{"cleared", cleared},
                                                          {"skipped_current_failures", skipped},
                                                          {"force", force},
                                                          {"jobs", changed_jobs}})
                                    .toJson(QJsonDocument::Compact)
                                    .constData());
        } else {
            std::printf("cleared failure history for %d job%s", cleared, cleared == 1 ? "" : "s");
            if (skipped > 0)
                std::printf(" (%d current failure%s skipped; use --force to clear anyway)",
                            skipped, skipped == 1 ? "" : "s");
            std::printf("\n");
        }
        return 0;
    }
    std::fprintf(stderr, "usage: daemon jobs list|history|failures|stats|add|show|run|enable|disable|remove|repair|clear-failures\n");
    return 2;
}

void update_job_by_id(const QString& profile, const QString& id, const std::function<void(QJsonObject&)>& fn) {
    QJsonObject doc = load_jobs_doc(profile);
    QJsonArray jobs = doc.value("jobs").toArray();
    for (int i = 0; i < jobs.size(); ++i) {
        QJsonObject job = jobs.at(i).toObject();
        if (job.value("id").toString() != id)
            continue;
        fn(job);
        job["updated_at"] = now_utc();
        jobs.replace(i, job);
        jobs_save_update(profile, jobs);
        return;
    }
}

void launch_scheduled_job(const QString& profile, const QJsonObject& job) {
    const QString id = job.value("id").toString();
    const QString name = job.value("name").toString();
    const int timeout_sec = job_timeout_sec(job);
    const QString run_id = record_job_run_start(profile, job, QStringLiteral("scheduled"));
    update_job_by_id(profile, id, [run_id](QJsonObject& j) {
        j["running"] = true;
        j["last_status"] = QStringLiteral("running");
        j["last_started_at"] = now_utc();
        j["last_error"] = QString();
        j["current_run_id"] = run_id;
    });
    append_job_log(profile, QStringLiteral("scheduled-start id=%1 name=\"%2\" timeout=%3s")
                                .arg(id, name)
                                .arg(timeout_sec));

    auto* p = new QProcess(qApp);
    auto* timeout = new QTimer(p);
    timeout->setSingleShot(true);
    QStringList args{QStringLiteral("--profile"), profile};
    args << json_array_to_strings(job.value("command").toArray());
    p->setProgram(QCoreApplication::applicationFilePath());
    p->setArguments(args);
    p->setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(timeout, &QTimer::timeout, p, [profile, id, p, timeout_sec]() {
        if (p->state() == QProcess::NotRunning)
            return;
        p->setProperty("daemon_timeout", true);
        append_job_log(profile, QStringLiteral("scheduled-timeout id=%1 timeout=%2s").arg(id).arg(timeout_sec));
        p->kill();
    });
    QObject::connect(p, &QProcess::errorOccurred, qApp, [profile, id, p, run_id](QProcess::ProcessError) {
        p->setProperty("daemon_history_recorded", true);
        update_job_by_id(profile, id, [](QJsonObject& j) {
            j["running"] = false;
            j["last_status"] = QStringLiteral("failed");
            j["last_error"] = QStringLiteral("process start error");
            j["fail_count"] = j.value("fail_count").toInt() + 1;
            j["current_run_id"] = QString();
        });
        record_job_run_finish(profile, run_id, QStringLiteral("failed"), -1, {}, {},
                              QStringLiteral("process start error"));
        append_job_log(profile, QStringLiteral("scheduled-error id=%1").arg(id));
        p->deleteLater();
    });
    QObject::connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     qApp, [profile, id, p, run_id](int exit_code, QProcess::ExitStatus) {
        const QString out = QString::fromUtf8(p->readAllStandardOutput());
        const QString err = QString::fromUtf8(p->readAllStandardError());
        const bool timed_out = p->property("daemon_timeout").toBool();
        const bool history_recorded = p->property("daemon_history_recorded").toBool();
        update_job_by_id(profile, id, [exit_code, out, err, timed_out](QJsonObject& j) {
            const int every = j.value("interval_sec").toInt(0);
            j["running"] = false;
            j["last_run_at"] = now_utc();
            j["last_exit_code"] = timed_out ? -1 : exit_code;
            j["last_status"] = timed_out ? QStringLiteral("timeout")
                                          : (exit_code == 0 ? QStringLiteral("ok") : QStringLiteral("failed"));
            if (timed_out)
                j["last_error"] = QStringLiteral("job exceeded timeout_sec");
            j["last_output_tail"] = compact_tail(out + "\n" + err);
            j["run_count"] = j.value("run_count").toInt() + 1;
            if (timed_out || exit_code != 0)
                j["fail_count"] = j.value("fail_count").toInt() + 1;
            if (every > 0)
                j["next_run_at"] = QDateTime::currentDateTimeUtc().addSecs(every).toString(Qt::ISODateWithMs);
            j["current_run_id"] = QString();
        });
        const QString status = timed_out ? QStringLiteral("timeout")
                                         : (exit_code == 0 ? QStringLiteral("ok") : QStringLiteral("failed"));
        if (!history_recorded) {
            record_job_run_finish(profile, run_id, status, timed_out ? -1 : exit_code, out, err,
                                  timed_out ? QStringLiteral("job exceeded timeout_sec")
                                            : (exit_code == 0 ? QString() : compact_tail(err)));
        }
        append_job_log(profile, timed_out
                                    ? QStringLiteral("scheduled-finish id=%1 status=timeout").arg(id)
                                    : QStringLiteral("scheduled-finish id=%1 exit=%2").arg(id).arg(exit_code));
        p->deleteLater();
    });
    p->start();
    timeout->start(timeout_sec * 1000);
}

void scan_daemon_jobs(const QString& profile) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    reconcile_stale_running_jobs(profile, now);
    const QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
    for (const auto& v : jobs) {
        const QJsonObject job = v.toObject();
        if (!job.value("enabled").toBool() || job.value("running").toBool())
            continue;
        if (job.value("schedule").toString() != QStringLiteral("interval"))
            continue;
        QDateTime due = parse_utc(job.value("next_run_at").toString());
        if (!due.isValid())
            due = now;
        if (due <= now)
            launch_scheduled_job(profile, job);
    }
}

void start_daemon_job_scheduler(const QString& profile) {
    QDir().mkpath(daemon_state_dir(profile));
    QDir().mkpath(daemon_logs_dir(profile));
    append_job_log(profile, QStringLiteral("scheduler-start profile=%1").arg(profile));
    auto* timer = new QTimer(qApp);
    timer->setInterval(5000);
    QObject::connect(timer, &QTimer::timeout, qApp, [profile]() { scan_daemon_jobs(profile); });
    timer->start();
    QTimer::singleShot(0, qApp, [profile]() { scan_daemon_jobs(profile); });
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp,
                     [profile]() { append_job_log(profile, QStringLiteral("scheduler-stop profile=%1").arg(profile)); });
}

int daemon_monitors_command(const QString& profile, bool json, QStringList args) {
    const QString sub = args.isEmpty() ? QStringLiteral("status") : args.takeFirst().trimmed().toLower();
    if (sub == "status" || sub == "list") {
        QJsonObject out{{"profile", profile}, {"jobs", jobs_summary(profile)}, {"health", daemon_health_object(profile)}};
        if (json) {
            std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        } else {
            const QJsonObject j = out.value("jobs").toObject();
            std::printf("monitors/jobs  total=%d enabled=%d running=%d failed=%d stale=%d history=%d interval=%d\n",
                        j.value("total").toInt(), j.value("enabled").toInt(), j.value("running").toInt(),
                        j.value("failed").toInt(), j.value("stale").toInt(),
                        j.value("failed_history").toInt(), j.value("interval").toInt());
        }
        return 0;
    }
    if (sub == "repair" || sub == "restart") {
        return daemon_jobs_command(profile, json, {QStringLiteral("repair")});
    }
    std::fprintf(stderr, "usage: daemon monitors status|repair\n");
    return 2;
}

int daemon_add_template_job(const QString& profile, bool json, const QString& kind, QStringList args) {
    QString name;
    int every_sec = 0;
    int timeout_sec = 0;
    bool enabled = true;
    QJsonObject spec = parse_job_spec(kind, args, &name, &every_sec, &timeout_sec, &enabled);
    if (spec.isEmpty() && kind != "health-check")
        return 2;
    const QString normalized_kind = kind == "paper" ? QStringLiteral("paper-strategy") : kind;
    QJsonObject job = make_job(normalized_kind, name, spec, every_sec, timeout_sec, enabled);
    QJsonObject doc = load_jobs_doc(profile);
    QJsonArray jobs = doc.value("jobs").toArray();
    jobs.append(job);
    const int rc = jobs_save_update(profile, jobs);
    if (rc != 0) return rc;
    if (json)
        std::printf("%s\n", QJsonDocument(QJsonObject{{"job", job}}).toJson(QJsonDocument::Compact).constData());
    else
        std::printf("added %s  %s\n", qUtf8Printable(job.value("id").toString()),
                    qUtf8Printable(job.value("name").toString()));
    return 0;
}

int daemon_notify_command(const QString& profile, bool json, QStringList args) {
    const bool as_job = args.removeAll(QStringLiteral("--job")) > 0;
    if (as_job)
        return daemon_add_template_job(profile, json, QStringLiteral("notify"), args);
    QString name;
    int every_sec = 0;
    int timeout_sec = 0;
    bool enabled = true;
    QJsonObject spec = parse_job_spec(QStringLiteral("notify"), args, &name, &every_sec, &timeout_sec, &enabled);
    if (spec.isEmpty())
        return 2;
    QJsonObject job = make_job(QStringLiteral("notify"), name, spec, 0, timeout_sec, true);
    const ProcessResult r = run_job_once_sync(profile, job, 30000);
    if (json)
        std::printf("%s\n", QJsonDocument(QJsonObject{{"exit_code", r.exit_code},
                                                      {"stdout", r.out},
                                                      {"stderr", r.err}})
                                .toJson(QJsonDocument::Compact)
                                .constData());
    else {
        std::printf("%s", qUtf8Printable(r.out));
        if (!r.err.isEmpty()) std::fprintf(stderr, "%s", qUtf8Printable(r.err));
    }
    return r.exit_code == 0 ? 0 : 5;
}

int daemon_command(const QString& profile, bool json, QStringList args) {
    const QString sub = args.isEmpty() ? QStringLiteral("status") : args.takeFirst().trimmed().toLower();
    [[maybe_unused]] auto has_flag = [&](const QString& flag) { return args.removeAll(flag) > 0; };

    if (sub == "status" || sub == "check")
        return emit_daemon_status(profile, json);
    if (sub == "health")
        return emit_daemon_health(profile, json);
    if (sub == "readiness" || sub == "ready" || sub == "safety" || sub == "trade-gate")
        return emit_daemon_readiness(profile, json, args);
    if (sub == "logs" || sub == "log")
        return emit_daemon_logs(profile, json, args);
    if (sub == "audit" || sub == "security")
        return emit_daemon_audit(profile, json);
    if (sub == "jobs" || sub == "job")
        return daemon_jobs_command(profile, json, args);
    if (sub == "monitors" || sub == "monitor")
        return daemon_monitors_command(profile, json, args);
    if (sub == "notify" || sub == "notification")
        return daemon_notify_command(profile, json, args);
    if (sub == "ai") {
        if (args.isEmpty()) {
            std::fprintf(stderr, "usage: daemon ai <brief|risk|thesis|radar> <target> [--every-sec N]\n");
            return 2;
        }
        args.prepend(QStringLiteral("ai"));
        return daemon_jobs_command(profile, json, QStringList{QStringLiteral("add")} + args);
    }
    if (sub == "paper" || sub == "paper-strategy") {
        args.prepend(QStringLiteral("paper-strategy"));
        return daemon_jobs_command(profile, json, QStringList{QStringLiteral("add")} + args);
    }

    if (sub == "install") {
#if !defined(Q_OS_MACOS)
        std::fprintf(stderr, "daemon install is currently supported on macOS launchd only\n");
        return 2;
#else
        const bool replace = has_flag(QStringLiteral("--replace")) || has_flag(QStringLiteral("--force"));
        const bool start = has_flag(QStringLiteral("--start"));
        const bool dry_run = has_flag(QStringLiteral("--dry-run"));
        if (!args.isEmpty()) {
            std::fprintf(stderr, "usage: daemon install [--replace] [--start] [--dry-run]\n");
            return 2;
        }
        const QString path = daemon_plist_path(profile);
        if (QFileInfo::exists(path) && !replace && !dry_run) {
            std::fprintf(stderr, "daemon already installed for profile '%s'; use --replace\n",
                         qUtf8Printable(profile));
            return 2;
        }
        if (!dry_run) {
            QString error;
            if (!write_daemon_plist(profile, &error)) {
                std::fprintf(stderr, "%s\n", qUtf8Printable(error));
                return 7;
            }
        }
        if (json) {
            QJsonObject o = daemon_status_object(profile);
            o["installed"] = !dry_run || QFileInfo::exists(path);
            o["dry_run"] = dry_run;
            o["program"] = QCoreApplication::applicationFilePath();
            std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
        } else {
            std::printf("%s %s\n", dry_run ? "would install" : "installed", qUtf8Printable(path));
        }
        if (start && !dry_run)
            return daemon_start_impl(profile, json);
        return 0;
#endif
    }

    if (sub == "uninstall" || sub == "remove" || sub == "rm") {
#if !defined(Q_OS_MACOS)
        std::fprintf(stderr, "daemon uninstall is currently supported on macOS launchd only\n");
        return 2;
#else
        const bool dry_run = has_flag(QStringLiteral("--dry-run"));
        if (!args.isEmpty()) {
            std::fprintf(stderr, "usage: daemon uninstall [--dry-run]\n");
            return 2;
        }
        const QString path = daemon_plist_path(profile);
        const bool was_installed = QFileInfo::exists(path);
        bool stopped = false;
        if (!dry_run && (launchd_loaded(profile) || daemon_status_object(profile).value("running").toBool())) {
            const int rc = daemon_stop_impl(profile, false, true);
            if (rc != 0 && rc != 3)
                return rc;
            stopped = true;
        }
        if (!dry_run && was_installed && !QFile::remove(path)) {
            std::fprintf(stderr, "failed to remove %s\n", qUtf8Printable(path));
            return 7;
        }
        if (json) {
            QJsonObject o{{"profile", profile},
                          {"label", daemon_label(profile)},
                          {"plist", path},
                          {"removed", was_installed && !dry_run},
                          {"would_remove", dry_run && was_installed},
                          {"stopped", stopped},
                          {"dry_run", dry_run}};
            std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
        } else if (dry_run) {
            std::printf("%s %s\n", was_installed ? "would remove" : "not installed", qUtf8Printable(path));
        } else if (was_installed) {
            std::printf("removed %s\n", qUtf8Printable(path));
        } else {
            std::fprintf(stderr, "daemon is not installed for profile '%s'\n", qUtf8Printable(profile));
            return 3;
        }
        return 0;
#endif
    }

    if (sub == "start")
        return daemon_start_impl(profile, json);
    if (sub == "stop")
        return daemon_stop_impl(profile, json);
    if (sub == "restart") {
        const int stop_rc = daemon_stop_impl(profile, false, true);
        if (stop_rc != 0 && stop_rc != 3)
            return stop_rc;
        for (int i = 0; i < 40; ++i) {
            const bool loaded = launchd_loaded(profile);
            const bool running = daemon_status_object(profile).value("running").toBool();
            if (!loaded && !running)
                break;
            QThread::msleep(250);
        }
        QThread::msleep(1000);
        return daemon_start_impl(profile, json);
    }
    if (sub == "plist" || sub == "path") {
        const QString path = daemon_plist_path(profile);
        if (json)
            std::printf("%s\n", QJsonDocument(QJsonObject{{"profile", profile},
                                                          {"label", daemon_label(profile)},
                                                          {"plist", path}})
                                    .toJson(QJsonDocument::Compact)
                                    .constData());
        else
            std::printf("%s\n", qUtf8Printable(path));
        return 0;
    }

    std::fprintf(stderr,
                 "usage: daemon status|health|readiness|logs|audit|jobs|monitors|notify|ai|paper|"
                 "install|uninstall|start|stop|restart|plist\n");
    return 2;
}

} // namespace openmarketterminal::cli
