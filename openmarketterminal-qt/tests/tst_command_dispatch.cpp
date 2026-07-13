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
#include "storage/repositories/SettingsRepository.h"
using namespace openmarketterminal::cli;
using openmarketterminal::Database;

namespace openmarketterminal::cli {
// Defined in ServeCommand.cpp (compiled into this test binary); declared here
// to unit-test the lock-contention return path of the daemon's job updater
// that launch_scheduled_job's double-launch guard depends on.
bool update_job_by_id(const QString& profile, const QString& id,
                      const std::function<void(QJsonObject&)>& fn, int lock_timeout_ms);
}

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
    const QString kalshi_evidence = dir.path() + QStringLiteral("/kalshi-evidence");
    QDir().mkpath(kalshi_evidence);
    qputenv("OPENTERMINAL_KALSHI_EVIDENCE_DIR", kalshi_evidence.toUtf8());
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
        QCOMPARE(dispatch(QStringList{"--profile", profile, "daemon", "jobs", "add", "chronos2", "BTC-USD",
                                      "--horizon", "15m", "--every-sec", "900"}), 0);
        QCOMPARE(dispatch(QStringList{"--profile", profile, "daemon", "jobs", "add", "chronos2-equity", "AAPL",
                                      "--horizon", "1d", "--period", "2y", "--every-sec", "86400"}), 0);
        QCOMPARE(dispatch(QStringList{"--profile", profile, "daemon", "jobs", "add", "health-check",
                                      "--every-sec", "300"}), 0);
        QCOMPARE(dispatch(QStringList{"--profile", profile, "daemon", "notify", "--job", "--title", "OpenTerminal",
                                      "--message", "Daemon is alive", "--every-sec", "3600"}), 0);

        int rc = -1;
        const QJsonObject listed = json_object_from_dispatch(
            QStringList{"--json", "--profile", profile, "daemon", "jobs", "list"}, &rc);
        QCOMPARE(rc, 0);
        const QJsonArray jobs = listed.value("jobs").toArray();
        QCOMPARE(jobs.size(), 8);
        auto command_at = [&](int i) { return json_strings(jobs.at(i).toObject().value("command").toArray()); };
        QCOMPARE(jobs.at(0).toObject().value("kind").toString(), QString("ai"));
        QCOMPARE(command_at(0), (QStringList{"brief", "SPY"}));
        QCOMPARE(command_at(1), (QStringList{"radar", "AI semiconductors"}));
        QCOMPARE(command_at(2), (QStringList{"notebook", "run", "trading-sma-crossover-backtest"}));
        QCOMPARE(command_at(3), (QStringList{"strategy", "paper-run", "meanrev", "--max-iters", "1", "--interval-sec", "0"}));
        QCOMPARE(command_at(4), (QStringList{"edge", "chronos2", "forecast", "BTC-USD", "--horizon", "15m",
                                             "--journal", "--min-journal-edge-bps", "15.00"}));
        QCOMPARE(command_at(5), (QStringList{"edge", "chronos2", "equity", "AAPL", "--horizon", "1d",
                                             "--period", "2y", "--journal", "--min-journal-edge-bps", "50.00"}));
        QCOMPARE(command_at(6), (QStringList{"daemon", "health"}));
        QCOMPARE(command_at(7), (QStringList{"notify", "send", "--all", "--title", "OpenTerminal",
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
    // Proof protocol v2: the seed set is 28 rows (Kraken/Coinbase scalp,
    // spot 1h/4h/1d, 18 Kalshi horizon/cohort/exit-policy books, long_short, and
    // chronos2/1h/1d/equity -- btc5m/chronos2_5m are retired), and
    // 'spot' and 'kalshi' are each THREE rows of one kind (distinct
    // strategy_ids). This test picks the LAST-seen row of kind 'spot' for the
    // pause/resume half and the long_short row for the retire half --
    // exercising the pause/resume/retire status-flip contract on two distinct
    // still-seeded kinds.
    void sandbox_seed_list_pause_resume_retire() {
        sandbox_test_home();
        int rc = -1;
        QJsonObject seeded = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "seed"}, &rc);
        QCOMPARE(rc, 0);
        const QJsonArray seeded_ids = seeded.value("seeded").toArray();
        QCOMPARE(seeded_ids.size(), 28);
        QCOMPARE(seeded.value("retired_stale").toInt(-1), 0);

        QJsonObject listed = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "list"}, &rc);
        QCOMPARE(rc, 0);
        const QJsonArray rows = listed.value("strategies").toArray();
        QCOMPARE(rows.size(), 28);

        QString spot_id;
        QString long_short_id;
        for (const QJsonValue& v : rows) {
            const QJsonObject row = v.toObject();
            QCOMPARE(row.value("status").toString(), QString("active"));
            if (row.value("kind").toString() == QLatin1String("spot"))
                spot_id = row.value("strategy_id").toString();
            if (row.value("kind").toString() == QLatin1String("long_short"))
                long_short_id = row.value("strategy_id").toString();
        }
        QVERIFY(!spot_id.isEmpty());
        QVERIFY(!long_short_id.isEmpty());

        // pause prints the updated row and flips it out of the active count.
        const QJsonObject paused = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "pause", spot_id}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(paused.value("strategy_id").toString(), spot_id);
        QCOMPARE(paused.value("status").toString(), QString("paused"));

        QJsonObject active_after_pause = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "list", "--status", "active"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(active_after_pause.value("strategies").toArray().size(), 27);

        // resume flips it back.
        const QJsonObject resumed = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "resume", spot_id}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(resumed.value("status").toString(), QString("active"));
        QJsonObject active_after_resume = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "list", "--status", "active"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(active_after_resume.value("strategies").toArray().size(), 28);

        // retire is permanent-by-convention here but still just a status flip.
        const QJsonObject retired = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "retire", long_short_id}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(retired.value("status").toString(), QString("retired"));
        QJsonObject active_after_retire = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "list", "--status", "active"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(active_after_retire.value("strategies").toArray().size(), 27);

        // put it back so later slots in this file see the full season-1 set.
        rc = -1;
        capture_stdout([&]() {
            rc = dispatch({QStringLiteral("sandbox"), QStringLiteral("resume"), long_short_id});
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
        // Retirement now happens inside the registry so GUI and CLI seeding
        // share the same quarantine behavior. The CLI's follow-up pass may
        // therefore have nothing left to count.
        QVERIFY(seeded.value("retired_stale").toInt(0) >= 0);

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

    // --- Task 7: `sandbox install-jobs` / `remove-jobs` ---------------------
    void sandbox_install_jobs_creates_managed_jobs() {
        sandbox_test_home();
        int rc = -1;
        const QJsonObject out = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "install-jobs"}, &rc);
        QCOMPARE(rc, 0);
        const QJsonArray jobs = out.value("jobs").toArray();
        QVERIFY2(jobs.size() >= 11, "install-jobs must create the full sandbox proof stack");
        bool has_tick = false;
        bool has_score = false;
        bool has_btc_ticks = false;
        bool has_long_short = false;
        bool has_chronos_5m = false;
        bool has_chronos_15m = false;
        bool has_chronos_1h = false;
        bool has_chronos_1d = false;
        bool has_spot_swing_1h = false;
        bool has_spot_swing_4h = false;
        bool has_spot_swing_1d = false;
        bool has_stale_crypto_universe_60 = false;
        bool has_btc5m_producer = false;
        bool has_kalshi_auto_15m = false;
        bool has_kalshi_auto_1h = false;
        bool has_kalshi_auto_daily = false;
        bool has_kalshi_recovery_watchdog = false;
        bool has_kalshi_live_autonomous_pulse = false;
        bool has_kalshi_live_reconcile = false;
        bool has_legacy_kalshi_scan = false;
        auto starts_with = [](const QStringList& command, const QStringList& prefix) {
            if (command.size() < prefix.size())
                return false;
            for (int i = 0; i < prefix.size(); ++i)
                if (command.at(i) != prefix.at(i))
                    return false;
            return true;
        };
        for (const QJsonValue& v : jobs) {
            const QJsonObject job = v.toObject();
            QCOMPARE(job.value("managed_by").toString(), QStringLiteral("strategy-sandbox"));
            QVERIFY(job.value("enabled").toBool());
            const QStringList command = json_strings(job.value("command").toArray());
            if (starts_with(command, QStringList{"edge", "collect", "BTC"})) {
                has_btc_ticks = true;
            } else if (starts_with(command, QStringList{"edge", "long-short-strategy", "BTC-USD"})) {
                has_long_short = true;
            } else if (starts_with(command, QStringList{"edge", "chronos2", "forecast", "BTC-USD"}) &&
                       command.contains(QStringLiteral("--horizon")) &&
                       command.at(command.indexOf(QStringLiteral("--horizon")) + 1) == QStringLiteral("5m")) {
                // Real-horizon reshape (task 3): no installed job may run a
                // 5m chronos forecast -- unreal, no venue, retired kind.
                has_chronos_5m = true;
            } else if (command == QStringList{"edge", "chronos2", "forecast", "BTC-USD", "--horizon", "15m",
                                             "--journal", "--min-journal-edge-bps", "15"}) {
                has_chronos_15m = true;
            } else if (command == QStringList{"edge", "chronos2", "forecast", "BTC-USD", "--horizon", "1h",
                                             "--journal", "--min-journal-edge-bps", "35"}) {
                has_chronos_1h = true;
            } else if (command == QStringList{"edge", "chronos2", "forecast", "BTC-USD", "--horizon", "1d",
                                             "--journal", "--min-journal-edge-bps", "75"}) {
                has_chronos_1d = true;
            } else if (starts_with(command, QStringList{"edge", "spot-swing-gate"})) {
                // Real-horizon reshape (task 3): the spot feed emits >=1h
                // horizons via spot-swing-gate, not crypto-universe
                // --horizon-sec 60. One job per real spot book horizon
                // (1h/4h/1d) must be installed.
                const int hidx = command.indexOf(QStringLiteral("--horizon"));
                const QString hz = (hidx >= 0 && hidx + 1 < command.size()) ? command.at(hidx + 1) : QString();
                if (hz == QStringLiteral("1h"))
                    has_spot_swing_1h = true;
                else if (hz == QStringLiteral("4h"))
                    has_spot_swing_4h = true;
                else if (hz == QStringLiteral("1d"))
                    has_spot_swing_1d = true;
            } else if (starts_with(command, QStringList{"kalshi", "auto", "run"})) {
                const int cidx = command.indexOf(QStringLiteral("--category"));
                const QString category = cidx >= 0 && cidx + 1 < command.size()
                    ? command.at(cidx + 1) : QString();
                if (category == QStringLiteral("Crypto#BTC@live"))
                    has_kalshi_auto_15m = has_kalshi_recovery_watchdog = true;
                else if (category == QStringLiteral("Crypto#BTC@hourly"))
                    has_kalshi_auto_1h = true;
                else if (category == QStringLiteral("Crypto#BTC@daily"))
                    has_kalshi_auto_daily = true;
                if (category == QStringLiteral("Crypto#BTC@live") ||
                    category == QStringLiteral("Crypto#BTC@hourly")) {
                    QCOMPARE(job.value("interval_sec").toInt(), 60);
                    QCOMPARE(job.value("timeout_sec").toInt(), 30);
                }
            } else if (starts_with(command, QStringList{"kalshi", "auto", "live", "execute-next"})) {
                has_kalshi_live_autonomous_pulse = command.contains(QStringLiteral("--require-session"));
                QCOMPARE(job.value("interval_sec").toInt(), 2);
                QCOMPARE(job.value("timeout_sec").toInt(), 10);
            } else if (command == QStringList{"kalshi", "auto", "positions"}) {
                has_kalshi_live_reconcile = true;
            } else if (command.contains(QStringLiteral("journal-kalshi-scan"))) {
                has_legacy_kalshi_scan = true;
            } else if (command == QStringList{"edge", "crypto-universe", "--venue", "coinbase", "--horizon-sec",
                                             "60", "--duration-ms", "1500", "--min-edge-bps", "25"}) {
                has_stale_crypto_universe_60 = true;
            } else if (command.contains(QStringLiteral("evaluate-btc5m-live"))) {
                // Real-horizon reshape (task 3): the btc5m book is retired, so
                // its producer job (source='edge journal-evaluate-btc5m-live')
                // must be gone -- nothing consumes those rows anymore.
                has_btc5m_producer = true;
            } else if (command == QStringList{"sandbox", "tick"}) {
                has_tick = true;
                QCOMPARE(job.value("interval_sec").toInt(), 30);
                QCOMPARE(job.value("timeout_sec").toInt(), 25);
            } else if (command == QStringList{"sandbox", "score-now"}) {
                has_score = true;
                QCOMPARE(job.value("interval_sec").toInt(), 21600);
                QCOMPARE(job.value("timeout_sec").toInt(), 120);
            }
        }
        QVERIFY2(has_btc_ticks, "install-jobs must keep BTC tick data warm");
        QVERIFY2(has_long_short, "install-jobs must create the Coinbase long/short proof producer");
        QVERIFY2(!has_chronos_5m, "install-jobs must not install a 5m chronos forecast job");
        QVERIFY2(has_chronos_15m, "install-jobs must create the Chronos BTC 15m producer");
        QVERIFY2(has_chronos_1h, "install-jobs must create the Chronos BTC 1h producer");
        QVERIFY2(has_chronos_1d, "install-jobs must create the Chronos BTC 1d producer");
        QVERIFY2(has_spot_swing_1h,
                 "install-jobs must create the spot-swing-gate 1h producer feeding the spot_1h book");
        QVERIFY2(has_spot_swing_4h,
                 "install-jobs must create the spot-swing-gate 4h producer feeding the spot_4h book");
        QVERIFY2(has_spot_swing_1d,
                 "install-jobs must create the spot-swing-gate 1d producer feeding the spot_1d book");
        QVERIFY2(!has_stale_crypto_universe_60,
                 "install-jobs must not install the stale crypto-universe --horizon-sec 60 spot job");
        QVERIFY2(!has_btc5m_producer,
                 "install-jobs must not install a btc5m producer job -- the btc5m book is retired, so its "
                 "'edge journal-evaluate-btc5m-live' rows have no consumer");
        QVERIFY2(has_kalshi_auto_15m, "install-jobs must feed Kalshi v2 15m cohorts");
        QVERIFY2(!has_kalshi_auto_1h,
                 "the persistent event engine must replace the duplicate hourly polling producer");
        QVERIFY2(has_kalshi_auto_daily, "install-jobs must feed Kalshi v2 daily cohorts");
        QVERIFY2(has_kalshi_recovery_watchdog,
                 "install-jobs must retain one slow WebSocket recovery watchdog");
        QVERIFY2(!has_kalshi_live_autonomous_pulse,
                 "install-jobs must retire the two-second autonomous polling pulse");
        QVERIFY2(has_kalshi_live_reconcile,
                 "install-jobs must persist authenticated Kalshi positions and fills for scoring");
        QVERIFY2(!has_legacy_kalshi_scan,
                 "install-jobs must not recreate the retired polluted Kalshi scan source");
        QVERIFY2(has_tick, "install-jobs must create the sandbox tick job");
        QVERIFY2(has_score, "install-jobs must create the sandbox score-now job");
    }

    void kalshi_bounded_auto_session_enforces_rolling_hour_limit() {
        sandbox_test_home();
        auto& settings = openmarketterminal::SettingsRepository::instance();
        QVERIFY(settings.set(QStringLiteral("cli.allow_trading"), QStringLiteral("true"),
                             QStringLiteral("test")).is_ok());
        QVERIFY(settings.set(QStringLiteral("cli.live_trading_armed"), QStringLiteral("true"),
                             QStringLiteral("test")).is_ok());
        QVERIFY(settings.set(QStringLiteral("cli.allowed_venues"), QStringLiteral("kalshi"),
                             QStringLiteral("test")).is_ok());
        QVERIFY(settings.set(QStringLiteral("cli.kill_switch"), QStringLiteral("true"),
                             QStringLiteral("test")).is_ok());

        int rc = -1;
        QJsonObject blocked = json_object_from_dispatch(
            {QStringLiteral("--json"), QStringLiteral("kalshi"), QStringLiteral("auto"),
             QStringLiteral("live"), QStringLiteral("session"), QStringLiteral("1h")}, &rc);
        QCOMPARE(rc, 6);
        QCOMPARE(blocked.value(QStringLiteral("status")).toString(), QStringLiteral("blocked"));
        QVERIFY(blocked.value(QStringLiteral("reason")).toString().contains(
            QStringLiteral("Settings > Security")));
        QCOMPARE(settings.get(QStringLiteral("cli.kill_switch"), QString()).value(),
                 QStringLiteral("true"));
        QVERIFY(settings.get(QStringLiteral("kalshi.live_automation.enabled"), QStringLiteral("false"))
                    .value() != QStringLiteral("true"));

        QVERIFY(settings.set(QStringLiteral("cli.kill_switch"), QStringLiteral("false"),
                             QStringLiteral("test")).is_ok());
        const QString unknown_intent = QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("asset_class"), QStringLiteral("prediction")},
            {QStringLiteral("venue"), QStringLiteral("kalshi")},
            {QStringLiteral("market_id"), QStringLiteral("KXBTC-UNKNOWN")},
            {QStringLiteral("outcome"), QStringLiteral("Yes")},
            {QStringLiteral("experiment_id"), QStringLiteral("kalshi-micro-live-v1")}})
            .toJson(QJsonDocument::Compact));
        QVERIFY(Database::instance().execute(
            "INSERT INTO order_drafts (draft_id,intent_json,status,created_at) VALUES (?,?,?,?)",
            {QStringLiteral("unknown-order"), unknown_intent,
             QStringLiteral("submission_unknown"),
             QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}).is_ok());
        QJsonObject reconciliation_blocked = json_object_from_dispatch(
            {QStringLiteral("--json"), QStringLiteral("kalshi"), QStringLiteral("auto"),
             QStringLiteral("live"), QStringLiteral("session"), QStringLiteral("1h")}, &rc);
        QCOMPARE(rc, 6);
        QCOMPARE(reconciliation_blocked.value(QStringLiteral("status")).toString(),
                 QStringLiteral("blocked"));
        QVERIFY(reconciliation_blocked.value(QStringLiteral("reconciliation_required")).toBool());
        QCOMPARE(reconciliation_blocked.value(QStringLiteral("submission_unknown_count")).toInt(), 1);
        QVERIFY(Database::instance().execute(
            "DELETE FROM order_drafts WHERE draft_id='unknown-order'").is_ok());

        QJsonObject armed = json_object_from_dispatch(
            {QStringLiteral("--json"), QStringLiteral("kalshi"), QStringLiteral("auto"),
             QStringLiteral("live"), QStringLiteral("session"), QStringLiteral("1h")}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(armed.value(QStringLiteral("session_active")).toBool());
        QVERIFY(armed.value(QStringLiteral("live_armed")).toBool());
        QVERIFY(armed.value(QStringLiteral("global_live_gate_armed")).toBool());
        QVERIFY(armed.value(QStringLiteral("autonomous")).toBool());
        QVERIFY(armed.value(QStringLiteral("parallel_paper_enabled")).toBool());
        QCOMPARE(armed.value(QStringLiteral("max_orders_per_hour")).toInt(), 10);
        QVERIFY(!armed.value(QStringLiteral("submission_requires_human_approval")).toBool());
        QVERIFY(!armed.value(QStringLiteral("session")).toObject()
                     .value(QStringLiteral("session_id")).toString().isEmpty());
        const QJsonObject armed_session = armed.value(QStringLiteral("session")).toObject();
        QCOMPARE(armed_session.value(QStringLiteral("schedule")).toString(),
                 QStringLiteral("current_clock_hour"));
        const QDateTime armed_at = QDateTime::fromString(
            armed_session.value(QStringLiteral("started_at")).toString(), Qt::ISODateWithMs);
        const QDateTime ends_at = QDateTime::fromString(
            armed_session.value(QStringLiteral("ends_at")).toString(), Qt::ISODateWithMs);
        QVERIFY(armed_at.isValid());
        QVERIFY(ends_at.isValid());
        QCOMPARE(ends_at.toUTC().time().minute(), 0);
        QCOMPARE(ends_at.toUTC().time().second(), 0);
        QCOMPARE(ends_at.toUTC().time().msec(), 0);
        QVERIFY(armed_at < ends_at);
        QVERIFY(armed_at.secsTo(ends_at) <= 3600);
        QCOMPARE(settings.get(QStringLiteral("kalshi.paper_automation.enabled"), QString()).value(),
                 QStringLiteral("true"));

        QJsonObject paper_status = json_object_from_dispatch(
            {QStringLiteral("--json"), QStringLiteral("kalshi"), QStringLiteral("auto"),
             QStringLiteral("paper"), QStringLiteral("status")}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(paper_status.value(QStringLiteral("enabled")).toBool());
        QVERIFY(paper_status.value(QStringLiteral("hourly_limit")).isNull());
        QVERIFY(paper_status.value(QStringLiteral("execution")).toString().contains(
            QStringLiteral("same timestamped decision gate")));

        QVERIFY(Database::instance().execute(
            "DELETE FROM trade_audit WHERE json_extract(intent_json,'$.experiment_id')="
            "'kalshi-micro-live-v1'").is_ok());
        const QJsonObject autonomous_intent{
            {QStringLiteral("asset_class"), QStringLiteral("prediction")},
            {QStringLiteral("venue"), QStringLiteral("kalshi")},
            {QStringLiteral("experiment_id"), QStringLiteral("kalshi-micro-live-v1")},
            {QStringLiteral("autonomous"), true}};
        for (int i = 0; i < 10; ++i) {
            QJsonObject row_intent = autonomous_intent;
            if (i == 0)
                row_intent.remove(QStringLiteral("autonomous")); // Legacy normalized draft.
            const QString intent = QString::fromUtf8(
                QJsonDocument(row_intent).toJson(QJsonDocument::Compact));
            QVERIFY(Database::instance().execute(
                "INSERT INTO trade_audit (ts,phase,tool,account,mode,intent_json,decision,reason,risk_snapshot_json) "
                "VALUES (?,?,?,?,?,?,?,?,?)",
                {QDateTime::currentDateTimeUtc().addSecs(-i).toString(Qt::ISODateWithMs),
                 QStringLiteral("submit"), QStringLiteral("submit_order"), QStringLiteral(""),
                 QStringLiteral("live"), intent, QStringLiteral("filled"),
                 QStringLiteral("test fill"), QStringLiteral("{}")}).is_ok());
        }

        QJsonObject status = json_object_from_dispatch(
            {QStringLiteral("--json"), QStringLiteral("kalshi"), QStringLiteral("auto"),
             QStringLiteral("live"), QStringLiteral("status")}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(status.value(QStringLiteral("orders_last_hour")).toInt(), 10);
        QCOMPARE(status.value(QStringLiteral("orders_remaining_this_hour")).toInt(), 0);

        QJsonObject pulse = json_object_from_dispatch(
            {QStringLiteral("--json"), QStringLiteral("kalshi"), QStringLiteral("auto"),
             QStringLiteral("live"), QStringLiteral("execute-next"),
             QStringLiteral("--require-session")}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(pulse.value(QStringLiteral("status")).toString(), QStringLiteral("rate_limited"));
        QVERIFY(pulse.value(QStringLiteral("reason")).toString().contains(QStringLiteral("10")));

        QJsonObject stopped = json_object_from_dispatch(
            {QStringLiteral("--json"), QStringLiteral("kalshi"), QStringLiteral("auto"),
             QStringLiteral("live"), QStringLiteral("session"), QStringLiteral("stop")}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(!stopped.value(QStringLiteral("session_active")).toBool());
        QVERIFY(!stopped.value(QStringLiteral("live_armed")).toBool());
        QVERIFY(stopped.value(QStringLiteral("global_live_gate_armed")).toBool());
        QCOMPARE(settings.get(QStringLiteral("kalshi.live_automation.enabled"), QString()).value(),
                 QStringLiteral("false"));
        settings.set(QStringLiteral("cli.allow_trading"), QStringLiteral("false"), QStringLiteral("test"));
        settings.set(QStringLiteral("cli.live_trading_armed"), QStringLiteral("false"), QStringLiteral("test"));
        settings.set(QStringLiteral("cli.allowed_venues"), QString(), QStringLiteral("test"));
    }

    // Reconcile regression: when a producer drops OUT of sandbox_job_specs()
    // (e.g. the real-horizon reshape retiring crypto-universe-60, btc5m,
    // chronos-5m), install-jobs must disable the now-orphaned managed job in
    // jobs.json rather than leaving it enabled with no consumer forever.
    void sandbox_install_jobs_retires_dropped_spec_jobs() {
        sandbox_test_home();
        int rc = -1;
        json_object_from_dispatch(QStringList{"--json", "sandbox", "install-jobs"}, &rc);
        QCOMPARE(rc, 0);

        const QString jobs_path = profile_root_for(QStringLiteral("default")) +
                                   QStringLiteral("/daemon/jobs.json");
        QFile jobs_file(jobs_path);
        QVERIFY2(jobs_file.open(QIODevice::ReadOnly | QIODevice::Text), "jobs.json must exist after install-jobs");
        QJsonParseError perr;
        QJsonDocument doc = QJsonDocument::fromJson(jobs_file.readAll(), &perr);
        jobs_file.close();
        QCOMPARE(perr.error, QJsonParseError::NoError);
        QVERIFY(doc.isObject());
        QJsonArray jobs = doc.object().value("jobs").toArray();

        // Inject a stale managed job whose sandbox_job key is NOT (and never
        // will be) in sandbox_job_specs() -- simulates a producer that just
        // got dropped from the spec by a reshape.
        const QString now = QStringLiteral("2020-01-01T00:00:00Z");
        QJsonObject stale{
            {"id", QStringLiteral("job_obsolete")},
            {"name", QStringLiteral("Strategy sandbox obsolete feed")},
            {"kind", QStringLiteral("command")},
            {"enabled", true},
            {"schedule", QStringLiteral("interval")},
            {"interval_sec", 60},
            {"timeout_sec", 30},
            {"next_run_at", now},
            {"running", false},
            {"run_count", 0},
            {"fail_count", 0},
            {"created_at", now},
            {"updated_at", now},
            {"managed_by", QStringLiteral("strategy-sandbox")},
            {"sandbox_job", QStringLiteral("obsolete-feed")},
            {"description", QStringLiteral("stale producer left over from a dropped spec entry")},
            {"command", QJsonArray{QStringLiteral("edge"), QStringLiteral("journal"),
                                    QStringLiteral("evaluate-btc5m-live")}},
            {"spec", QJsonObject{{"command", QJsonArray{QStringLiteral("edge"), QStringLiteral("journal"),
                                                         QStringLiteral("evaluate-btc5m-live")}}}}};
        jobs.append(stale);
        QJsonObject out_doc = doc.object();
        out_doc["jobs"] = jobs;
        QVERIFY2(jobs_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text),
                 "must be able to rewrite jobs.json to inject the stale job");
        jobs_file.write(QJsonDocument(out_doc).toJson(QJsonDocument::Compact));
        jobs_file.close();

        // Run install-jobs again -- it must reconcile: disable the stale
        // 'obsolete-feed' job (its key is no longer in the spec) while
        // leaving current spec jobs (e.g. 'tick') enabled.
        const QJsonObject second = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "install-jobs"}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY2(second.value("retired").toInt() >= 1,
                 "install-jobs JSON output must report at least one retired stale job");

        QFile jobs_file2(jobs_path);
        QVERIFY(jobs_file2.open(QIODevice::ReadOnly | QIODevice::Text));
        const QJsonDocument doc2 = QJsonDocument::fromJson(jobs_file2.readAll());
        jobs_file2.close();
        QVERIFY(doc2.isObject());
        bool found_obsolete = false;
        bool obsolete_disabled = false;
        bool found_tick_enabled = false;
        QJsonArray jobs_without_obsolete;
        for (const QJsonValue& v : doc2.object().value("jobs").toArray()) {
            const QJsonObject job = v.toObject();
            if (job.value("managed_by").toString() == QStringLiteral("strategy-sandbox") &&
                job.value("sandbox_job").toString() == QStringLiteral("obsolete-feed")) {
                found_obsolete = true;
                obsolete_disabled = !job.value("enabled").toBool();
                continue;  // drop the synthetic stale job so it doesn't pollute shared
                           // test-process state (sandbox_test_home() reuses one HOME/
                           // jobs.json across every slot in this binary).
            }
            if (job.value("managed_by").toString() == QStringLiteral("strategy-sandbox") &&
                job.value("sandbox_job").toString() == QStringLiteral("tick")) {
                found_tick_enabled = job.value("enabled").toBool();
            }
            jobs_without_obsolete.append(job);
        }
        // Restore jobs.json to the state other test slots in this shared-HOME
        // process expect (no leftover synthetic managed job).
        QJsonObject cleaned_doc = doc2.object();
        cleaned_doc["jobs"] = jobs_without_obsolete;
        QFile jobs_file3(jobs_path);
        if (jobs_file3.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            jobs_file3.write(QJsonDocument(cleaned_doc).toJson(QJsonDocument::Compact));
            jobs_file3.close();
        }

        QVERIFY2(found_obsolete, "the injected stale job must still be present in jobs.json (disabled, not deleted)");
        QVERIFY2(obsolete_disabled, "install-jobs must disable a managed job whose spec key was dropped");
        QVERIFY2(found_tick_enabled, "a still-current spec job (tick) must remain enabled after reconcile");
    }

    void sandbox_remove_jobs_disables_but_does_not_delete() {
        sandbox_test_home();
        int rc = -1;
        const QJsonObject installed = json_object_from_dispatch(QStringList{"--json", "sandbox", "install-jobs"}, &rc);
        QCOMPARE(rc, 0);
        const int installed_count = installed.value("jobs").toArray().size();
        QVERIFY(installed_count > 2);

        const QJsonObject removed = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "remove-jobs"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(removed.value("disabled").toInt(), installed_count);

        const QJsonObject listed = json_object_from_dispatch(
            QStringList{"--json", "daemon", "jobs", "list"}, &rc);
        QCOMPARE(rc, 0);
        int sandbox_jobs_seen = 0;
        for (const QJsonValue& v : listed.value("jobs").toArray()) {
            const QJsonObject job = v.toObject();
            if (job.value("managed_by").toString() != QStringLiteral("strategy-sandbox"))
                continue;
            ++sandbox_jobs_seen;
            QVERIFY2(!job.value("enabled").toBool(), "remove-jobs must disable, not just report, the job");
        }
        QCOMPARE(sandbox_jobs_seen, installed_count);  // disabled, not deleted -- rows must still be present
    }

    void update_job_by_id_reports_failure_under_held_jobs_lock() {
        sandbox_test_home();
        int rc = -1;
        const QJsonObject out = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "install-jobs"}, &rc);
        QCOMPARE(rc, 0);
        const QString job_id = out.value("jobs").toArray().at(0).toObject().value("id").toString();
        QVERIFY(!job_id.isEmpty());

        bool mutated = false;
        {
            openmarketterminal::cli::automation::StateLock held(
                QStringLiteral("default"), QStringLiteral("jobs"));
            QVERIFY2(held.locked(), "test setup: must be able to take the jobs lock");
            const bool ok = update_job_by_id(QStringLiteral("default"), job_id,
                                             [&mutated](QJsonObject&) { mutated = true; },
                                             /*lock_timeout_ms=*/100);
            QVERIFY2(!ok, "update_job_by_id must report failure when the jobs lock is held elsewhere "
                          "-- launch_scheduled_job's double-launch guard depends on this");
            QVERIFY2(!mutated, "the mutation callback must not run when the lock was not acquired");
        }
        // Lock released: the same update must now succeed and persist.
        const bool ok = update_job_by_id(QStringLiteral("default"), job_id,
                                         [](QJsonObject& j) { j["last_error"] = QStringLiteral("t7-probe"); },
                                         /*lock_timeout_ms=*/100);
        QVERIFY(ok);
        const QJsonObject listed = json_object_from_dispatch(
            QStringList{"--json", "daemon", "jobs", "list"}, &rc);
        QCOMPARE(rc, 0);
        bool persisted = false;
        for (const QJsonValue& v : listed.value("jobs").toArray()) {
            const QJsonObject job = v.toObject();
            if (job.value("id").toString() == job_id)
                persisted = job.value("last_error").toString() == QStringLiteral("t7-probe");
        }
        QVERIFY2(persisted, "a true return must mean the mutation actually reached jobs.json");
    }

    // --- Task 9: `sandbox score-now` / `leaderboard` / `book` ---------------
    // These are dispatch/shape tests only -- SandboxScorer's aggregation
    // arithmetic (hand-computed fixture, NULL-pnl exclusion, drawdown, etc.)
    // is covered exhaustively by tst_sandbox_scorer.cpp. Here we just confirm
    // the CLI wires score_all/leaderboard through correctly and that the
    // `ranked` bool follows the resolved>=30 && !hypothetical rule.
    void sandbox_score_now_reports_per_strategy_rollup() {
        sandbox_test_home();
        int rc = -1;
        const QJsonObject out =
            json_object_from_dispatch(QStringList{"--json", "sandbox", "score-now"}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(out.value("scored").toBool());
        const QJsonArray strategies = out.value("strategies").toArray();
        QVERIFY2(!strategies.isEmpty(), "score-now must roll up every registered strategy");
        for (const QJsonValue& v : strategies) {
            const QJsonObject row = v.toObject();
            QVERIFY(row.contains("strategy_id"));
            QVERIFY(row.contains("resolved"));
            QVERIFY(row.contains("net_pnl"));
            QVERIFY(row.contains("ranked"));
        }
    }

    void sandbox_leaderboard_ranked_bool_follows_sample_and_hypothetical_rule() {
        sandbox_test_home();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();

        auto insert_strategy = [](const QString& id, const QString& kind, const QString& params) {
            auto r = Database::instance().execute(
                "INSERT INTO sandbox_strategy (strategy_id, kind, symbols, params_json, status, created_at,"
                " notes) VALUES (?,?,?,?,?,?,?)",
                {id, kind, QStringLiteral("BTC-USD"), params, QStringLiteral("active"),
                 QDateTime::currentMSecsSinceEpoch(), QStringLiteral("")});
            QVERIFY2(r.is_ok(), r.is_err() ? r.error().c_str() : "");
        };
        auto insert_closed = [&](const QString& strategy_id, const QString& position_id, double pnl,
                                 qint64 closed_at) {
            auto r = Database::instance().execute(
                "INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side,"
                " hypothetical, qty, limit_price, expires_at, state, opened_at, closed_at, entry_fee,"
                " exit_fee, realized_pnl, close_reason, data_quality, notional_usd, created_at)"
                " VALUES (?,?,?,?,'buy',0,1.0,100.0,?,'closed',?,?,0,0,?,'target','ok',10.0,?)",
                {position_id, strategy_id, position_id + QStringLiteral("-dec"), QStringLiteral("BTC-USD"),
                 closed_at, closed_at, closed_at, pnl, closed_at});
            QVERIFY2(r.is_ok(), r.is_err() ? r.error().c_str() : "");
        };

        // Ranked: 30 resolved closes, not hypothetical.
        const QString ranked_id = QStringLiteral("t9_ranked_strategy");
        insert_strategy(ranked_id, QStringLiteral("spot"), QStringLiteral("{}"));
        for (int i = 0; i < 30; ++i)
            insert_closed(ranked_id, QStringLiteral("t9_ranked_pos_%1").arg(i), 1.0, now - i * 1000);

        // Insufficient sample: only 2 resolved closes, not hypothetical.
        const QString thin_id = QStringLiteral("t9_thin_strategy");
        insert_strategy(thin_id, QStringLiteral("spot"), QStringLiteral("{}"));
        insert_closed(thin_id, QStringLiteral("t9_thin_pos_0"), 1.0, now);
        insert_closed(thin_id, QStringLiteral("t9_thin_pos_1"), -1.0, now - 1000);

        // Hypothetical: 30 resolved closes, but hypothetical=true in params
        // -- must never be ranked regardless of sample size.
        const QString hyp_id = QStringLiteral("t9_hyp_strategy");
        insert_strategy(hyp_id, QStringLiteral("long_short"), QStringLiteral("{\"hypothetical\":true}"));
        for (int i = 0; i < 30; ++i)
            insert_closed(hyp_id, QStringLiteral("t9_hyp_pos_%1").arg(i), 1.0, now - i * 1000);

        // Flat/zero ranked book (net_pnl == 0, 30 resolved, not hypothetical)
        // -- a valid outcome, not an error: text mode must print
        // "no demonstrated edge" for it rather than treating it specially.
        const QString flat_id = QStringLiteral("t9_flat_strategy");
        insert_strategy(flat_id, QStringLiteral("spot"), QStringLiteral("{}"));
        for (int i = 0; i < 30; ++i)
            insert_closed(flat_id, QStringLiteral("t9_flat_pos_%1").arg(i), (i % 2 == 0) ? 1.0 : -1.0,
                         now - i * 1000);

        int score_rc = -1;
        json_object_from_dispatch(QStringList{"--json", "sandbox", "score-now"}, &score_rc);
        QCOMPARE(score_rc, 0);

        int rc = -1;
        const QJsonObject lb = json_object_from_dispatch(QStringList{"--json", "sandbox", "leaderboard"}, &rc);
        QCOMPARE(rc, 0);
        bool found_ranked = false, found_thin = false, found_hyp = false, found_flat = false;
        for (const QJsonValue& v : lb.value("leaderboard").toArray()) {
            const QJsonObject row = v.toObject();
            const QString id = row.value("strategy_id").toString();
            if (id == ranked_id) {
                found_ranked = true;
                QVERIFY2(row.value("ranked").toBool(), "30 resolved, non-hypothetical must be ranked");
            } else if (id == thin_id) {
                found_thin = true;
                QVERIFY2(!row.value("ranked").toBool(), "2 resolved must be insufficient-sample, never ranked");
            } else if (id == hyp_id) {
                found_hyp = true;
                QVERIFY2(!row.value("ranked").toBool(), "hypothetical must never be ranked regardless of sample");
                QVERIFY(row.value("hypothetical").toBool());
            } else if (id == flat_id) {
                found_flat = true;
                QVERIFY2(row.value("ranked").toBool(), "a flat (net_pnl==0) book with enough samples is still ranked");
                QVERIFY(qFuzzyIsNull(row.value("net_pnl").toDouble()));
            }
        }
        QVERIFY(found_ranked);
        QVERIFY(found_thin);
        QVERIFY(found_hyp);
        QVERIFY(found_flat);

        // Text mode must not crash, must bucket into the documented section
        // headings, and must print "no demonstrated edge" for the flat
        // ranked book -- a valid result, not an error/different code path.
        const QString text = capture_stdout(
            [&]() { return dispatch(QStringList{"sandbox", "leaderboard"}); }, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(text.contains("RANKED"));
        QVERIFY2(text.contains("no demonstrated edge"), qUtf8Printable(text));
        QVERIFY(text.contains("INSUFFICIENT SAMPLE"));
        QVERIFY(text.contains("HYPOTHETICAL"));
    }

    void sandbox_book_returns_per_day_rows_and_live_counts() {
        sandbox_test_home();
        const QString strategy_id = QStringLiteral("t9_book_strategy");
        auto ins = Database::instance().execute(
            "INSERT INTO sandbox_strategy (strategy_id, kind, symbols, params_json, status, created_at,"
            " notes) VALUES (?,?,?,?,?,?,?)",
            {strategy_id, QStringLiteral("spot"), QStringLiteral("BTC-USD"), QStringLiteral("{}"),
             QStringLiteral("active"), QDateTime::currentMSecsSinceEpoch(), QStringLiteral("")});
        QVERIFY2(ins.is_ok(), ins.is_err() ? ins.error().c_str() : "");
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        auto pos = Database::instance().execute(
            "INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side,"
            " hypothetical, qty, limit_price, expires_at, state, opened_at, closed_at, entry_fee, exit_fee,"
            " realized_pnl, close_reason, data_quality, notional_usd, created_at)"
            " VALUES (?,?,?,?,'buy',0,1.0,100.0,?,'closed',?,?,0,0,2.0,'target','ok',10.0,?)",
            {QStringLiteral("t9_book_pos_0"), strategy_id, QStringLiteral("t9_book_dec_0"),
             QStringLiteral("BTC-USD"), now, now, now, now});
        QVERIFY2(pos.is_ok(), pos.is_err() ? pos.error().c_str() : "");

        int score_rc = -1;
        json_object_from_dispatch(QStringList{"--json", "sandbox", "score-now"}, &score_rc);
        QCOMPARE(score_rc, 0);

        int rc = -1;
        const QJsonObject book =
            json_object_from_dispatch(QStringList{"--json", "sandbox", "book", strategy_id}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(book.value("strategy_id").toString(), strategy_id);
        QCOMPARE(book.value("closed_count").toInt(), 1);
        QCOMPARE(book.value("open_count").toInt(), 0);
        const QJsonArray days = book.value("days").toArray();
        QVERIFY2(days.size() >= 1, "book must return at least today's score_date row");
        bool found_resolved_day = false;
        for (const QJsonValue& v : days) {
            if (v.toObject().value("resolved_count").toInt() == 1)
                found_resolved_day = true;
        }
        QVERIFY2(found_resolved_day, "the day the position closed on must show resolved_count=1");

        int bad_rc = -1;
        json_object_from_dispatch(QStringList{"--json", "sandbox", "book", "nonexistent_strategy_id"}, &bad_rc);
        QCOMPARE(bad_rc, 4);
    }

    // T9-review fold: score-now must reject trailing args like every other
    // sandbox subcommand (leaderboard/book already do) instead of silently
    // ignoring them.
    void sandbox_score_now_rejects_trailing_args() {
        sandbox_test_home();
        int rc = -1;
        capture_stdout([&]() {
            rc = dispatch({QStringLiteral("sandbox"), QStringLiteral("score-now"),
                           QStringLiteral("bogus-trailing-arg")});
            return rc;
        });
        QCOMPARE(rc, 2);
    }

    // --- Task 10: `sandbox eligibility` --------------------------------------
    // Dispatch/shape tests only -- the spec-§4.7 boundary matrix itself is
    // covered exhaustively by tst_sandbox_eligibility.cpp's pure-function
    // tests. Here we confirm the CLI wires list_strategies()/leaderboard()/
    // the first-position-date and total_positions queries into
    // evaluate_eligibility correctly, retired books are skipped, and
    // hypothetical books show up as unconditionally blocked.
    void sandbox_eligibility_blocks_thin_sample_and_hypothetical_and_skips_retired() {
        sandbox_test_home();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();

        auto insert_strategy = [](const QString& id, const QString& kind, const QString& params,
                                  const QString& status) {
            auto r = Database::instance().execute(
                "INSERT INTO sandbox_strategy (strategy_id, kind, symbols, params_json, status, created_at,"
                " notes) VALUES (?,?,?,?,?,?,?)",
                {id, kind, QStringLiteral("BTC-USD"), params, status, QDateTime::currentMSecsSinceEpoch(),
                 QStringLiteral("")});
            QVERIFY2(r.is_ok(), r.is_err() ? r.error().c_str() : "");
        };
        auto insert_closed = [&](const QString& strategy_id, const QString& position_id, double pnl,
                                 qint64 closed_at, qint64 created_at) {
            auto r = Database::instance().execute(
                "INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side,"
                " hypothetical, qty, limit_price, expires_at, state, opened_at, closed_at, entry_fee,"
                " exit_fee, realized_pnl, close_reason, data_quality, notional_usd, created_at)"
                " VALUES (?,?,?,?,'buy',0,1.0,100.0,?,'closed',?,?,0,0,?,'target','ok',10.0,?)",
                {position_id, strategy_id, position_id + QStringLiteral("-dec"), QStringLiteral("BTC-USD"),
                 closed_at, closed_at, closed_at, pnl, created_at});
            QVERIFY2(r.is_ok(), r.is_err() ? r.error().c_str() : "");
        };

        // Thin sample: only 5 resolved closes, recent (well under both the
        // 28-day and 30-resolved bars) -> multiple blockers expected.
        const QString thin_id = QStringLiteral("t10_thin_strategy");
        insert_strategy(thin_id, QStringLiteral("spot"), QStringLiteral("{}"), QStringLiteral("active"));
        for (int i = 0; i < 5; ++i)
            insert_closed(thin_id, QStringLiteral("t10_thin_pos_%1").arg(i), 1.0, now - i * 1000, now - i * 1000);

        // Hypothetical: 30 resolved, 40 days of history, positive pnl, tiny
        // drawdown, zero degraded -- would pass every other bar, but must
        // still come back blocked with exactly "hypothetical instrument".
        const QString hyp_id = QStringLiteral("t10_hyp_strategy");
        insert_strategy(hyp_id, QStringLiteral("long_short"), QStringLiteral("{\"hypothetical\":true}"),
                        QStringLiteral("active"));
        const qint64 forty_days_ago = now - 40LL * 24 * 3600 * 1000;
        for (int i = 0; i < 30; ++i)
            insert_closed(hyp_id, QStringLiteral("t10_hyp_pos_%1").arg(i), 1.0, now - i * 1000,
                         forty_days_ago - i * 1000);

        // Retired: would otherwise pass every bar, but must not even appear
        // in the output -- nothing to promote about a book nobody runs.
        const QString retired_id = QStringLiteral("t10_retired_strategy");
        insert_strategy(retired_id, QStringLiteral("spot"), QStringLiteral("{}"), QStringLiteral("retired"));
        for (int i = 0; i < 30; ++i)
            insert_closed(retired_id, QStringLiteral("t10_retired_pos_%1").arg(i), 1.0, now - i * 1000,
                         forty_days_ago - i * 1000);

        int rc = -1;
        const QJsonObject out =
            json_object_from_dispatch(QStringList{"--json", "sandbox", "eligibility"}, &rc);
        QCOMPARE(rc, 0);

        bool found_thin = false, found_hyp = false, found_retired = false;
        for (const QJsonValue& v : out.value("eligibility").toArray()) {
            const QJsonObject row = v.toObject();
            const QString id = row.value("strategy_id").toString();
            if (id == thin_id) {
                found_thin = true;
                QVERIFY(!row.value("eligible").toBool());
                QVERIFY2(!row.value("blockers").toArray().isEmpty(), "thin sample must carry blockers");
            } else if (id == hyp_id) {
                found_hyp = true;
                QVERIFY(!row.value("eligible").toBool());
                const QJsonArray blockers = row.value("blockers").toArray();
                QCOMPARE(blockers.size(), 1);
                QCOMPARE(blockers.first().toString(), QStringLiteral("hypothetical instrument"));
            } else if (id == retired_id) {
                found_retired = true;
            }
        }
        QVERIFY(found_thin);
        QVERIFY(found_hyp);
        QVERIFY2(!found_retired, "retired strategies must be skipped entirely by sandbox eligibility");

        // Text mode must not crash and must bucket into ELIGIBLE/BLOCKED.
        const QString text = capture_stdout(
            [&]() { return dispatch(QStringList{"sandbox", "eligibility"}); }, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(text.contains("BLOCKED"));

        int bad_rc = -1;
        capture_stdout([&]() {
            bad_rc = dispatch({QStringLiteral("sandbox"), QStringLiteral("eligibility"),
                              QStringLiteral("bogus-trailing-arg")});
            return bad_rc;
        });
        QCOMPARE(bad_rc, 2);
    }

    void sandbox_eligibility_reports_eligible_for_a_fully_qualifying_book() {
        sandbox_test_home();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 forty_days_ago = now - 40LL * 24 * 3600 * 1000;

        const QString id = QStringLiteral("t10_qualifying_strategy");
        auto r = Database::instance().execute(
            "INSERT INTO sandbox_strategy (strategy_id, kind, symbols, params_json, status, created_at,"
            " notes) VALUES (?,?,?,?,?,?,?)",
            {id, QStringLiteral("spot"), QStringLiteral("BTC-USD"), QStringLiteral("{}"),
             QStringLiteral("active"), QDateTime::currentMSecsSinceEpoch(), QStringLiteral("")});
        QVERIFY2(r.is_ok(), r.is_err() ? r.error().c_str() : "");

        // 40 resolved closes, net positive, negligible drawdown, zero
        // degraded, spanning 40 days of history -- clears every bar.
        for (int i = 0; i < 40; ++i) {
            const double pnl = (i % 4 == 0) ? -1.0 : 1.0; // net positive, small drawdown.
            auto pos = Database::instance().execute(
                "INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side,"
                " hypothetical, qty, limit_price, expires_at, state, opened_at, closed_at, entry_fee,"
                " exit_fee, realized_pnl, close_reason, data_quality, notional_usd, created_at)"
                " VALUES (?,?,?,?,'buy',0,1.0,100.0,?,'closed',?,?,0,0,?,'target','ok',10.0,?)",
                {QStringLiteral("t10_qual_pos_%1").arg(i), id, QStringLiteral("t10_qual_dec_%1").arg(i),
                 QStringLiteral("BTC-USD"), now - i * 1000, now - i * 1000, now - i * 1000, pnl,
                 forty_days_ago - i * 1000});
            QVERIFY2(pos.is_ok(), pos.is_err() ? pos.error().c_str() : "");
        }

        int rc = -1;
        const QJsonObject out =
            json_object_from_dispatch(QStringList{"--json", "sandbox", "eligibility"}, &rc);
        QCOMPARE(rc, 0);
        bool found = false;
        for (const QJsonValue& v : out.value("eligibility").toArray()) {
            const QJsonObject row = v.toObject();
            if (row.value("strategy_id").toString() == id) {
                found = true;
                QVERIFY2(row.value("eligible").toBool(),
                        qUtf8Printable(QJsonDocument(row.value("blockers").toArray()).toJson()));
                QVERIFY(row.value("blockers").toArray().isEmpty());
                QVERIFY(row.value("active_days").toInt() >= 28);
                QCOMPARE(row.value("resolved").toInt(), 40);
            }
        }
        QVERIFY(found);

        const QString text = capture_stdout(
            [&]() { return dispatch(QStringList{"sandbox", "eligibility"}); }, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(text.contains("ELIGIBLE"));
    }
};
QTEST_MAIN(TstCommandDispatch)
#include "tst_command_dispatch.moc"
