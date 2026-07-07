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
#include "storage/sqlite/Database.h"
using namespace openmarketterminal::cli;
using openmarketterminal::Database;

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

// Points HOME at ONE process-lifetime temp dir (never at a fresh one per
// call) -- see the Task 6 sandbox-suite comment below for why: headless_
// runtime()'s DB is opened exactly once per binary, at whatever HOME is live
// the first time any sandbox command runs, and stays open at that path for
// the rest of the run. A per-slot QTemporaryDir would rm -rf that path out
// from under the still-open sqlite connection the moment the slot returns.
//
// It ALSO brings the headless runtime (and thus Database::instance()) up via
// one read-only `sandbox list` dispatch, exactly once. That is what makes
// slots that hit Database::instance().execute() directly (fixture inserts)
// work when run STANDALONE via QTest's function filter (e.g.
// `./tst_command_dispatch sandbox_seed_retires_stale_row`): under a filter
// no earlier slot has dispatched anything, so without this bring-up the
// direct insert fails with "DB connection unavailable on this thread".
// `sandbox list` is safe to use for the bring-up because it only SELECTs --
// it inserts no strategy or position rows, so the seed-count exactness in
// the first sandbox slot still holds.
static QString sandbox_test_home() {
    static QTemporaryDir dir;
    qputenv("HOME", dir.path().toUtf8());
    static bool runtime_ready = false;
    if (!runtime_ready) {
        capture_stdout([] {
            return dispatch({QStringLiteral("--json"), QStringLiteral("sandbox"), QStringLiteral("list")});
        });
        runtime_ready = true;
    }
    return dir.path();
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
    void automation_stop_disarms_live_guard() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        using namespace openmarketterminal::cli::automation;
        QVERIFY(write_json_object(live_guard_path("default"),
                                  QJsonObject{{"enabled", true},
                                              {"expires_at", QDateTime::currentDateTimeUtc().addSecs(3600).toString(Qt::ISODateWithMs)}},
                                  nullptr));
        int rc = -1;
        const QString out = capture_stdout([&]() {
            rc = dispatch({QStringLiteral("--json"), QStringLiteral("automation"), QStringLiteral("stop")});
            return rc;
        });
        QCOMPARE(rc, 0);
        QVERIFY(out.contains(QStringLiteral("\"live_guard_disarmed\":true")));
        QCOMPARE(read_json_object(live_guard_path("default")).value("enabled").toBool(), false);
    }
    void automation_status_shows_guard() {
        QTemporaryDir home;
        qputenv("HOME", home.path().toUtf8());
        using namespace openmarketterminal::cli::automation;
        QVERIFY(write_json_object(live_guard_path("default"),
                                  QJsonObject{{"enabled", true}}, nullptr));
        const QString out = capture_stdout([&]() {
            return dispatch({QStringLiteral("--json"), QStringLiteral("automation"), QStringLiteral("status")});
        });
        QVERIFY2(out.contains(QStringLiteral("\"live_guard\"")), "status must surface the live guard");
    }

    // --- Task 6: `sandbox` CLI group ---------------------------------------
    //
    // headless_runtime() (CommandDispatch.cpp) is a process-lifetime
    // singleton -- its very first init() call (triggered below by the first
    // sandbox dispatch, since coverage_command never actually touches the
    // DB despite taking --headless) wins the profile/DB for the rest of
    // THIS test binary's run, and every later init() call (any --profile,
    // any HOME) is a no-op. That means sandbox_strategy/sandbox_position
    // live in one shared DB across every slot below -- exactly like
    // tst_sandbox_executor.cpp's shared-DB model -- AND that whatever HOME
    // was live at that first init is where the sqlite file physically
    // lives for good: a later slot's QTemporaryDir going out of scope would
    // rm -rf that directory out from under the open connection (observed as
    // "disk I/O error"), so sandbox_test_home() hands out one static,
    // process-lifetime temp dir instead of a fresh one per slot. These
    // slots are declared (and must stay) in this order so the counts below
    // are exact: this is the very first code anywhere in the process to
    // touch sandbox_strategy, so seeding starts from a genuinely empty
    // table.
    //
    // Self-isolation rule: EVERY slot below that touches the DB -- whether
    // through dispatch() or via Database::instance().execute() directly --
    // must call sandbox_test_home() as its FIRST line (it is idempotent).
    // QTest's function filter (`./tst_command_dispatch <slot_name>`) runs a
    // slot with no predecessors, so a slot may not lean on an earlier slot
    // having set HOME or brought the runtime/DB up; sandbox_test_home()
    // does both.
    void sandbox_seed_list_pause_resume_retire() {
        sandbox_test_home();
        int rc = -1;
        QJsonObject seeded = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "seed"}, &rc);
        QCOMPARE(rc, 0);
        const QJsonArray seeded_ids = seeded.value("seeded").toArray();
        QCOMPARE(seeded_ids.size(), 5);
        QCOMPARE(seeded.value("retired_stale").toInt(-1), 0);

        QJsonObject listed = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "list"}, &rc);
        QCOMPARE(rc, 0);
        const QJsonArray rows = listed.value("strategies").toArray();
        QCOMPARE(rows.size(), 5);

        QString spot_id;
        QString btc5m_id;
        for (const QJsonValue& v : rows) {
            const QJsonObject row = v.toObject();
            QCOMPARE(row.value("status").toString(), QString("active"));
            if (row.value("kind").toString() == QLatin1String("spot"))
                spot_id = row.value("strategy_id").toString();
            if (row.value("kind").toString() == QLatin1String("btc5m"))
                btc5m_id = row.value("strategy_id").toString();
        }
        QVERIFY(!spot_id.isEmpty());
        QVERIFY(!btc5m_id.isEmpty());

        // pause prints the updated row and flips it out of the active count.
        const QJsonObject paused = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "pause", spot_id}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(paused.value("strategy_id").toString(), spot_id);
        QCOMPARE(paused.value("status").toString(), QString("paused"));

        QJsonObject active_after_pause = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "list", "--status", "active"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(active_after_pause.value("strategies").toArray().size(), 4);

        // resume flips it back.
        const QJsonObject resumed = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "resume", spot_id}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(resumed.value("status").toString(), QString("active"));
        QJsonObject active_after_resume = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "list", "--status", "active"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(active_after_resume.value("strategies").toArray().size(), 5);

        // retire is permanent-by-convention here but still just a status flip.
        const QJsonObject retired = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "retire", btc5m_id}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(retired.value("status").toString(), QString("retired"));
        QJsonObject active_after_retire = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "list", "--status", "active"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(active_after_retire.value("strategies").toArray().size(), 4);

        // put it back so later slots in this file see the full season-1 set.
        rc = -1;
        capture_stdout([&]() {
            rc = dispatch({QStringLiteral("sandbox"), QStringLiteral("resume"), btc5m_id});
            return rc;
        });
        QCOMPARE(rc, 0);

        // invalid id -> non-zero exit + stderr message, no crash.
        for (const QString& sub : {QStringLiteral("pause"), QStringLiteral("resume"), QStringLiteral("retire")}) {
            int bad_rc = -1;
            capture_stdout([&]() {
                bad_rc = dispatch({QStringLiteral("--json"), QStringLiteral("sandbox"), sub,
                                   QStringLiteral("no-such-strategy-id")});
                return bad_rc;
            });
            QVERIFY2(bad_rc != 0, qUtf8Printable(sub));
        }
    }

    void sandbox_positions_empty_before_any_position_rows() {
        sandbox_test_home();
        int rc = -1;
        const QJsonObject out = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "positions"}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(out.value("positions").toArray().isEmpty());
    }

    void sandbox_tick_on_empty_journal_is_all_zero() {
        sandbox_test_home();
        int rc = -1;
        const QJsonObject out = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "tick"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(out.value("opened").toInt(-1), 0);
        QCOMPARE(out.value("filled").toInt(-1), 0);
        QCOMPARE(out.value("unfilled").toInt(-1), 0);
        QCOMPARE(out.value("closed").toInt(-1), 0);
        QCOMPARE(out.value("skipped").toInt(-1), 0);
    }

    void sandbox_seed_retires_stale_row() {
        sandbox_test_home();
        // A hand-inserted 'active' row sharing a seed kind ('spot') but NOT
        // one of seed_default_strategies' current ids is exactly the
        // old-param-book-left-behind scenario the T5 review flagged: without
        // the retire pass, `sandbox seed` would leave it active forever and
        // PaperExecutor::run_cycle would keep trading a stale param set.
        const QString bogus_id = QStringLiteral("bogus_stale_spot_task6");
        auto ins = Database::instance().execute(
            "INSERT INTO sandbox_strategy (strategy_id, kind, symbols, params_json, status, created_at, notes) "
            "VALUES (?,?,?,?,?,?,?)",
            {bogus_id, QStringLiteral("spot"), QStringLiteral("BTC-USD"), QStringLiteral("{}"),
             QStringLiteral("active"), QDateTime::currentMSecsSinceEpoch(), QStringLiteral("")});
        QVERIFY2(ins.is_ok(), ins.is_err() ? ins.error().c_str() : "");

        int rc = -1;
        const QJsonObject seeded = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "seed"}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(seeded.value("retired_stale").toInt(0) >= 1);

        const QJsonObject listed = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "list"}, &rc);
        QCOMPARE(rc, 0);
        bool found = false;
        for (const QJsonValue& v : listed.value("strategies").toArray()) {
            const QJsonObject row = v.toObject();
            if (row.value("strategy_id").toString() == bogus_id) {
                found = true;
                QCOMPARE(row.value("status").toString(), QString("retired"));
            }
        }
        QVERIFY2(found, "the bogus stale row must still be listed (just retired), not deleted");
    }

    void sandbox_positions_null_realized_pnl_is_json_null_not_zero() {
        sandbox_test_home();
        // Data-gap contract (PaperExecutor.h): a position closed with no
        // pre-expiry tick to close against gets realized_pnl = NULL, and
        // that MUST serialize as JSON null, never a coalesced 0 -- 0 would
        // read as "closed flat" instead of "no data to score."
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const QString decision_id = QStringLiteral("task6_nullpnl_decision");
        auto ins = Database::instance().execute(
            "INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side, hypothetical,"
            " qty, limit_price, target_price, stop_price, expires_at, state, opened_at, closed_at,"
            " entry_fee, exit_fee, realized_pnl, close_reason, data_quality, notional_usd, created_at)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("task6_nullpnl_position"), QStringLiteral("task6_nullpnl_strategy"), decision_id,
             QStringLiteral("BTC-USD"), QStringLiteral("buy"), 0, 1.0, 100.0, QVariant(), QVariant(), now,
             QStringLiteral("closed"), now, now, 0.0, 0.0, QVariant(), QStringLiteral("expiry"),
             QStringLiteral("degraded"), 50.0, now});
        QVERIFY2(ins.is_ok(), ins.is_err() ? ins.error().c_str() : "");

        int rc = -1;
        const QString raw = capture_stdout([&]() {
            rc = dispatch({QStringLiteral("--json"), QStringLiteral("sandbox"), QStringLiteral("positions"),
                           QStringLiteral("--closed")});
            return rc;
        });
        QCOMPARE(rc, 0);
        QVERIFY2(raw.contains(QStringLiteral("\"realized_pnl\":null")),
                 qUtf8Printable(raw));

        const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
        bool found = false;
        for (const QJsonValue& v : doc.object().value("positions").toArray()) {
            const QJsonObject row = v.toObject();
            if (row.value("decision_id").toString() == decision_id) {
                found = true;
                QVERIFY(row.contains("realized_pnl"));
                QVERIFY(row.value("realized_pnl").isNull());
            }
        }
        QVERIFY2(found, "the closed null-pnl fixture row must be returned by --closed");
    }
};
QTEST_MAIN(TstCommandDispatch)
#include "tst_command_dispatch.moc"
