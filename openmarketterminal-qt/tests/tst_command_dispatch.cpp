#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <functional>
#include <unistd.h>
#include "cli/CommandDispatch.h"
#include "cli/BridgeDiscovery.h"
#include "cli/automation/AutomationState.h"
using namespace openmarketterminal::cli;

static QString capture_stdout(const std::function<int()>& fn, int* rc_out = nullptr) {
    fflush(stdout);
    int fds[2];
    if (pipe(fds) != 0)
        return {};
    const int saved = dup(STDOUT_FILENO);
    dup2(fds[1], STDOUT_FILENO);
    close(fds[1]);

    const int rc = fn();
    fflush(stdout);

    dup2(saved, STDOUT_FILENO);
    close(saved);

    QByteArray out;
    char buf[4096];
    ssize_t n = 0;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<int>(n));
    close(fds[0]);
    if (rc_out)
        *rc_out = rc;
    return QString::fromUtf8(out);
}

static QJsonArray coverage_rows_for(const QString& query, int* rc_out = nullptr) {
    int rc = -1;
    const QString out = capture_stdout([&]() {
        return dispatch(QStringList{"--json", "--headless", "coverage", query});
    }, &rc);
    if (rc_out)
        *rc_out = rc;
    if (rc != 0)
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8());
    if (!doc.isObject())
        return {};
    return doc.object().value("coverage").toArray();
}

static bool has_screen(const QJsonArray& rows, const QString& screen_id) {
    for (const auto& v : rows) {
        if (v.toObject().value("screen_id").toString() == screen_id)
            return true;
    }
    return false;
}

