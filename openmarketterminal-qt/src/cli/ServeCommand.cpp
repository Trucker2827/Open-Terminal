#include "cli/ServeCommand.h"
#include "cli/BridgeDiscoveryFile.h"
#include "core/headless/HeadlessRuntime.h"
#include "mcp/TerminalMcpBridge.h"
#include "mcp/McpProvider.h"
#include "mcp/McpTypes.h"
#include "mcp/tools/SettingsGate.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSocketNotifier>
#include <QThread>
#include <QJsonObject>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QTextStream>
#include <QTimer>
#include <QUuid>
#include <QXmlStreamWriter>
#include <cstdio>
#include <csignal>
#include <functional>
#include <optional>
#ifndef _WIN32
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

QString launch_domain() {
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

QJsonObject make_job(const QString& kind, const QString& name, const QJsonObject& spec, int every_sec,
                     bool enabled = true) {
    QStringList command = command_for_job_kind(kind, spec);
    const QString id = QStringLiteral("job_%1").arg(QUuid::createUuid().toString(QUuid::Id128).left(12));
    const QString created = now_utc();
    return QJsonObject{{"id", id},
                       {"name", name.trimmed().isEmpty() ? kind + QStringLiteral(" ") + id.right(4) : name.trimmed()},
                       {"kind", kind},
                       {"enabled", enabled},
                       {"schedule", every_sec > 0 ? QStringLiteral("interval") : QStringLiteral("manual")},
                       {"interval_sec", every_sec},
                       {"next_run_at", every_sec > 0 ? created : QString()},
                       {"running", false},
                       {"run_count", 0},
                       {"fail_count", 0},
                       {"created_at", created},
                       {"updated_at", created},
                       {"spec", spec},
                       {"command", strings_to_json_array(command)}};
}

ProcessResult run_job_once_sync(const QString& profile, const QJsonObject& job, int timeout_ms = 120000) {
    QStringList args{QStringLiteral("--profile"), profile};
    args << json_array_to_strings(job.value("command").toArray());
    return run_process(QCoreApplication::applicationFilePath(), args, timeout_ms);
}

QJsonObject jobs_summary(const QString& profile) {
    const QJsonArray jobs = load_jobs_doc(profile).value("jobs").toArray();
    int enabled = 0, running = 0, failed = 0, interval = 0;
    QJsonArray by_kind;
    QJsonObject counts;
    for (const auto& v : jobs) {
        const QJsonObject j = v.toObject();
        if (j.value("enabled").toBool()) ++enabled;
        if (j.value("running").toBool()) ++running;
        if (j.value("fail_count").toInt() > 0) ++failed;
        if (j.value("schedule").toString() == QStringLiteral("interval")) ++interval;
        const QString kind = j.value("kind").toString("unknown");
        counts[kind] = counts.value(kind).toInt() + 1;
    }
    for (auto it = counts.begin(); it != counts.end(); ++it)
        by_kind.append(QJsonObject{{"kind", it.key()}, {"count", it.value().toInt()}});
    return QJsonObject{{"total", jobs.size()},
                       {"enabled", enabled},
                       {"running", running},
                       {"failed", failed},
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

bool write_daemon_plist(const QString& profile, QString* error) {
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
    o["capabilities"] = QJsonArray{"health", "logs", "jobs", "monitors", "notify", "paper-strategy",
                                   "ai-automation", "audit"};
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
    std::printf("jobs         total=%d enabled=%d running=%d failed=%d interval=%d\n",
                j.value("total").toInt(), j.value("enabled").toInt(), j.value("running").toInt(),
                j.value("failed").toInt(), j.value("interval").toInt());
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
    }
    const ProcessResult kick = run_process(QStringLiteral("launchctl"),
                                           {QStringLiteral("kickstart"), QStringLiteral("-k"),
                                            launch_domain() + "/" + daemon_label(profile)},
                                           10000);
    if (kick.exit_code != 0) {
        std::fprintf(stderr, "launchctl kickstart failed: %s\n", qUtf8Printable(kick.err.trimmed()));
        return 7;
    }
    if (json) {
        QJsonObject o = daemon_status_object(profile);
        o["started"] = true;
        std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
    } else {
        std::printf("daemon started for profile '%s'\n", qUtf8Printable(profile));
    }
    return 0;
#endif
}

int daemon_stop_impl(const QString& profile, bool json, bool quiet = false) {
#if !defined(Q_OS_MACOS)
    Q_UNUSED(json);
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
#ifndef _WIN32
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
    std::printf("%-16s %-22s %-15s %-8s %-8s %s\n", "id", "name", "kind", "enabled", "last", "command");
    for (const auto& v : jobs) {
        const QJsonObject j = v.toObject();
        std::printf("%-16s %-22s %-15s %-8s %-8s %s\n",
                    qUtf8Printable(j.value("id").toString()),
                    qUtf8Printable(j.value("name").toString().left(22)),
                    qUtf8Printable(j.value("kind").toString()),
                    j.value("enabled").toBool() ? "yes" : "no",
                    qUtf8Printable(j.value("last_status").toString("-")),
                    qUtf8Printable(json_array_to_strings(j.value("command").toArray()).join(' ')));
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

QJsonObject parse_job_spec(QString kind, QStringList args, QString* name, int* every_sec, bool* enabled) {
    kind = kind.trimmed().toLower();
    QJsonObject spec;
    *name = {};
    *every_sec = 0;
    *enabled = true;

    for (int i = 0; i < args.size(); ++i) {
        const QString flag = args.at(i);
        if (flag == "--name") {
            if (!consume_value(args, i, flag, name)) return {};
        } else if (flag == "--every-sec" || flag == "--interval-sec") {
            if (!consume_int(args, i, flag, every_sec)) return {};
        } else if (flag == "--disabled") {
            args.removeAt(i--);
            *enabled = false;
        }
    }

    if (kind == "command") {
        const int sep = args.indexOf(QStringLiteral("--"));
        QStringList command = sep >= 0 ? args.mid(sep + 1) : args;
        if (command.isEmpty()) {
            std::fprintf(stderr, "usage: daemon jobs add command [--name N] [--every-sec N] -- <cli args...>\n");
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
    if (sub == "add" || sub == "create") {
        if (args.isEmpty()) {
            std::fprintf(stderr, "usage: daemon jobs add <command|brief|ai|notebook|paper-strategy|notify|health-check> ...\n");
            return 2;
        }
        QString kind = args.takeFirst().trimmed().toLower();
        QString name;
        int every_sec = 0;
        bool enabled = true;
        QJsonObject spec = parse_job_spec(kind, args, &name, &every_sec, &enabled);
        if (spec.isEmpty() && kind != "health-check")
            return 2;
        if (kind == "paper") kind = QStringLiteral("paper-strategy");
        QJsonObject doc = load_jobs_doc(profile);
        QJsonArray jobs = doc.value("jobs").toArray();
        QJsonObject job = make_job(kind, name, spec, every_sec, enabled);
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
        append_job_log(profile, QStringLiteral("manual-start id=%1 name=\"%2\"").arg(job.value("id").toString(), job.value("name").toString()));
        const ProcessResult r = run_job_once_sync(profile, job);
        job["last_run_at"] = now_utc();
        job["last_exit_code"] = r.exit_code;
        job["last_status"] = r.exit_code == 0 ? QStringLiteral("ok") : QStringLiteral("failed");
        job["last_output_tail"] = compact_tail(r.out + "\n" + r.err);
        job["run_count"] = job.value("run_count").toInt() + 1;
        if (r.exit_code != 0) job["fail_count"] = job.value("fail_count").toInt() + 1;
        jobs.replace(idx, job);
        jobs_save_update(profile, jobs);
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
    std::fprintf(stderr, "usage: daemon jobs list|add|show|run|enable|disable|remove|repair\n");
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
    update_job_by_id(profile, id, [](QJsonObject& j) {
        j["running"] = true;
        j["last_status"] = QStringLiteral("running");
        j["last_started_at"] = now_utc();
    });
    append_job_log(profile, QStringLiteral("scheduled-start id=%1 name=\"%2\"").arg(id, name));

    auto* p = new QProcess(qApp);
    QStringList args{QStringLiteral("--profile"), profile};
    args << json_array_to_strings(job.value("command").toArray());
    p->setProgram(QCoreApplication::applicationFilePath());
    p->setArguments(args);
    p->setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(p, &QProcess::errorOccurred, qApp, [profile, id](QProcess::ProcessError) {
        update_job_by_id(profile, id, [](QJsonObject& j) {
            j["running"] = false;
            j["last_status"] = QStringLiteral("failed");
            j["last_error"] = QStringLiteral("process start error");
            j["fail_count"] = j.value("fail_count").toInt() + 1;
        });
        append_job_log(profile, QStringLiteral("scheduled-error id=%1").arg(id));
    });
    QObject::connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     qApp, [profile, id, p](int exit_code, QProcess::ExitStatus) {
        const QString out = QString::fromUtf8(p->readAllStandardOutput());
        const QString err = QString::fromUtf8(p->readAllStandardError());
        update_job_by_id(profile, id, [exit_code, out, err](QJsonObject& j) {
            const int every = j.value("interval_sec").toInt(0);
            j["running"] = false;
            j["last_run_at"] = now_utc();
            j["last_exit_code"] = exit_code;
            j["last_status"] = exit_code == 0 ? QStringLiteral("ok") : QStringLiteral("failed");
            j["last_output_tail"] = compact_tail(out + "\n" + err);
            j["run_count"] = j.value("run_count").toInt() + 1;
            if (exit_code != 0)
                j["fail_count"] = j.value("fail_count").toInt() + 1;
            if (every > 0)
                j["next_run_at"] = QDateTime::currentDateTimeUtc().addSecs(every).toString(Qt::ISODateWithMs);
        });
        append_job_log(profile, QStringLiteral("scheduled-finish id=%1 exit=%2").arg(id).arg(exit_code));
        p->deleteLater();
    });
    p->start();
}

void scan_daemon_jobs(const QString& profile) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
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
            std::printf("monitors/jobs  total=%d enabled=%d running=%d failed=%d interval=%d\n",
                        j.value("total").toInt(), j.value("enabled").toInt(), j.value("running").toInt(),
                        j.value("failed").toInt(), j.value("interval").toInt());
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
    bool enabled = true;
    QJsonObject spec = parse_job_spec(kind, args, &name, &every_sec, &enabled);
    if (spec.isEmpty() && kind != "health-check")
        return 2;
    const QString normalized_kind = kind == "paper" ? QStringLiteral("paper-strategy") : kind;
    QJsonObject job = make_job(normalized_kind, name, spec, every_sec, enabled);
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
    bool enabled = true;
    QJsonObject spec = parse_job_spec(QStringLiteral("notify"), args, &name, &every_sec, &enabled);
    if (spec.isEmpty())
        return 2;
    QJsonObject job = make_job(QStringLiteral("notify"), name, spec, 0, true);
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
    auto has_flag = [&](const QString& flag) { return args.removeAll(flag) > 0; };

    if (sub == "status" || sub == "check")
        return emit_daemon_status(profile, json);
    if (sub == "health")
        return emit_daemon_health(profile, json);
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
                 "usage: daemon status|health|logs|audit|jobs|monitors|notify|ai|paper|"
                 "install|uninstall|start|stop|restart|plist\n");
    return 2;
}

} // namespace openmarketterminal::cli