static QJsonObject json_object_from_dispatch(const QStringList& args, int* rc_out = nullptr) {
    int rc = -1;
    const QString out = capture_stdout([&]() { return dispatch(args); }, &rc);
    if (rc_out)
        *rc_out = rc;
    const QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

static QStringList json_strings(const QJsonArray& arr) {
    QStringList out;
    for (const QJsonValue& v : arr)
        out << v.toString();
    return out;
}

class TstCommandDispatch : public QObject {
    Q_OBJECT
private slots:
    void strips_global_flags() {
        QStringList args{"--json", "--headless", "--profile", "alice", "mcp", "list"};
        GlobalOpts o;
        QVERIFY(parse_global_opts(args, o));
        QVERIFY(o.json);
        QVERIFY(o.headless);
        QCOMPARE(o.profile, QString("alice"));
        QCOMPARE(args, (QStringList{"mcp", "list"}));
    }
    void parses_help_flag() {
        QStringList args{"--help"};
        GlobalOpts o;
        QVERIFY(parse_global_opts(args, o));
        QVERIFY(o.help);
        QVERIFY(args.isEmpty());
    }
    void defaults_when_absent() {
        QStringList args{"status"};
        GlobalOpts o;
        QVERIFY(parse_global_opts(args, o));
        QVERIFY(!o.json);
        QCOMPARE(o.profile, QString("default"));
        QCOMPARE(args, (QStringList{"status"}));
    }
    void rejects_dangling_profile() {
        QStringList args{"--profile"};
        GlobalOpts o;
        QVERIFY(!parse_global_opts(args, o));
    }
    void resolve_missing_is_not_running() {
        QTemporaryDir home; qputenv("HOME", home.path().toUtf8());
        auto r = resolve("default");
        QVERIFY(std::holds_alternative<DiscoveryError>(r));
        QCOMPARE(std::get<DiscoveryError>(r), DiscoveryError::NotRunning);
    }
    void resolve_live_pid_succeeds() {
        QTemporaryDir home; qputenv("HOME", home.path().toUtf8());
        write_bridge_file(profile_root_for("default"),
            {"http://127.0.0.1:9", "tok", QCoreApplication::applicationPid(), "x", "gui"});
        auto r = resolve("default");
        QVERIFY(std::holds_alternative<Discovered>(r));
    }
    void scanner_help_is_routed() {
        QCOMPARE(dispatch(QStringList{"help", "scanner"}), 0);
        QCOMPARE(dispatch(QStringList{"--help", "scanner"}), 0);
    }
    void coverage_help_is_routed() {
        QCOMPARE(dispatch(QStringList{"help", "coverage"}), 0);
        QCOMPARE(dispatch(QStringList{"--help", "coverage"}), 0);
    }
    void important_help_routes_are_routed() {
        const QList<QString> topics = {
            "doctor", "setup", "research", "macro", "trade", "data", "files", "notes", "report", "notify",
            "excel", "workspace", "scanner", "watchlist", "portfolio", "profile", "settings", "security", "ai",
            "notebook", "strategy", "serve", "daemon",
        };
        for (const QString& topic : topics) {
            QCOMPARE(dispatch(QStringList{"help", topic}), 0);
            QCOMPARE(dispatch(QStringList{"--help", topic}), 0);
        }
    }
    void macro_help_advertises_direct_aliases() {
        int rc = -1;
        const QString out = capture_stdout([&]() { return dispatch(QStringList{"help", "macro"}); }, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(out.contains("macro fred <series-id>"));
        QVERIFY(out.contains("macro bls series <series-id>"));
        QVERIFY(out.contains("macro calendar [YYYY-MM-DD]"));
        QVERIFY(out.contains("macro cb <boe|rba|boc|riksbank|snb|norges>"));
        QVERIFY(out.contains("macro dbnomics providers"));
        QVERIFY(out.contains("macro dbnomics observations"));
        QVERIFY(out.contains("macro gov providers"));
        QVERIFY(out.contains("macro gov congress-bills"));
        QVERIFY(out.contains("macro econ-run <source>"));
        QVERIFY(out.contains("macro tool <mcp-tool>"));
    }
    void research_help_advertises_ownership_aliases() {
        int rc = -1;
        const QString out = capture_stdout([&]() { return dispatch(QStringList{"help", "research"}); }, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(out.contains("research insiders <ticker>"));
        QVERIFY(out.contains("research 13f-top <ticker>"));
        QVERIFY(out.contains("research politicians <ticker>"));
    }
    void spreadsheet_help_advertises_file_commands() {
        int rc = -1;
        const QString out = capture_stdout([&]() { return dispatch(QStringList{"help", "excel"}); }, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(out.contains("excel read <path>"));
        QVERIFY(out.contains("excel write <path>"));
        QVERIFY(out.contains("excel append <path>"));
        QVERIFY(out.contains("excel google-read <spreadsheet-id>"));
    }
    void workspace_help_advertises_layout_and_panel_commands() {
        int rc = -1;
        const QString out = capture_stdout([&]() { return dispatch(QStringList{"help", "workspace"}); }, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(out.contains("workspace layouts"));
        QVERIFY(out.contains("workspace export <layout-id-or-name>"));
        QVERIFY(out.contains("workspace panels"));
        QVERIFY(out.contains("workspace open <screen-or-alias>"));
    }
    void ai_help_advertises_recipes() {
        int rc = -1;
        const QString out = capture_stdout([&]() { return dispatch(QStringList{"help", "ai"}); }, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(out.contains("ai recipes"));
        QVERIFY(out.contains("ai recipe show <name>"));
        QVERIFY(out.contains("ai recipe run <name> <target...>"));
    }
    void daemon_plist_is_profile_scoped() {
        QTemporaryDir home; qputenv("HOME", home.path().toUtf8());
        int rc = -1;
        const QString out = capture_stdout([&]() {
            return dispatch(QStringList{"--profile", "desk one", "daemon", "plist"});
        }, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(out.contains("Library/LaunchAgents/org.openterminal.cli.daemon.desk-one.plist"));
    }
    void daemon_jobs_are_profile_scoped() {
        QTemporaryDir home; qputenv("HOME", home.path().toUtf8());
        QCOMPARE(dispatch(QStringList{"--profile", "desk one", "daemon", "jobs", "add", "health-check",
                                      "--every-sec", "60"}), 0);
        int rc = -1;
        const QString out = capture_stdout([&]() {
            return dispatch(QStringList{"--json", "--profile", "desk one", "daemon", "jobs", "list"});
        }, &rc);
        QCOMPARE(rc, 0);
        const QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8());
        QVERIFY(doc.isObject());
        const QJsonArray jobs = doc.object().value("jobs").toArray();
        QCOMPARE(jobs.size(), 1);
        QCOMPARE(jobs.first().toObject().value("kind").toString(), QString("health-check"));
        QCOMPARE(jobs.first().toObject().value("interval_sec").toInt(), 60);
    }
    void daemon_health_audit_and_logs_have_ui_contract() {
        QTemporaryDir home; qputenv("HOME", home.path().toUtf8());
        int rc = -1;
        const QJsonObject health = json_object_from_dispatch(
            QStringList{"--json", "--profile", "desk", "daemon", "health"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(health.value("profile").toString(), QString("desk"));
        QVERIFY(health.value("jobs").isObject());
        QVERIFY(health.value("logs").toObject().value("jobs").toString().contains("daemon.jobs.log"));
        QVERIFY(json_strings(health.value("capabilities").toArray()).contains("jobs"));
        QVERIFY(json_strings(health.value("capabilities").toArray()).contains("audit"));

        const QJsonObject audit = json_object_from_dispatch(
            QStringList{"--json", "--profile", "desk", "daemon", "audit"}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(!audit.value("api_keys_leave_machine").toBool());
        QVERIFY(!audit.value("unattended_live_trading").toBool());
        QVERIFY(!audit.value("daemon_bridge_destructive_tools").toBool());
        QVERIFY(!audit.value("daemon_settings_write_tools").toBool());
        QVERIFY(json_strings(audit.value("allowed_job_kinds").toArray()).contains("notify"));
        QVERIFY(json_strings(audit.value("guardrails").toArray()).contains("secrets are not written to daemon job specs"));

        const QJsonObject logs = json_object_from_dispatch(
            QStringList{"--json", "--profile", "desk", "daemon", "logs", "jobs", "--lines", "5"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(logs.value("profile").toString(), QString("desk"));
        QCOMPARE(logs.value("channel").toString(), QString("jobs"));
        QCOMPARE(logs.value("lines").toInt(), 5);
        QVERIFY(logs.value("path").toString().contains("daemon.jobs.log"));
        QVERIFY(logs.contains("text"));
    }
    void daemon_job_presets_generate_expected_commands() {
        QTemporaryDir home; qputenv("HOME", home.path().toUtf8());
        const QString profile = QStringLiteral("preset");
        QCOMPARE(dispatch(QStringList{"--profile", profile, "daemon", "ai", "brief", "SPY", "--every-sec", "86400"}), 0);
        QCOMPARE(dispatch(QStringList{"--profile", profile, "daemon", "ai", "radar", "AI semiconductors", "--every-sec", "3600"}), 0);
        QCOMPARE(dispatch(QStringList{"--profile", profile, "daemon", "jobs", "add", "notebook",
                                      "trading-sma-crossover-backtest", "--every-sec", "604800"}), 0);
        QCOMPARE(dispatch(QStringList{"--profile", profile, "daemon", "paper", "meanrev", "--every-sec", "300"}), 0);
        QCOMPARE(dispatch(QStringList{"--profile", profile, "daemon", "jobs", "add", "health-check",
                                      "--every-sec", "300"}), 0);
        QCOMPARE(dispatch(QStringList{"--profile", profile, "daemon", "notify", "--job", "--title", "OpenTerminal",
                                      "--message", "Daemon is alive", "--every-sec", "3600"}), 0);

        int rc = -1;
        const QJsonObject listed = json_object_from_dispatch(
            QStringList{"--json", "--profile", profile, "daemon", "jobs", "list"}, &rc);
        QCOMPARE(rc, 0);
        const QJsonArray jobs = listed.value("jobs").toArray();
        QCOMPARE(jobs.size(), 6);
        auto command_at = [&](int i) { return json_strings(jobs.at(i).toObject().value("command").toArray()); };
        QCOMPARE(jobs.at(0).toObject().value("kind").toString(), QString("ai"));
        QCOMPARE(command_at(0), (QStringList{"brief", "SPY"}));
        QCOMPARE(command_at(1), (QStringList{"radar", "AI semiconductors"}));
        QCOMPARE(command_at(2), (QStringList{"notebook", "run", "trading-sma-crossover-backtest"}));
        QCOMPARE(command_at(3), (QStringList{"strategy", "paper-run", "meanrev", "--max-iters", "1", "--interval-sec", "0"}));
        QCOMPARE(command_at(4), (QStringList{"daemon", "health"}));
        QCOMPARE(command_at(5), (QStringList{"notify", "send", "--all", "--title", "OpenTerminal",
                                             "--message", "Daemon is alive", "--level", "info", "--yes"}));
    }
    void daemon_job_detail_and_actions_are_ui_ready() {
        QTemporaryDir home; qputenv("HOME", home.path().toUtf8());
        const QString profile = QStringLiteral("detail");
        QCOMPARE(dispatch(QStringList{"--profile", profile, "daemon", "jobs", "add", "command", "--name", "Version",
                                      "--", "version"}), 0);
        int rc = -1;
        QJsonObject job = json_object_from_dispatch(
            QStringList{"--json", "--profile", profile, "daemon", "jobs", "show", "Version"}, &rc);
        QCOMPARE(rc, 0);
        const QString id = job.value("id").toString();
        QVERIFY(id.startsWith("job_"));
        QCOMPARE(job.value("name").toString(), QString("Version"));
        QCOMPARE(job.value("kind").toString(), QString("command"));
        QVERIFY(job.value("enabled").toBool());
        QCOMPARE(json_strings(job.value("command").toArray()), (QStringList{"version"}));
        QCOMPARE(json_strings(job.value("spec").toObject().value("command").toArray()), (QStringList{"version"}));

        QCOMPARE(dispatch(QStringList{"--profile", profile, "daemon", "jobs", "disable", id}), 0);
        job = json_object_from_dispatch(QStringList{"--json", "--profile", profile, "daemon", "jobs", "show", id}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(!job.value("enabled").toBool());

        QCOMPARE(dispatch(QStringList{"--profile", profile, "daemon", "jobs", "enable", id}), 0);
        job = json_object_from_dispatch(QStringList{"--json", "--profile", profile, "daemon", "jobs", "show", id}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(job.value("enabled").toBool());

        QCOMPARE(dispatch(QStringList{"--profile", profile, "daemon", "jobs", "remove", id}), 0);
        const QJsonObject listed = json_object_from_dispatch(
            QStringList{"--json", "--profile", profile, "daemon", "jobs", "list"}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(listed.value("jobs").toArray().isEmpty());
    }
    void coverage_contract_for_ownership_and_macro() {
        int rc = -1;
        const QJsonArray ownership = coverage_rows_for("ownership", &rc);
        QCOMPARE(rc, 0);
        QVERIFY(!ownership.isEmpty());
        QVERIFY(has_screen(ownership, "insider_trades"));
        QVERIFY(has_screen(ownership, "institution_holdings"));
        QVERIFY(has_screen(ownership, "politician_trades"));

        const QJsonArray macro = coverage_rows_for("macro", &rc);
        QCOMPARE(rc, 0);
        QVERIFY(!macro.isEmpty());
        QVERIFY(has_screen(macro, "economics"));
        QVERIFY(has_screen(macro, "dbnomics"));
        QVERIFY(has_screen(macro, "gov_data"));

        const QJsonArray excel = coverage_rows_for("excel", &rc);
        QCOMPARE(rc, 0);
        QVERIFY(!excel.isEmpty());
        QVERIFY(has_screen(excel, "excel"));
        QVERIFY(excel.first().toObject().value("command").toString().contains("excel read|write"));
        QVERIFY(!excel.first().toObject().value("notes").toString().contains("pending"));
    }
    void automation_arm_respects_profile() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        int rc = -1;
        capture_stdout([&]() {
            rc = dispatch({QStringLiteral("--json"), QStringLiteral("--profile"), QStringLiteral("botlab"),
                           QStringLiteral("automation"), QStringLiteral("arm-bot"),
                           QStringLiteral("--max-order-usd"), QStringLiteral("50"),
                           QStringLiteral("--symbols"), QStringLiteral("BTC-USD"),
                           QStringLiteral("--yes"), QStringLiteral("--i-understand-live-risk")});
            return rc;
        });
        QCOMPARE(rc, 0);
        const QString botlab_guard = automation::live_guard_path(QStringLiteral("botlab"));
        const QString default_guard = automation::live_guard_path(QStringLiteral("default"));
        QVERIFY2(QFile::exists(botlab_guard), "guard must land in the armed profile");
        QVERIFY2(!QFile::exists(default_guard), "guard must NOT leak into the default profile");
    }
    void automation_dry_run_does_not_consume() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        using namespace openmarketterminal::cli::automation;
        const QJsonObject cand{{"symbol", "BTC-USD"},
                               {"verdict", "PAPER TRADE CANDIDATE"},
                               {"action", "PAPER_LIMIT_BUY_ONLY"},
                               {"reference_price", 60000.0},
                               {"ts_ms", QString::number(QDateTime::currentMSecsSinceEpoch())}};
        QString err;
        QVERIFY(append_jsonl(decisions_path("default"), cand, &err));
        int rc = -1;
        const QString out = capture_stdout([&]() {
            rc = dispatch({QStringLiteral("--json"), QStringLiteral("automation"),
                           QStringLiteral("execute-next"), QStringLiteral("--symbol"),
                           QStringLiteral("BTC-USD"), QStringLiteral("--dry-run")});
            return rc;
        });
        QCOMPARE(rc, 0);
        QVERIFY(out.contains(QStringLiteral("\"dry_run\":true")));
        QVERIFY2(!QFile::exists(consumed_path("default")), "dry-run must not consume");
        QVERIFY2(!QFile::exists(daily_orders_path("default")),
                 "dry-run must not create the daily live-order counter");
        // second dry-run still finds the candidate
        const QString out2 = capture_stdout([&]() {
            return dispatch({QStringLiteral("--json"), QStringLiteral("automation"),
                             QStringLiteral("execute-next"), QStringLiteral("--symbol"),
                             QStringLiteral("BTC-USD"), QStringLiteral("--dry-run")});
        });
        QVERIFY(out2.contains(QStringLiteral("\"order\"")));
    }
};
QTEST_MAIN(TstCommandDispatch)
#include "tst_command_dispatch.moc"
