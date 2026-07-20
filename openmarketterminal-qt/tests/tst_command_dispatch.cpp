#include <QtTest>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTemporaryDir>
#include <functional>
#include <unistd.h>
#include "cli/CommandDispatch.h"
#include "cli/BridgeDiscovery.h"
#include "cli/automation/AutomationState.h"
#include "services/edge_radar/AdvisoryProtocol.h"
#include "storage/sqlite/Database.h"
#include "storage/repositories/AiHandlerRepository.h"
#include "storage/repositories/SettingsRepository.h"
using namespace openmarketterminal::cli;
using openmarketterminal::AiHandler;
using openmarketterminal::AiHandlerRepository;
using openmarketterminal::Database;

namespace openmarketterminal::cli {
// Defined in ServeCommand.cpp (compiled into this test binary); declared here
// to unit-test the lock-contention return path of the daemon's job updater
// that launch_scheduled_job's double-launch guard depends on.
bool update_job_by_id(const QString& profile, const QString& id,
                      const std::function<void(QJsonObject&)>& fn, int lock_timeout_ms);
}

// assess() now expires edge_decision_journal rows older than 24h
// (wall-clock) -- seeds must be fresh, `now`-relative timestamps rather than
// tiny fixed values.
static qint64 recent_ms(qint64 back = 60000) { return QDateTime::currentMSecsSinceEpoch() - back; }

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

// `ai screen` (Task 2) emits a top-level JSON ARRAY (screen_to_json), not an
// object -- json_object_from_dispatch's QJsonDocument::isObject() guard would
// silently discard it, so parse the raw stdout as an array instead.
static QJsonArray json_array_from_dispatch(const QStringList& args, int* rc_out = nullptr) {
    int rc = -1;
    const QString out = capture_stdout([&]() { return dispatch(args); }, &rc);
    if (rc_out)
        *rc_out = rc;
    const QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8());
    return doc.isArray() ? doc.array() : QJsonArray{};
}

static QStringList json_strings(const QJsonArray& arr) {
    QStringList out;
    for (const QJsonValue& v : arr)
        out << v.toString();
    return out;
}

// Recursive key-name scan used by the advisory-firewall leak test: `key` must
// not appear ANYWHERE in `value`, at any nesting depth, inside any object
// (arrays are walked too, in case a forbidden field ever ends up inside an
// array of objects). This is deliberately name-based, not value-based --
// the firewall's job is to guarantee a forbidden field is never even
// present, regardless of what it would contain.
static bool json_contains_key_deep(const QJsonValue& value, const QString& key) {
    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        if (obj.contains(key))
            return true;
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            if (json_contains_key_deep(it.value(), key))
                return true;
        }
        return false;
    }
    if (value.isArray()) {
        for (const QJsonValue& v : value.toArray()) {
            if (json_contains_key_deep(v, key))
                return true;
        }
        return false;
    }
    return false;
}

static bool json_contains_key_deep(const QJsonObject& obj, const QString& key) {
    return json_contains_key_deep(QJsonValue(obj), key);
}

// ai ctx decision-packet Task 3 read-only invariant helpers -- fingerprint
// EVERY cli.* settings row (key|value|updated_at) so a test can assert `ai
// ctx` mutates none of them (mirrors the shape of the kalshi gate rows
// exercised above, generalized to "all of them" rather than one named key).
// Also used by the `ai handler status` gate-immutability test to detect any
// write it might make: a value change, a stray new row, OR a same-value
// INSERT-OR-REPLACE that re-stamps updated_at (the settings table carries
// `updated_at TEXT DEFAULT (datetime('now'))`).
static QStringList cli_settings_fingerprint() {
    QStringList out;
    auto rows = Database::instance().execute(
        "SELECT key, value, updated_at FROM settings WHERE key LIKE 'cli.%' ORDER BY key");
    if (rows.is_ok()) {
        QSqlQuery& q = rows.value();
        while (q.next()) {
            out << q.value(0).toString() + QStringLiteral("|") + q.value(1).toString() +
                       QStringLiteral("|") + q.value(2).toString();
        }
    }
    return out;
}

static int table_row_count(const QString& table) {
    auto rows = Database::instance().execute(QStringLiteral("SELECT COUNT(*) FROM ") + table);
    if (rows.is_ok() && rows.value().next())
        return rows.value().value(0).toInt();
    return -1;
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
    void kalshi_execution_ledger_is_durable_and_fill_idempotent() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute("DELETE FROM kalshi_live_fills").is_ok());
        QVERIFY(Database::instance().execute("DELETE FROM kalshi_live_orders").is_ok());
        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        QVERIFY(Database::instance().execute(
            "INSERT INTO kalshi_live_orders(client_order_id,order_id,market_id,asset_id,action,outcome,state,"
            "requested_count,filled_count,remaining_count,limit_price,created_at,updated_at) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
            {"client-1", "order-1", "KXTEST", "KXTEST:yes", "buy", "YES", "partially_filled",
             10.0, 4.0, 6.0, 0.42, now, now}).is_ok());
        const auto insert_fill = [&] {
            return Database::instance().execute(
                "INSERT OR IGNORE INTO kalshi_live_fills(fill_key,order_id,client_order_id,market_id,asset_id,"
                "action,outcome,count,price,received_ts_ms,source) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                {"fill-1", "order-1", "client-1", "KXTEST", "KXTEST:yes", "buy", "YES",
                 4.0, 0.41, QDateTime::currentMSecsSinceEpoch(), "test"});
        };
        QVERIFY(insert_fill().is_ok());
        QVERIFY(insert_fill().is_ok());
        QCOMPARE(table_row_count(QStringLiteral("kalshi_live_fills")), 1);
    }

    void kalshi_cancel_orders_empty_scope_is_safe_without_credentials() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute("DELETE FROM kalshi_live_orders").is_ok());
        int rc = -1;
        const QJsonObject out = json_object_from_dispatch(
            {"--json", "--headless", "kalshi", "auto", "cancel-orders", "--yes"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(out.value("requested").toInt(), 0);
        QVERIFY(out.value("verified").toBool());
    }

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
            "notebook", "strategy", "serve", "daemon", "control", "mission",
        };
        for (const QString& topic : topics) {
            QCOMPARE(dispatch(QStringList{"help", topic}), 0);
            QCOMPARE(dispatch(QStringList{"--help", topic}), 0);
        }
    }
    void control_manifest_is_machine_readable_and_secrets_free() {
        int rc = -1;
        const QJsonObject manifest = json_object_from_dispatch(
            QStringList{"--json", "--headless", "control", "manifest"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(manifest.value("schema_version").toInt(), 1);
        QCOMPARE(manifest.value("name").toString(), QStringLiteral("openterminal-control-plane"));
        const QJsonArray capabilities = manifest.value("capabilities").toArray();
        QVERIFY(capabilities.size() >= 6);

        bool has_snapshot = false;
        bool has_trading_brief = false;
        bool has_guarded_prediction = false;
        for (const QJsonValue& value : capabilities) {
            const QJsonObject capability = value.toObject();
            has_snapshot = has_snapshot || capability.value("id").toString() == QStringLiteral("observe.snapshot");
            has_trading_brief = has_trading_brief ||
                               capability.value("id").toString() == QStringLiteral("observe.trading_brief");
            has_guarded_prediction = has_guarded_prediction ||
                (capability.value("id").toString() == QStringLiteral("execute.prediction") &&
                 capability.value("mode").toString() == QStringLiteral("guarded-live"));
        }
        QVERIFY(has_snapshot);
        QVERIFY(has_trading_brief);
        QVERIFY(has_guarded_prediction);
        bool has_external_context = false;
        for (const QJsonValue& value : manifest.value("capabilities").toArray()) {
            has_external_context = has_external_context ||
                                   value.toObject().value("id").toString() == QStringLiteral("observe.external_context");
        }
        QVERIFY(has_external_context);
        const QByteArray serialized = QJsonDocument(manifest).toJson(QJsonDocument::Compact).toLower();
        QVERIFY(!serialized.contains("api_key"));
        QVERIFY(!serialized.contains("private_key"));
        QVERIFY(!serialized.contains("secret_value"));
    }
    void mission_protocol_is_scoped_machine_readable_and_read_only() {
        sandbox_test_home();
        const qint64 now = recent_ms(1000);
        QVERIFY(Database::instance().execute(
            "INSERT INTO edge_decision_journal (id,created_at,updated_at,venue,symbol,horizon,market_id,question,"
            "direction,side,call,gate,market_probability,model_probability,raw_edge,edge_after_cost,spread_cost,"
            "fee_cost,liquidity_score,confidence,seconds_left,data_status,freshness_json,features_json,reasons,source) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("mission-prediction"), now, now, QStringLiteral("kalshi"), QStringLiteral("BTC-USD"),
             QStringLiteral("1h"), QStringLiteral("KXMISSION"), QStringLiteral("BTC above test"),
             QStringLiteral("above"), QStringLiteral("yes"), QStringLiteral("WATCH"), QStringLiteral("hold"),
             0.50, 0.52, 0.02, 0.01, 0.005, 0.002, 4.0, 0.7, 120,
             QStringLiteral("watch"), QStringLiteral("{}"),
             QStringLiteral("{\"reference_price\":64000,\"quote_observed_at_ms\":\"%1\"}").arg(now),
             QStringLiteral("mission test"), QStringLiteral("kalshi auto-plan")}).is_ok());

        const QStringList settings_before = cli_settings_fingerprint();
        const int drafts_before = table_row_count(QStringLiteral("order_drafts"));
        int rc = -1;
        const QJsonObject manifest = json_object_from_dispatch(
            QStringList{"--json", "--headless", "mission", "manifest"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(manifest.value("protocol").toString(), QStringLiteral("openterminal-mission"));

        const QJsonObject context = json_object_from_dispatch(
            QStringList{"--json", "--headless", "mission", "context", "--symbol", "BTC-USD", "--market", "prediction", "--limit", "1"}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(context.value("read_only").toBool());
        QCOMPARE(context.value("market").toString(), QStringLiteral("prediction"));
        QVERIFY(context.value("fast_local_execution_facts").toObject().value("execution_relevant").toBool());
        QVERIFY(!context.value("slow_advisory_research").toObject().value("executable").toBool());

        const QJsonObject plan = json_object_from_dispatch(
            QStringList{"--json", "--headless", "mission", "plan", "--symbol", "BTC-USD", "--market", "prediction", "--limit", "1"}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(plan.value("read_only").toBool());
        QVERIFY(plan.value("execution").toObject().value("requires_human_arm").toBool());

        const QJsonObject explain = json_object_from_dispatch(
            QStringList{"--json", "--headless", "mission", "explain", "mission-prediction"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(explain.value("journal").toObject().value("decision_id").toString(), QStringLiteral("mission-prediction"));
        QVERIFY(explain.value("rule").toString().contains(QStringLiteral("decision-time evidence")));

        const QString missing_scope = capture_stdout([&]() {
            return dispatch(QStringList{"--headless", "mission", "context", "--symbol", "BTC-USD"});
        }, &rc);
        QCOMPARE(rc, 2);
        QVERIFY(missing_scope.isEmpty());
        QCOMPARE(cli_settings_fingerprint(), settings_before);
        QCOMPARE(table_row_count(QStringLiteral("order_drafts")), drafts_before);
        QVERIFY(Database::instance().execute(
            "DELETE FROM edge_decision_journal WHERE id='mission-prediction'").is_ok());
    }
    void control_prediction_pulse_is_compact_and_read_only() {
        sandbox_test_home();
        const qint64 now = recent_ms(1000);
        const QString features = QStringLiteral(
            "{\"signal\":{\"kind\":\"above\",\"floor\":64000,\"spot\":64100,"
            "\"yes_bid\":0.54,\"yes_ask\":0.56,\"yes_depth\":12,\"no_bid\":0.43,"
            "\"no_ask\":0.45,\"no_depth\":10,\"selected_bid\":0.54,\"selected_ask\":0.56,"
            "\"selected_depth\":12,\"quote_observed_at_ms\":\"%1\",\"context_up_probability\":0.61,"
            "\"calibration_samples\":31},\"micro_evidence\":{\"eligible\":true,\"tier\":\"calibrated\"},"
            "\"news_context\":{\"as_of_ms\":\"%1\",\"verdict\":\"UP\",\"score\":2.5,"
            "\"confidence\":0.7,\"distinct_sources\":2,\"catalysts\":[\"ETF flow\"],\"stories\":["
            "{\"source\":\"Reuters\",\"headline\":\"BTC demand rises\",\"summary\":\"Test context\","
            "\"direction\":\"UP\",\"published_ts\":\"%1\"}]}}")
            .arg(now);
        QVERIFY(Database::instance().execute(
            "INSERT INTO edge_decision_journal (id,created_at,updated_at,venue,symbol,horizon,market_id,question,"
            "direction,side,call,gate,market_probability,model_probability,raw_edge,edge_after_cost,spread_cost,"
            "fee_cost,liquidity_score,confidence,seconds_left,data_status,freshness_json,features_json,reasons,source) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("pulse-yes"), now, now, QStringLiteral("kalshi"), QStringLiteral("BTC-USD"),
             QStringLiteral("1h"), QStringLiteral("KXPULSE-YES"), QStringLiteral("BTC above"),
             QStringLiteral("above"), QStringLiteral("yes"), QStringLiteral("MICRO EVIDENCE LEG"),
             QStringLiteral("pass"), 0.56, 0.63, 0.07, 0.05, 0.02, 0.01, 12.0, 0.8, 900,
             QStringLiteral("trade_candidate"), QStringLiteral("{}"), features,
             QStringLiteral("test pulse"), QStringLiteral("kalshi auto-plan")}).is_ok());

        const QStringList settings_before = cli_settings_fingerprint();
        const int drafts_before = table_row_count(QStringLiteral("order_drafts"));
        int rc = -1;
        const QJsonObject out = json_object_from_dispatch(
            QStringList{"--json", "--headless", "control", "pulse", "prediction", "--limit", "1"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(out.value("lane").toString(), QStringLiteral("prediction"));
        QVERIFY(out.value("read_only").toBool());
        QCOMPARE(out.value("external_context").toObject().value("mode").toString(),
                 QStringLiteral("cached_external_research"));
        const QJsonArray markets = out.value("markets").toArray();
        QCOMPARE(markets.size(), 1);
        const QJsonObject market = markets.first().toObject();
        QCOMPARE(market.value("market_id").toString(), QStringLiteral("KXPULSE-YES"));
        QCOMPARE(market.value("yes").toObject().value("ask").toDouble(), 0.56);
        QCOMPARE(market.value("no").toObject().value("ask").toDouble(), 0.45);
        QCOMPARE(market.value("selected_quote").toObject().value("depth").toDouble(), 12.0);
        const QJsonObject external = market.value("external_context").toObject();
        QVERIFY(external.value("advisory_only").toBool());
        QCOMPARE(external.value("role").toString(), QStringLiteral("weighted_context"));
        QVERIFY(external.value("fresh_for_horizon").toBool());
        QCOMPARE(external.value("headlines").toArray().first().toObject().value("source").toString(),
                 QStringLiteral("Reuters"));
        QCOMPARE(out.value("external_context_policy").toObject().value("15m").toString(),
                 QStringLiteral("context only; maximum 10% advisory weight when <=5m old"));
        QCOMPARE(cli_settings_fingerprint(), settings_before);
        QCOMPARE(table_row_count(QStringLiteral("order_drafts")), drafts_before);
        // This binary deliberately shares one profile-local sandbox DB across
        // its slots. Remove the synthetic pulse row so later sandbox-cycle
        // tests still exercise an actually empty journal.
        QVERIFY(Database::instance().execute(
            "DELETE FROM edge_decision_journal WHERE id='pulse-yes'").is_ok());
    }
    void control_trading_brief_is_compact_and_read_only() {
        sandbox_test_home();
        const qint64 now = recent_ms(1000);
        QVERIFY(Database::instance().execute(
            "INSERT INTO edge_decision_journal (id,created_at,updated_at,venue,symbol,horizon,market_id,question,"
            "direction,side,call,gate,market_probability,model_probability,raw_edge,edge_after_cost,spread_cost,"
            "fee_cost,liquidity_score,confidence,seconds_left,data_status,freshness_json,features_json,reasons,source) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("brief-final-window"), now, now, QStringLiteral("kalshi"), QStringLiteral("BTC-USD"),
             QStringLiteral("15m"), QStringLiteral("KXBRIEF-60"), QStringLiteral("BTC final minute"),
             QStringLiteral("above"), QStringLiteral("yes"), QStringLiteral("WATCH"), QStringLiteral("hold"),
             0.50, 0.52, 0.02, 0.01, 0.005, 0.002, 4.0, 0.7, 45,
             QStringLiteral("watch"), QStringLiteral("{}"), QStringLiteral("{}"),
             QStringLiteral("test brief"), QStringLiteral("kalshi auto-plan")}).is_ok());

        const QStringList settings_before = cli_settings_fingerprint();
        const int drafts_before = table_row_count(QStringLiteral("order_drafts"));
        int rc = -1;
        const QJsonObject out = json_object_from_dispatch(
            QStringList{"--json", "--headless", "control", "brief", "--symbol", "BTC-USD", "--limit", "1"}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(out.value("read_only").toBool());
        QVERIFY(out.value("data_contract").toString().contains(QStringLiteral("Freshness")));
        QVERIFY(out.value("tournament").toObject().value("paper_only").toBool());
        const QJsonObject lanes = out.value("lanes").toObject();
        const QJsonObject crypto = lanes.value("crypto").toObject();
        QVERIFY(crypto.contains(QStringLiteral("feed_quality")));
        QCOMPARE(crypto.value("symbol").toString(), QStringLiteral("BTC-USD"));
        const QJsonObject prediction = lanes.value("prediction").toObject();
        QCOMPARE(prediction.value("candidate_count").toInt(), 1);
        QCOMPARE(prediction.value("settlement_bands").toObject().value("final_60s").toInt(), 1);
        QVERIFY(prediction.value("mandate").toObject().contains(QStringLiteral("kill_switch")));
        QCOMPARE(out.value("timeline").toArray().size(), 3);
        QCOMPARE(cli_settings_fingerprint(), settings_before);
        QCOMPARE(table_row_count(QStringLiteral("order_drafts")), drafts_before);
        QVERIFY(Database::instance().execute(
            "DELETE FROM edge_decision_journal WHERE id='brief-final-window'").is_ok());
    }
    void control_plan_watch_and_audit_are_read_only() {
        sandbox_test_home();
        const qint64 now = recent_ms(1000);
        QVERIFY(Database::instance().execute(
            "INSERT INTO edge_decision_journal (id,created_at,updated_at,venue,symbol,horizon,market_id,question,"
            "direction,side,call,gate,market_probability,model_probability,raw_edge,edge_after_cost,spread_cost,"
            "fee_cost,liquidity_score,confidence,seconds_left,data_status,freshness_json,features_json,reasons,source) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("control-plan-audit"), now, now, QStringLiteral("kalshi"), QStringLiteral("BTC-USD"),
             QStringLiteral("1h"), QStringLiteral("KXPLAN"), QStringLiteral("BTC plan test"),
             QStringLiteral("above"), QStringLiteral("yes"), QStringLiteral("WATCH"), QStringLiteral("hold"),
             0.50, 0.52, 0.02, 0.01, 0.005, 0.002, 4.0, 0.7, 120,
             QStringLiteral("watch"), QStringLiteral("{}"), QStringLiteral("{}"),
             QStringLiteral("control test"), QStringLiteral("kalshi auto-plan")}).is_ok());
        const int drafts_before = table_row_count(QStringLiteral("order_drafts"));
        int rc = -1;
        const QJsonObject plan = json_object_from_dispatch(
            QStringList{"--json", "--headless", "control", "plan", "--symbol", "BTC-USD", "--limit", "1"}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(plan.value("read_only").toBool());
        QVERIFY(plan.value("execution").toObject().value("requires_human_gate").toBool());
        const QJsonObject audit = json_object_from_dispatch(
            QStringList{"--json", "--headless", "control", "audit", "--limit", "1"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(audit.value("decision_count").toInt(), 1);
        QCOMPARE(audit.value("decisions").toArray().first().toObject().value("decision_id").toString(),
                 QStringLiteral("control-plan-audit"));
        const QString watch = capture_stdout([&]() {
            return dispatch(QStringList{"--headless", "control", "watch", "--iterations", "1", "--interval-sec", "1"});
        });
        QVERIFY(watch.contains(QStringLiteral("NO_ACTION")) || watch.contains(QStringLiteral("REVIEW_")));
        QCOMPARE(table_row_count(QStringLiteral("order_drafts")), drafts_before);
        QVERIFY(Database::instance().execute(
            "DELETE FROM edge_decision_journal WHERE id='control-plan-audit'").is_ok());
    }
    void control_snapshot_includes_symbol_specific_spot_research_read_only() {
        sandbox_test_home();
        const qint64 now_s = QDateTime::currentSecsSinceEpoch();
        QVERIFY(Database::instance().execute(
            "INSERT INTO news_articles (id,headline,summary,source,region,category,link,sort_ts,priority,"
            "sentiment,impact,tickers,tier,lang,threat_level,threat_cat,threat_conf,source_flag) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("control-spot-news"), QStringLiteral("Bitcoin demand test"),
             QStringLiteral("A local advisory-only test article."), QStringLiteral("TestWire"),
             QStringLiteral("US"), QStringLiteral("market"), QStringLiteral("https://example.test/btc"), now_s,
             QStringLiteral("BREAKING"), QStringLiteral("BULLISH"), QStringLiteral("HIGH"),
             QStringLiteral("[\"BTC\"]"), 1, QStringLiteral("en"), QStringLiteral("INFO"),
             QString(), 0.0, 0}).is_ok());

        const QStringList settings_before = cli_settings_fingerprint();
        const int drafts_before = table_row_count(QStringLiteral("order_drafts"));
        int rc = -1;
        const QJsonObject out = json_object_from_dispatch(
            QStringList{"--json", "--headless", "control", "snapshot", "--symbol", "BTC-USD"}, &rc);
        QCOMPARE(rc, 0);
        const QJsonObject external = out.value("crypto").toObject().value("external_context").toObject();
        QVERIFY(external.value("advisory_only").toBool());
        QCOMPARE(external.value("symbol").toString(), QStringLiteral("BTC-USD"));
        QVERIFY(external.value("article_count").toInt() >= 1);
        const QJsonArray headlines = external.value("headlines").toArray();
        QVERIFY(!headlines.isEmpty());
        QCOMPARE(headlines.first().toObject().value("source").toString(), QStringLiteral("TestWire"));
        QCOMPARE(external.value("review_action").toString(), QStringLiteral("BUY_CONTEXT_ONLY"));
        QCOMPARE(cli_settings_fingerprint(), settings_before);
        QCOMPARE(table_row_count(QStringLiteral("order_drafts")), drafts_before);
        QVERIFY(Database::instance().execute(
            "DELETE FROM news_articles WHERE id='control-spot-news'").is_ok());
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
    // Proof protocol v2 plus the producer-backed honest lane grid: 37 rows,
    // spot 1h/4h/1d, 18 Kalshi horizon/cohort/exit-policy books, long_short,
    // chronos2/1h/1d/equity, and the two crypto maker_decisions lanes
    // (btc5m/chronos2_5m are retired). 'spot' and 'kalshi' are each THREE rows of one kind (distinct
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
        QCOMPARE(seeded_ids.size(), 37);
        QCOMPARE(seeded.value("retired_stale").toInt(-1), 0);

        QJsonObject listed = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "list"}, &rc);
        QCOMPARE(rc, 0);
        const QJsonArray rows = listed.value("strategies").toArray();
        QCOMPARE(rows.size(), 37);

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
        QCOMPARE(active_after_pause.value("strategies").toArray().size(), 36);

        // resume flips it back.
        const QJsonObject resumed = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "resume", spot_id}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(resumed.value("status").toString(), QString("active"));
        QJsonObject active_after_resume = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "list", "--status", "active"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(active_after_resume.value("strategies").toArray().size(), 37);

        // retire is permanent-by-convention here but still just a status flip.
        const QJsonObject retired = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "retire", long_short_id}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(retired.value("status").toString(), QString("retired"));
        QJsonObject active_after_retire = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "list", "--status", "active"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(active_after_retire.value("strategies").toArray().size(), 36);

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
        bool has_local_model_publisher = false;
        bool has_spot_swing_1h = false;
        bool has_spot_swing_4h = false;
        bool has_spot_swing_1d = false;
        bool has_stale_crypto_universe_60 = false;
        bool has_btc5m_producer = false;
        bool has_legacy_kalshi_auto_plan = false;
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
                                             "--publish", "--journal", "--min-journal-edge-bps", "15"}) {
                has_chronos_15m = true;
            } else if (command == QStringList{"edge", "chronos2", "forecast", "BTC-USD", "--horizon", "1h",
                                             "--publish", "--journal", "--min-journal-edge-bps", "35"}) {
                has_chronos_1h = true;
            } else if (command == QStringList{"edge", "chronos2", "forecast", "BTC-USD", "--horizon", "1d",
                                             "--publish", "--journal", "--min-journal-edge-bps", "75"}) {
                has_chronos_1d = true;
            } else if (command == QStringList{"edge", "publish-horizons", "--symbol", "BTC", "--market-prob",
                                               "0.50", "--spread", "0.03", "--min-samples", "30"}) {
                has_local_model_publisher = true;
                QCOMPARE(job.value("interval_sec").toInt(), 60);
                QCOMPARE(job.value("timeout_sec").toInt(), 30);
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
                has_legacy_kalshi_auto_plan = true;
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
                QCOMPARE(job.value("interval_sec").toInt(), 45);
                QCOMPARE(job.value("timeout_sec").toInt(), 45);
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
        QVERIFY2(has_local_model_publisher,
                 "install-jobs must keep timestamped local BTC model outputs fresh for all horizons");
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
        QVERIFY2(!has_legacy_kalshi_auto_plan,
                 "the persistent Kalshi WebSocket engine owns decision and recovery work; "
                 "install-jobs must not recreate scheduled REST auto-plan jobs");
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

    void kalshi_flow_and_snapshot_explain_legacy_daemon_evidence() {
        const QString home = sandbox_test_home();
        const QString path = QDir(home).filePath(
            QStringLiteral("kalshi-evidence/kalshi-ws-books.json"));
        QFile evidence(path);
        QVERIFY(evidence.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QJsonObject legacy{{"schema", 1},
                                  {"updated_at_ms", QStringLiteral("2026-07-19T12:00:00.000Z")},
                                  {"books", QJsonObject{{"KXBTC-TEST:yes", QJsonObject{}},
                                                        {"KXBTC-TEST:no", QJsonObject{}}}}};
        QCOMPARE(evidence.write(QJsonDocument(legacy).toJson(QJsonDocument::Compact)),
                 QJsonDocument(legacy).toJson(QJsonDocument::Compact).size());
        evidence.close();

        int rc = -1;
        const QJsonObject flow = json_object_from_dispatch(
            {QStringLiteral("--json"), QStringLiteral("kalshi"), QStringLiteral("auto"),
             QStringLiteral("flow")}, &rc);
        QCOMPARE(rc, 5);
        QCOMPARE(flow.value(QStringLiteral("schema")).toInt(), 1);
        QCOMPARE(flow.value(QStringLiteral("required_schema")).toInt(), 3);
        QVERIFY(flow.value(QStringLiteral("reason")).toString().contains(
            QStringLiteral("upgrade to schema 3")));
        QCOMPARE(json_strings(flow.value(QStringLiteral("available_tickers")).toArray()),
                 QStringList{QStringLiteral("KXBTC-TEST")});

        const QJsonObject snapshot = json_object_from_dispatch(
            {QStringLiteral("--json"), QStringLiteral("kalshi"), QStringLiteral("auto"),
             QStringLiteral("snapshot"), QStringLiteral("--ticker"), QStringLiteral("KXBTC-TEST")},
            &rc);
        QCOMPARE(rc, 5);
        QVERIFY(snapshot.value(QStringLiteral("reason")).toString().contains(
            QStringLiteral("upgrade to schema 3")));
        QCOMPARE(snapshot.value(QStringLiteral("evidence")).toObject()
                     .value(QStringLiteral("schema")).toInt(), 1);
    }

    // Reconcile regression: when a producer drops OUT of sandbox_job_specs(),
    // install-jobs must disable its stale managed job rather than leaving it
    // enabled forever. The retired Kalshi REST watchdog is a concrete case:
    // the persistent WebSocket engine owns that work now.
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

        // Inject the old REST watchdog as if it predated this install. Its
        // sandbox_job key is intentionally absent from sandbox_job_specs().
        const QString now = QStringLiteral("2020-01-01T00:00:00Z");
        QJsonObject stale{
            {"id", QStringLiteral("job_legacy_kalshi_rest")},
            {"name", QStringLiteral("Kalshi auto recovery watchdog")},
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
            {"sandbox_job", QStringLiteral("kalshi-auto-recovery-watchdog")},
            {"description", QStringLiteral("retired REST Kalshi auto-plan watchdog")},
            {"command", QJsonArray{QStringLiteral("kalshi"), QStringLiteral("auto"),
                                    QStringLiteral("run")}},
            {"spec", QJsonObject{{"command", QJsonArray{QStringLiteral("kalshi"), QStringLiteral("auto"),
                                                         QStringLiteral("run")}}}}};
        jobs.append(stale);
        QJsonObject out_doc = doc.object();
        out_doc["jobs"] = jobs;
        QVERIFY2(jobs_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text),
                 "must be able to rewrite jobs.json to inject the stale job");
        jobs_file.write(QJsonDocument(out_doc).toJson(QJsonDocument::Compact));
        jobs_file.close();

        // Run install-jobs again -- it must reconcile: disable the old REST
        // watchdog while leaving current spec jobs (e.g. 'tick') enabled.
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
        bool found_legacy_watchdog = false;
        bool legacy_watchdog_disabled = false;
        bool found_tick_enabled = false;
        QJsonArray jobs_without_obsolete;
        for (const QJsonValue& v : doc2.object().value("jobs").toArray()) {
            const QJsonObject job = v.toObject();
            if (job.value("managed_by").toString() == QStringLiteral("strategy-sandbox") &&
                job.value("sandbox_job").toString() == QStringLiteral("kalshi-auto-recovery-watchdog")) {
                found_legacy_watchdog = true;
                legacy_watchdog_disabled = !job.value("enabled").toBool();
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

        QVERIFY2(found_legacy_watchdog,
                 "the retired REST watchdog must remain recorded in jobs.json as disabled, not deleted");
        QVERIFY2(legacy_watchdog_disabled,
                 "install-jobs must disable the retired Kalshi REST watchdog");
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

    void sandbox_lane_significance_uses_only_honest_ok_quality_rows() {
        sandbox_test_home();
        const QString strategy_id = QStringLiteral("t9_lane_significance_strategy");
        const QString params = QStringLiteral(
            "{\"venue\":\"kraken_pro\",\"liquidity\":\"taker\",\"half_spread_bps\":5}");
        auto strategy = Database::instance().execute(
            "INSERT INTO sandbox_strategy (strategy_id, kind, symbols, params_json, status, created_at,"
            " notes) VALUES (?,?,?,?,?,?,?)",
            {strategy_id, QStringLiteral("scalp"), QStringLiteral("BTC-USD"), params,
             QStringLiteral("active"), QDateTime::currentMSecsSinceEpoch(), QStringLiteral("")});
        QVERIFY2(strategy.is_ok(), strategy.is_err() ? strategy.error().c_str() : "");

        const qint64 day_ms = 24LL * 60 * 60 * 1000;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        auto insert_closed = [&](const QString& id, double pnl, qint64 closed_at,
                                 const QString& quality) {
            auto r = Database::instance().execute(
                "INSERT INTO sandbox_position (position_id, strategy_id, decision_id, symbol, side,"
                " hypothetical, qty, limit_price, expires_at, state, opened_at, closed_at, entry_fee,"
                " exit_fee, realized_pnl, close_reason, data_quality, notional_usd, created_at)"
                " VALUES (?,?,?,?,'buy',0,1.0,100.0,?,'closed',?,?,0,0,?,'target',?,10.0,?)",
                {id, strategy_id, id + QStringLiteral("-dec"), QStringLiteral("BTC-USD"), closed_at,
                 closed_at, closed_at, pnl, quality, closed_at});
            QVERIFY2(r.is_ok(), r.is_err() ? r.error().c_str() : "");
        };
        insert_closed(QStringLiteral("t9_lane_ok_1"), 1.25, now - 2 * day_ms, QStringLiteral("ok"));
        insert_closed(QStringLiteral("t9_lane_ok_2"), -0.25, now - day_ms, QStringLiteral("ok"));
        insert_closed(QStringLiteral("t9_lane_degraded"), 99.0, now, QStringLiteral("degraded"));
        insert_closed(QStringLiteral("t9_lane_unknown"), 99.0, now, QStringLiteral("unknown"));

        int rc = -1;
        const QJsonObject out = json_object_from_dispatch(
            QStringList{"--json", "sandbox", "lane-significance"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(out.value("minimum_sessions").toInt(), 20);
        bool found = false;
        for (const QJsonValue& value : out.value("lanes").toArray()) {
            const QJsonObject lane = value.toObject();
            if (lane.value("lane").toString() != QStringLiteral("scalp/kraken_pro/taker"))
                continue;
            found = true;
            QCOMPARE(lane.value("trades").toInt(), 2);
            QCOMPARE(lane.value("sessions").toInt(), 2);
            QVERIFY(qAbs(lane.value("net_pnl").toDouble() - 1.0) < 1e-9);
            QCOMPARE(lane.value("verdict").toString(), QStringLiteral("insufficient"));
        }
        QVERIFY2(found, "lane-significance must report the honest fixture lane");
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

    // ai ctx decision-packet Task 3 -- `ai ctx <symbol> --json` reads the
    // latest edge_decision_journal row (seeded exactly like Task 2's
    // tst_decision_context.cpp fixture) and emits DecisionContext::to_json.
    void ai_ctx_reads_edge_and_gates_json() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute(
            "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, side, gate,"
            " market_probability, model_probability, edge_after_cost, spread_cost, fee_cost,"
            " confidence, freshness_json, source) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("cd-ctx-1"), recent_ms(), recent_ms(), QStringLiteral("CTXTEST-USD"),
             QStringLiteral("buy"), QStringLiteral("pass"), 0.40, 0.55, 8.0, 2.0, 1.0, 0.8,
             QStringLiteral("{\"freshest_age_ms\":120,\"live_sources\":3}"),
             QStringLiteral("edge crypto-recommend")}).is_ok());

        int rc = -1;
        const QJsonObject out = json_object_from_dispatch(
            QStringList{"ai", "ctx", "CTXTEST-USD", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(out.value(QStringLiteral("symbol")).toString(), QStringLiteral("CTXTEST-USD"));
        QVERIFY(out.value(QStringLiteral("has_edge_signal")).toBool());
        QVERIFY(out.contains(QStringLiteral("clears_cost")));
        QVERIFY(out.contains(QStringLiteral("freshness")));
        QCOMPARE(out.value(QStringLiteral("recommendation_hint")).toString(),
                 QStringLiteral("all gates pass"));
    }

    // No edge_decision_journal row for the symbol -> still exit 0 with a
    // graceful "no edge signal" packet (never a hard failure).
    void ai_ctx_no_data_symbol_returns_no_edge_signal() {
        sandbox_test_home();
        int rc = -1;
        const QJsonObject out = json_object_from_dispatch(
            QStringList{"ai", "ctx", "ZZZZ-USD", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(!out.value(QStringLiteral("has_edge_signal")).toBool());
        QCOMPARE(out.value(QStringLiteral("recommendation_hint")).toString(),
                 QStringLiteral("no edge signal"));
    }

    // READ-ONLY invariant: `ai ctx` must not mutate a single cli.* settings
    // row, nor insert/delete any edge_decision_journal or order_drafts row.
    // Fingerprint (key|value|updated_at) every cli.* row before and after --
    // any drift, even a no-op re-write of the same value, changes
    // updated_at and would show up here.
    void ai_ctx_is_read_only_on_gates() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute(
            "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, gate,"
            " edge_after_cost, spread_cost, fee_cost, freshness_json, source)"
            " VALUES (?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("cd-ctx-2"), recent_ms(), recent_ms(), QStringLiteral("CTXRO-USD"),
             QStringLiteral("pass"), 5.0, 1.0, 0.5,
             QStringLiteral("{\"freshest_age_ms\":100,\"live_sources\":3}"),
             QStringLiteral("x")}).is_ok());

        const QStringList before = cli_settings_fingerprint();
        const int journal_before = table_row_count(QStringLiteral("edge_decision_journal"));
        const int orders_before = table_row_count(QStringLiteral("order_drafts"));
        QVERIFY(journal_before >= 0);
        QVERIFY(orders_before >= 0);

        int rc = -1;
        json_object_from_dispatch(QStringList{"ai", "ctx", "CTXRO-USD", "--json"}, &rc);
        QCOMPARE(rc, 0);

        const QStringList after = cli_settings_fingerprint();
        const int journal_after = table_row_count(QStringLiteral("edge_decision_journal"));
        const int orders_after = table_row_count(QStringLiteral("order_drafts"));
        QCOMPARE(after, before);
        QCOMPARE(journal_after, journal_before);
        QCOMPARE(orders_after, orders_before);
    }

    // READ-ONLY invariant, post-④b: `ai ctx <symbol>` now aggregates the
    // ledger's net position for its `position` field
    // (ai_ledger::net_position_for_symbol), but must still not mutate a
    // single cli.* settings row nor write any ai_fill row while doing so.
    void ai_ctx_position_read_is_read_only() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute(
            "INSERT INTO ai_fill (id,handler,symbol,side,quantity,fill_price,fee,realized_pnl,ts,draft_id) "
            "VALUES ('ro1','h','CTXRO2-USD','buy',5,100,0,0,1000,'d')").is_ok());

        const QStringList before = cli_settings_fingerprint();
        const int fills_before = table_row_count(QStringLiteral("ai_fill"));

        int rc = -1;
        json_object_from_dispatch(QStringList{"ai", "ctx", "CTXRO2-USD", "--json"}, &rc);
        QCOMPARE(rc, 0);

        QCOMPARE(cli_settings_fingerprint(), before);                        // no cli.* gate written
        QCOMPARE(table_row_count(QStringLiteral("ai_fill")), fills_before);   // assess wrote no fill
    }

    // ai ctx floor verdict, piece ⑤b Task 3 -- `ai ctx <symbol> --json` gains
    // read-only floor_permits/floor_reason fields computed from the
    // DecisionPacket via ai_strategy::floor_verdict. A "pass" gate with
    // edge_after_cost>0 makes clears_cost="true" (DecisionContext.cpp:138),
    // so the honest-edge system positively endorses the symbol and the floor
    // permits.
    void ai_ctx_floor_permits_endorsed_symbol() {
        sandbox_test_home();
        // F2: floor_permits is now long-aware, so an endorsing-long row must
        // carry a long side ("buy") to keep permitting -- data-completion,
        // not a weakening (this row always meant to represent a LONG
        // endorsement for the ai ctx floor_permits check).
        QVERIFY(Database::instance().execute(
            QStringLiteral(
                "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, gate, side,"
                " edge_after_cost, spread_cost, fee_cost, freshness_json, source)"
                " VALUES ('flp1', %1, %1, 'FLP-USD', 'pass', 'buy', 5.0, 1.0, 0.5,"
                "         '{\"freshest_age_ms\":100,\"live_sources\":3}', 'x')").arg(recent_ms())).is_ok());
        int rc = -1;
        QJsonObject o = json_object_from_dispatch(QStringList{"ai","ctx","FLP-USD","--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(o.value("floor_permits").toBool(), true);
        QCOMPARE(o.value("floor_reason").toString(), QString());
    }

    // F2 CLI surface: a fully-endorsed edge (gate=pass/cost-clear/fresh) that
    // recommends SHORT must NOT report floor_permits=true for a (long-only)
    // enter -- the honest edge says short, `ai ctx` must say so.
    void ai_ctx_floor_reports_opposite_side() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute(
            QStringLiteral(
                "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, gate, side,"
                " edge_after_cost, spread_cost, fee_cost, freshness_json, source)"
                " VALUES ('flp4', %1, %1, 'FLPS-USD', 'pass', 'short', 5.0, 1.0, 0.5,"
                "         '{\"freshest_age_ms\":100,\"live_sources\":3}', 'x')").arg(recent_ms())).is_ok());
        int rc = -1;
        QJsonObject o = json_object_from_dispatch(QStringList{"ai","ctx","FLPS-USD","--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(o.value("floor_permits").toBool(), false);
        QVERIFY2(o.value("floor_reason").toString().contains(QStringLiteral("opposite side")),
                  qUtf8Printable(o.value("floor_reason").toString()));
    }

    // A "reject" gate never earns clears_cost="true"/gate=="pass", so the
    // floor skips with the specific "edge gate not pass" reason.
    void ai_ctx_floor_skips_rejected_symbol() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute(
            QStringLiteral(
                "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, gate,"
                " edge_after_cost, spread_cost, fee_cost, freshness_json, source)"
                " VALUES ('flp2', %1, %1, 'FLPR-USD', 'reject', 5.0, 1.0, 0.5,"
                "         '{\"freshest_age_ms\":100,\"live_sources\":3}', 'x')").arg(recent_ms())).is_ok());
        int rc = -1;
        QJsonObject o = json_object_from_dispatch(QStringList{"ai","ctx","FLPR-USD","--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(o.value("floor_permits").toBool(), false);
        QCOMPARE(o.value("floor_reason").toString(), QStringLiteral("edge gate not pass"));
    }

    // READ-ONLY invariant: computing/emitting the floor verdict must not
    // mutate a single cli.* settings row (floor_verdict is pure; assess()
    // is SELECT-only).
    void ai_ctx_floor_is_read_only() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute(
            QStringLiteral(
                "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, gate,"
                " edge_after_cost, spread_cost, fee_cost, freshness_json, source)"
                " VALUES ('flp3', %1, %1, 'FLPRO-USD', 'pass', 5.0, 1.0, 0.5,"
                "         '{\"freshest_age_ms\":100,\"live_sources\":3}', 'x')").arg(recent_ms())).is_ok());
        const QStringList before = cli_settings_fingerprint();
        int rc = -1;
        json_object_from_dispatch(QStringList{"ai","ctx","FLPRO-USD","--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(cli_settings_fingerprint(), before);
    }

    // ai ctx track record, piece ⑥ Task 1 -- `ai ctx <symbol> --json` gains a
    // read-only track_record object computed from ai_ledger::scorecard_of
    // (aggregate across all handlers, symbol-scoped). The edge row must be
    // FRESH so it isn't excluded by any later recency bound.
    void ai_ctx_emits_track_record() {
        sandbox_test_home();
        const qint64 fresh = QDateTime::currentMSecsSinceEpoch() - 60000;
        QVERIFY(Database::instance().execute(
            "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, gate,"
            " edge_after_cost, spread_cost, fee_cost, freshness_json, source)"
            " VALUES ('tr-edge', ?, ?, 'TR-USD', 'pass', 5.0, 1.0, 0.5,"
            "         '{\"freshest_age_ms\":100,\"live_sources\":3}', 'x')", {fresh, fresh}).is_ok());
        // Two closing fills under handler 'claude': one win (+100), one loss (-40).
        QVERIFY(Database::instance().execute(
            "INSERT INTO ai_fill (id,handler,symbol,side,quantity,fill_price,fee,realized_pnl,ts,draft_id) "
            "VALUES ('trf1','claude','TR-USD','sell',1,110,0,100,1000,'d'),"
            "       ('trf2','claude','TR-USD','sell',1,90,0,-40,1001,'d')").is_ok());
        int rc = -1;
        QJsonObject o = json_object_from_dispatch(QStringList{"ai","ctx","TR-USD","--json"}, &rc);
        QCOMPARE(rc, 0);
        const QJsonObject tr = o.value("track_record").toObject();
        QCOMPARE(tr.value("trades").toInt(), 2);
        QCOMPARE(tr.value("wins").toInt(), 1);
        QCOMPARE(tr.value("losses").toInt(), 1);
        QCOMPARE(tr.value("hit_rate").toDouble(), 0.5);
        QCOMPARE(tr.value("realized_total").toDouble(), 60.0);
    }

    // READ-ONLY invariant: scorecard_of is SELECT-only -- computing/emitting
    // track_record must not mutate a single cli.* settings row nor insert
    // any ai_fill row.
    void ai_ctx_track_record_read_only() {
        sandbox_test_home();
        const QStringList before = cli_settings_fingerprint();
        const int fills_before = table_row_count(QStringLiteral("ai_fill"));
        int rc = -1;
        json_object_from_dispatch(QStringList{"ai","ctx","NOTR-USD","--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(cli_settings_fingerprint(), before);
        QCOMPARE(table_row_count(QStringLiteral("ai_fill")), fills_before);
    }

    // ai screen shortlist Task 2 -- `ai screen --market crypto --json` ranks
    // the all-gates-pass candidates by edge_after_cost desc and tags each
    // with its market. Fixture mirrors tst_screener.cpp's
    // screen_filters_ranks_and_tags seed shape exactly (id, created_at,
    // updated_at, symbol, venue, side, gate, edge_after_cost, spread_cost,
    // fee_cost, freshness_json, source).
    void ai_screen_ranks_crypto_json() {
        sandbox_test_home();
        auto seed = [&](const QString& id, const QString& sym, const QString& venue,
                        const QString& gate, double edge, qint64 ts) {
            auto r = Database::instance().execute(
                "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, venue, side, gate,"
                " edge_after_cost, spread_cost, fee_cost, freshness_json, source) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                {id, ts, ts, sym, venue, QStringLiteral("buy"), gate, edge, 0.0, 0.0,
                 QStringLiteral("{\"freshest_age_ms\":100,\"live_sources\":2}"), QStringLiteral("s")});
            QVERIFY2(r.is_ok(), r.is_err() ? r.error().c_str() : "");
        };
        seed(QStringLiteral("cd-scr-1"), QStringLiteral("SCRBTC-USD"), QStringLiteral("coinbase_advanced"),
             QStringLiteral("pass"), 12.0, recent_ms());
        seed(QStringLiteral("cd-scr-2"), QStringLiteral("SCRETH-USD"), QStringLiteral("coinbase_advanced"),
             QStringLiteral("pass"), 5.0, recent_ms());
        // A crypto FAIL (excluded from the shortlist).
        seed(QStringLiteral("cd-scr-3"), QStringLiteral("SCRSOL-USD"), QStringLiteral("coinbase_advanced"),
             QStringLiteral("fail"), 9.0, recent_ms());
        // A prediction passer -- must NOT appear when --market crypto filters it out.
        seed(QStringLiteral("cd-scr-4"), QStringLiteral("SCRKXBTC-USD"), QStringLiteral("kalshi"),
             QStringLiteral("pass"), 20.0, recent_ms());

        int rc = -1;
        const QJsonArray arr = json_array_from_dispatch(
            QStringList{"ai", "screen", "--market", "crypto", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(arr.size(), 2);

        const QJsonObject first = arr.at(0).toObject();
        const QJsonObject second = arr.at(1).toObject();
        QCOMPARE(first.value(QStringLiteral("symbol")).toString(), QStringLiteral("SCRBTC-USD"));
        QCOMPARE(first.value(QStringLiteral("market")).toString(), QStringLiteral("crypto"));
        QVERIFY(first.contains(QStringLiteral("edge_after_cost")));
        QCOMPARE(first.value(QStringLiteral("recommendation_hint")).toString(),
                 QStringLiteral("all gates pass"));
        QCOMPARE(second.value(QStringLiteral("symbol")).toString(), QStringLiteral("SCRETH-USD"));
        // Ranked by edge_after_cost desc: 12.0 (SCRBTC) before 5.0 (SCRETH).
        QVERIFY(first.value(QStringLiteral("edge_after_cost")).toDouble() >
                second.value(QStringLiteral("edge_after_cost")).toDouble());
    }

    // Unrecognized --market value -> usage error, exit 2 (never falls through
    // to "all markets").
    void ai_screen_rejects_bad_market() {
        sandbox_test_home();
        int rc = -1;
        capture_stdout([&]() { return dispatch(QStringList{"ai", "screen", "--market", "bogus"}); }, &rc);
        QCOMPARE(rc, 2);
    }

    // A recognized market with no seeded/passing edge_decision_journal rows
    // -> exit 0 with an empty JSON array, not an error. Asserts the raw
    // stdout text itself (not just a parsed-array emptiness check) --
    // json_array_from_dispatch would also return an empty QJsonArray for
    // blank/null/malformed output, which would let this test pass even if
    // the command emitted nothing at all.
    void ai_screen_empty_market_returns_empty_array() {
        sandbox_test_home();
        int rc = -1;
        const QString out = capture_stdout([&]() {
            return dispatch(QStringList{"ai", "screen", "--market", "equity", "--json"});
        }, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(out.trimmed(), QStringLiteral("[]"));

        const QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8());
        QVERIFY(doc.isArray());
        QVERIFY(doc.array().isEmpty());
    }

    // --limit garbage/non-positive -> usage error, exit 2 (never silently
    // falls back to the default).
    void ai_screen_rejects_bad_limit() {
        sandbox_test_home();
        int rc = -1;
        capture_stdout([&]() { return dispatch(QStringList{"ai", "screen", "--limit", "abc"}); }, &rc);
        QCOMPARE(rc, 2);

        rc = -1;
        capture_stdout([&]() { return dispatch(QStringList{"ai", "screen", "--limit", "0"}); }, &rc);
        QCOMPARE(rc, 2);

        rc = -1;
        capture_stdout([&]() { return dispatch(QStringList{"ai", "screen", "--limit", "-3"}); }, &rc);
        QCOMPARE(rc, 2);
    }

    // READ-ONLY invariant: `ai screen` must not mutate a single cli.*
    // settings row, nor insert/delete any edge_decision_journal or
    // order_drafts row -- it only ever SELECTs (Screener.h's READ-ONLY
    // INVARIANT, reused via DecisionContext::assess).
    void ai_screen_is_read_only_on_gates() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute(
            "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, venue, side, gate,"
            " edge_after_cost, spread_cost, fee_cost, freshness_json, source) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
            {QStringLiteral("cd-scr-ro"), recent_ms(), recent_ms(), QStringLiteral("SCRROX-USD"),
             QStringLiteral("coinbase_advanced"), QStringLiteral("buy"), QStringLiteral("pass"), 6.0, 0.0, 0.0,
             QStringLiteral("{}"), QStringLiteral("s")}).is_ok());

        const QStringList before = cli_settings_fingerprint();
        const int journal_before = table_row_count(QStringLiteral("edge_decision_journal"));
        const int orders_before = table_row_count(QStringLiteral("order_drafts"));
        QVERIFY(journal_before >= 0);
        QVERIFY(orders_before >= 0);

        int rc = -1;
        json_array_from_dispatch(QStringList{"ai", "screen", "--market", "crypto", "--json"}, &rc);
        QCOMPARE(rc, 0);

        const QStringList after = cli_settings_fingerprint();
        const int journal_after = table_row_count(QStringLiteral("edge_decision_journal"));
        const int orders_after = table_row_count(QStringLiteral("order_drafts"));
        QCOMPARE(after, before);
        QCOMPARE(journal_after, journal_before);
        QCOMPARE(orders_after, orders_before);
    }

    // ai paper ledger Task 5 -- `ai positions --handler H --json` emits the
    // folded open position for that handler+symbol (net_qty, avg_entry_price,
    // realized_pnl), sourced from ai_ledger::positions_of (Task 3, pure
    // read-only fold over ai_fill rows). Seeded directly via SQL -- Task 5 is
    // read-only and must not depend on Task 6's `ai record-fill`.
    void ai_positions_reports_open_positions() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute(
            "INSERT INTO ai_fill (id,handler,symbol,side,quantity,fill_price,fee,realized_pnl,ts,draft_id) "
            "VALUES ('p1','cli','POS-USD','buy',5,20,0,0,100,'d')").is_ok());

        int rc = -1;
        QJsonArray arr = json_array_from_dispatch(QStringList{"ai", "positions", "--handler", "cli", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(arr.size(), 1);
        QCOMPARE(arr.at(0).toObject().value("symbol").toString(), QStringLiteral("POS-USD"));
        QCOMPARE(arr.at(0).toObject().value("net_qty").toDouble(), 5.0);
    }

    // `ai ledger --handler H --json` lists that handler's fills recent-first
    // (ts DESC), sourced from AiFillRepository::list (Task 2).
    void ai_ledger_lists_recent_fills() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute(
            "INSERT INTO ai_fill (id,handler,symbol,side,quantity,fill_price,fee,realized_pnl,ts,draft_id) "
            "VALUES ('l1','cli2','LDG-USD','buy',5,20,0,0,100,'d'),"
            "       ('l2','cli2','LDG-USD','sell',5,25,0,25,200,'d')").is_ok());

        int rc = -1;
        QJsonArray arr = json_array_from_dispatch(
            QStringList{"ai", "ledger", "--handler", "cli2", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(arr.size(), 2);
        QCOMPARE(arr.at(0).toObject().value("id").toString(), QStringLiteral("l2"));  // recent first
    }

    // `ai pnl --handler H --json` emits {realized_pnl, open_positions[]},
    // sourced from ai_ledger::realized_total + positions_of (Task 3).
    void ai_pnl_reports_realized_total() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute(
            "INSERT INTO ai_fill (id,handler,symbol,side,quantity,fill_price,fee,realized_pnl,ts,draft_id) "
            "VALUES ('n1','cli3','PNL-USD','buy',5,20,0,0,100,'d'),"
            "       ('n2','cli3','PNL-USD','sell',5,25,0,25,200,'d')").is_ok());

        int rc = -1;
        QJsonObject obj = json_object_from_dispatch(QStringList{"ai", "pnl", "--handler", "cli3", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(obj.value("realized_pnl").toDouble(), 25.0);
    }

    // READ-ONLY invariant: `ai positions|pnl|ledger|scorecard` must not mutate
    // a single cli.* settings row, nor insert any ai_fill row -- these four
    // commands issue only SELECTs (via positions_of/realized_total/list/
    // scorecard_of, all repository reads). ai_fill is these commands' own
    // domain table, so the count check guards against the worst violation: a
    // read command that accidentally appends a fill (e.g. a future
    // record_fill mis-wire). `scorecard` is bundled in HERE (rather than only
    // relying on its own `ai_scorecard_is_read_only`) because this slot runs
    // BEFORE any of the dedicated `ai_scorecard_*` slots below, so its
    // `before` snapshot cannot already contain a `cli.*` row a prior
    // scorecard-test neuter left behind -- unlike a same-key/same-value
    // same-second `INSERT OR REPLACE` that runs AFTER an earlier test already
    // wrote that exact row (undetectable by fingerprint at that point), a
    // neutered write here is the FIRST touch of `cli.*` in the run and so
    // reliably flips this assertion.
    void ai_read_commands_are_read_only_on_gates() {
        sandbox_test_home();
        const QStringList before = cli_settings_fingerprint();
        const int fills_before = table_row_count(QStringLiteral("ai_fill"));
        QVERIFY(fills_before >= 0);
        int rc = -1;
        json_array_from_dispatch(QStringList{"ai", "positions", "--json"}, &rc);
        QCOMPARE(rc, 0);
        json_array_from_dispatch(QStringList{"ai", "ledger", "--json"}, &rc);
        QCOMPARE(rc, 0);
        json_object_from_dispatch(QStringList{"ai", "pnl", "--json"}, &rc);
        QCOMPARE(rc, 0);
        json_object_from_dispatch(QStringList{"ai", "scorecard", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(cli_settings_fingerprint(), before);  // no cli.* gate written
        QCOMPARE(table_row_count(QStringLiteral("ai_fill")), fills_before);  // no ai_fill row written
    }

    // `ai record-fill` is the CLI write surface (Task 6): a buy then an
    // opposite sell must append two ai_fill rows via record_fill and reflect
    // the folded position + realized pnl in the emitted JSON.
    void ai_record_fill_writes_row_and_books_pnl() {
        sandbox_test_home();
        int rc = -1;
        QJsonObject open = json_object_from_dispatch(
            QStringList{"ai", "record-fill", "--handler", "w", "--symbol", "W-USD",
                        "--side", "buy", "--qty", "10", "--price", "100", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(open.value("position").toObject().value("net_qty").toDouble(), 10.0);

        QJsonObject close = json_object_from_dispatch(
            QStringList{"ai", "record-fill", "--handler", "w", "--symbol", "W-USD",
                        "--side", "sell", "--qty", "4", "--price", "130", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(close.value("fill").toObject().value("realized_pnl").toDouble(), 120.0);  // (130-100)*4
        QCOMPARE(close.value("position").toObject().value("net_qty").toDouble(), 6.0);
    }

    // Invalid args (missing --side, non-positive --qty) must be rejected with
    // exit 2 and write NO ai_fill row -- the validation guard runs BEFORE any
    // DB write. Note: missing --side and qty<=0 are ALSO independently caught
    // by lower layers (ai_fill.side is NOT NULL in the schema; record_fill()
    // itself rejects qty<=0), so this test also exercises a garbage --side
    // value ("hold") that only the CLI-level guard catches -- apply_fill()
    // silently treats any non-"sell"/"short" side as a buy, so without this
    // guard a bogus --side would record_fill() a wrong-direction row instead
    // of being rejected.
    void ai_record_fill_rejects_bad_args() {
        sandbox_test_home();
        const int rows_before = table_row_count(QStringLiteral("ai_fill"));
        int rc = 0;
        // missing --side
        json_object_from_dispatch(QStringList{"ai", "record-fill", "--handler", "w2",
                                              "--symbol", "W2-USD", "--qty", "1", "--price", "10"}, &rc);
        QCOMPARE(rc, 2);
        // garbage --side value (neither "buy" nor "sell") -- only the CLI
        // guard catches this; record_fill()/apply_fill() would silently
        // accept it as a buy.
        rc = 0;
        json_object_from_dispatch(QStringList{"ai", "record-fill", "--handler", "w2", "--symbol", "W2-USD",
                                              "--side", "hold", "--qty", "1", "--price", "10"}, &rc);
        QCOMPARE(rc, 2);
        // non-positive qty
        rc = 0;
        json_object_from_dispatch(QStringList{"ai", "record-fill", "--handler", "w2", "--symbol", "W2-USD",
                                              "--side", "buy", "--qty", "0", "--price", "10"}, &rc);
        QCOMPARE(rc, 2);
        QCOMPARE(table_row_count(QStringLiteral("ai_fill")), rows_before);  // no row written
    }

    // PARAMOUNT invariant: even the WRITE command's only mutation is the
    // single ai_fill row -- it must never touch a cli.* gate setting.
    void ai_record_fill_writes_no_gate() {
        sandbox_test_home();
        const QStringList before = cli_settings_fingerprint();
        int rc = -1;
        json_object_from_dispatch(QStringList{"ai", "record-fill", "--handler", "w3", "--symbol", "W3-USD",
                                              "--side", "buy", "--qty", "1", "--price", "10", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(cli_settings_fingerprint(), before);  // writes ai_fill, never a cli.* gate
    }

    // `ai scorecard --handler H --json` emits the handler's realized track
    // record (aggregate over realized closes), sourced from
    // ai_ledger::scorecard_of (Task 1). One open fill (excluded) + one win +
    // one loss -> 2 trades, 1 win, 1 loss, hit_rate 0.5, realized_total 60.
    void ai_scorecard_reports_track_record() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute(
            "INSERT INTO ai_fill (id,handler,symbol,side,quantity,fill_price,fee,realized_pnl,ts,draft_id) "
            "VALUES ('sc1','SCLI','SC-USD','buy',10,100,0,0,1000,'d'),"      // open, excluded
            "       ('sc2','SCLI','SC-USD','sell',2,110,0,100,1001,'d'),"    // win
            "       ('sc3','SCLI','SC-USD','sell',2,90,0,-40,1002,'d')").is_ok());  // loss

        int rc = -1;
        QJsonObject o = json_object_from_dispatch(
            QStringList{"ai", "scorecard", "--handler", "SCLI", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(o.value("trades").toInt(), 2);
        QCOMPARE(o.value("wins").toInt(), 1);
        QCOMPARE(o.value("losses").toInt(), 1);
        QCOMPARE(o.value("hit_rate").toDouble(), 0.5);
        QCOMPARE(o.value("realized_total").toDouble(), 60.0);  // 100 - 40
    }

    // An unknown/empty handler has no fills -> zeroed scorecard, still exit 0.
    void ai_scorecard_empty_is_zero() {
        sandbox_test_home();
        int rc = -1;
        QJsonObject o = json_object_from_dispatch(
            QStringList{"ai", "scorecard", "--handler", "nobody-xyz", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(o.value("trades").toInt(), 0);
        QCOMPARE(o.value("hit_rate").toDouble(), 0.0);
    }

    // READ-ONLY invariant: `ai scorecard` must not mutate a single cli.*
    // settings row, nor insert/alter any ai_fill row -- scorecard_of issues
    // only a SELECT (via AiFillRepository::list).
    void ai_scorecard_is_read_only() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute(
            "INSERT INTO ai_fill (id,handler,symbol,side,quantity,fill_price,fee,realized_pnl,ts,draft_id) "
            "VALUES ('scr1','h','SCRO-USD','sell',1,10,0,5,1000,'d')").is_ok());
        const QStringList before = cli_settings_fingerprint();
        const int fills_before = table_row_count(QStringLiteral("ai_fill"));
        int rc = -1;
        json_object_from_dispatch(QStringList{"ai", "scorecard", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(cli_settings_fingerprint(), before);
        QCOMPARE(table_row_count(QStringLiteral("ai_fill")), fills_before);
    }

    // `ai act` typed-action preview (Task 3 of piece Vc; direction-aware,
    // track 1 close-out): READ-ONLY -- translates a typed verb into a paper
    // TradeIntent preview given the symbol's current ledger position AND the
    // deterministic edge's ENDORSED direction (resolved via the SAME shared
    // ai_strategy::edge_direction_of() helper LlmStrategy uses by default),
    // without writing anything. Journal seed shapes mirror the passing `ai
    // ctx` floor tests (ai_ctx_floor_permits_endorsed_symbol et al.).

    // Enter, long: an endorsed LONG edge (gate=pass, cost-clear, fresh,
    // side=buy) resolves edge_direction=1 and an enter maps to a buy sized
    // by conviction.
    void ai_act_enter_long_previews_buy() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute(
            QStringLiteral(
                "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, gate, side,"
                " edge_after_cost, spread_cost, fee_cost, freshness_json, source)"
                " VALUES ('act-long1', %1, %1, 'LONGE-USD', 'pass', 'buy', 5.0, 1.0, 0.5,"
                "         '{\"freshest_age_ms\":100,\"live_sources\":3}', 'x')").arg(recent_ms())).is_ok());
        int rc = -1;
        QJsonObject o = json_object_from_dispatch(
            QStringList{"ai", "act", "LONGE-USD", "enter", "--conviction", "0.5", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(o.value("edge_direction").toInt(), 1);
        const QJsonObject intent = o.value("intent").toObject();
        QCOMPARE(intent.value("side").toString(), QStringLiteral("buy"));
        QCOMPARE(intent.value("quantity").toDouble(), 5.0);          // 0.5 * 10
        QCOMPARE(intent.value("order_type").toString(), QStringLiteral("market"));
    }

    // Enter, short: an endorsed SHORT edge (gate=pass, cost-clear, fresh,
    // side=short) resolves edge_direction=-1 and an enter maps to a sell --
    // this is the short-side-entries invariant track 1 exists to prove.
    void ai_act_enter_short_previews_sell() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute(
            QStringLiteral(
                "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, gate, side,"
                " edge_after_cost, spread_cost, fee_cost, freshness_json, source)"
                " VALUES ('act-short1', %1, %1, 'SHORTE-USD', 'pass', 'short', 5.0, 1.0, 0.5,"
                "         '{\"freshest_age_ms\":100,\"live_sources\":3}', 'x')").arg(recent_ms())).is_ok());
        int rc = -1;
        QJsonObject o = json_object_from_dispatch(
            QStringList{"ai", "act", "SHORTE-USD", "enter", "--conviction", "0.5", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(o.value("edge_direction").toInt(), -1);
        const QJsonObject intent = o.value("intent").toObject();
        QCOMPARE(intent.value("side").toString(), QStringLiteral("sell"));
        QCOMPARE(intent.value("quantity").toDouble(), 5.0);          // 0.5 * 10
        QCOMPARE(intent.value("order_type").toString(), QStringLiteral("market"));
    }

    // Enter, no/stale edge: a symbol with no edge_decision_journal row at
    // all resolves edge_direction=0 (missing -> not affirmatively endorsed),
    // so enter previews NO intent -- it never invents a side.
    void ai_act_enter_no_edge_previews_null() {
        sandbox_test_home();
        int rc = -1;
        QJsonObject o = json_object_from_dispatch(
            QStringList{"ai", "act", "NOEDGE-USD", "enter", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(o.value("edge_direction").toInt(), 0);
        QVERIFY(o.value("intent").isNull());
    }

    // Exit, short position: a sell fill leaves net_qty<0; exit ignores
    // edge_direction entirely and covers via a buy sized to |net_qty|.
    void ai_act_exit_short_position_previews_buy_cover() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute(
            "INSERT INTO ai_fill (id,handler,symbol,side,quantity,fill_price,fee,realized_pnl,ts,draft_id) "
            "VALUES ('act-shortpos1','claude','SHORTPOS-USD','sell',6,100,0,0,1000,'d')").is_ok());  // net_qty = -6
        int rc = -1;
        QJsonObject o = json_object_from_dispatch(
            QStringList{"ai", "act", "SHORTPOS-USD", "exit", "--json"}, &rc);
        QCOMPARE(rc, 0);
        const QJsonObject intent = o.value("intent").toObject();
        QCOMPARE(intent.value("side").toString(), QStringLiteral("buy"));
        QCOMPARE(intent.value("quantity").toDouble(), 6.0);
    }

    void ai_act_exit_previews_sell_of_position() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute(
            "INSERT INTO ai_fill (id,handler,symbol,side,quantity,fill_price,fee,realized_pnl,ts,draft_id) "
            "VALUES ('act1','claude','EXT-USD','buy',8,100,0,0,1000,'d')").is_ok());  // net_qty = 8
        int rc = -1;
        QJsonObject o = json_object_from_dispatch(
            QStringList{"ai", "act", "EXT-USD", "exit", "--json"}, &rc);
        QCOMPARE(rc, 0);
        const QJsonObject intent = o.value("intent").toObject();
        QCOMPARE(intent.value("side").toString(), QStringLiteral("sell"));
        QCOMPARE(intent.value("quantity").toDouble(), 8.0);
    }

    void ai_act_trim_flat_is_null_intent() {
        sandbox_test_home();
        int rc = -1;
        QJsonObject o = json_object_from_dispatch(
            QStringList{"ai", "act", "FLAT-USD", "trim", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(o.value("intent").isNull());   // nothing to trim
    }

    // "hold" is accepted as a vocabulary synonym for skip (parity with the
    // LLM verb surface) and always previews no intent.
    void ai_act_hold_is_null_intent() {
        sandbox_test_home();
        int rc = -1;
        QJsonObject o = json_object_from_dispatch(
            QStringList{"ai", "act", "HOLD-USD", "hold", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(o.value("intent").isNull());
    }

    void ai_act_unknown_action_exits_2() {
        sandbox_test_home();
        int rc = 0;
        json_object_from_dispatch(QStringList{"ai", "act", "X-USD", "bogus", "--json"}, &rc);
        QCOMPARE(rc, 2);
    }

    // READ-ONLY invariant: `ai act` must not mutate a single cli.* settings
    // row, nor insert/delete any edge_decision_journal or ai_fill row --
    // position_of and edge_direction_of (via assess) only ever SELECT, and
    // translate_action is pure (mirrors the `ai ctx` read-only fingerprint
    // tests above).
    void ai_act_is_read_only() {
        sandbox_test_home();
        QVERIFY(Database::instance().execute(
            QStringLiteral(
                "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, gate, side,"
                " edge_after_cost, spread_cost, fee_cost, freshness_json, source)"
                " VALUES ('act-ro1', %1, %1, 'RO-USD', 'pass', 'buy', 5.0, 1.0, 0.5,"
                "         '{\"freshest_age_ms\":100,\"live_sources\":3}', 'x')").arg(recent_ms())).is_ok());
        const QStringList before = cli_settings_fingerprint();
        const int fills_before = table_row_count(QStringLiteral("ai_fill"));
        const int journal_before = table_row_count(QStringLiteral("edge_decision_journal"));
        int rc = -1;
        json_object_from_dispatch(QStringList{"ai", "act", "RO-USD", "enter", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(cli_settings_fingerprint(), before);
        QCOMPARE(table_row_count(QStringLiteral("ai_fill")), fills_before);
        QCOMPARE(table_row_count(QStringLiteral("edge_decision_journal")), journal_before);
    }

    // `ai act --market` scopes the preview's edge_direction_of the same way a
    // scoped `ai run strategy claude --market` run would (Codex P1, #38): seed
    // the SAME symbol under a crypto (coinbase, buy) OLDER row and a prediction
    // (kalshi, short) NEWER row -- `--market crypto` must resolve the crypto
    // row's (buy) direction, not the newer prediction row's.
    void ai_act_market_scopes_edge_direction() {
        sandbox_test_home();
        const qint64 older = recent_ms(120000);  // 2 min ago
        const qint64 newer = recent_ms(30000);   // 30 s ago (newer)
        QVERIFY(Database::instance().execute(
            "INSERT INTO edge_decision_journal (id, created_at, updated_at, venue, symbol, side, gate,"
            " edge_after_cost, spread_cost, fee_cost, freshness_json, source)"
            " VALUES ('act-mkt-crypto', ?, ?, 'coinbase_advanced', 'ACTMKT-USD', 'buy', 'pass', 8.0, 1.0, 0.5,"
            "         '{\"freshest_age_ms\":100,\"live_sources\":3}', 'crypto row')", {older, older}).is_ok());
        QVERIFY(Database::instance().execute(
            "INSERT INTO edge_decision_journal (id, created_at, updated_at, venue, symbol, side, gate,"
            " edge_after_cost, spread_cost, fee_cost, freshness_json, source)"
            " VALUES ('act-mkt-pred', ?, ?, 'kalshi_mkt', 'ACTMKT-USD', 'short', 'pass', 3.0, 1.0, 0.5,"
            "         '{\"freshest_age_ms\":100,\"live_sources\":3}', 'prediction row')", {newer, newer}).is_ok());

        int rc = -1;
        QJsonObject o = json_object_from_dispatch(
            QStringList{"ai", "act", "ACTMKT-USD", "enter", "--market", "crypto", "--json"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(o.value("edge_direction").toInt(), 1);  // the OLDER crypto (buy) row, not the newer short.
        const QJsonObject intent = o.value("intent").toObject();
        QCOMPARE(intent.value("side").toString(), QStringLiteral("buy"));

        // Without --market: unchanged behavior -- newest-across-all == the kalshi (short) row.
        int rc_any = -1;
        QJsonObject o_any = json_object_from_dispatch(
            QStringList{"ai", "act", "ACTMKT-USD", "enter", "--json"}, &rc_any);
        QCOMPARE(rc_any, 0);
        QCOMPARE(o_any.value("edge_direction").toInt(), -1);
    }

    // --- Task 3: `ai strategy list` + `ai handler` CRUD (PAPER-ONLY/DISARMED;
    // AiHandlerRepository is a real DB-backed repo, so these need the shared
    // process-lifetime DB brought up via sandbox_test_home() like the sandbox
    // slots above). ---

    void ai_strategy_list_advertises_builtins() {
        sandbox_test_home();
        int rc = -1;
        const QJsonObject out = json_object_from_dispatch(
            QStringList{"--json", "ai", "strategy", "list"}, &rc);
        QCOMPARE(rc, 0);
        const QJsonArray strategies = out.value("strategies").toArray();
        QStringList names;
        for (const QJsonValue& v : strategies)
            names << v.toObject().value("name").toString();
        QVERIFY(names.contains("meanrev"));
        QVERIFY(names.contains("claude"));
        for (const QJsonValue& v : strategies) {
            const QJsonObject o = v.toObject();
            if (o.value("name").toString() == "claude")
                QVERIFY(o.value("needs_provider").toBool());
            if (o.value("name").toString() == "meanrev")
                QVERIFY(!o.value("needs_provider").toBool());
            QVERIFY(!o.value("description").toString().isEmpty());
        }
    }

    void ai_handler_create_list_show_roundtrip() {
        sandbox_test_home();
        int rc = -1;
        const QJsonObject created = json_object_from_dispatch(
            QStringList{"--json", "ai", "handler", "create", "crypto-scout", "--strategy", "claude",
                       "--symbols", "BTC-USD", "--market", "crypto"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(created.value("name").toString(), QString("crypto-scout"));
        QCOMPARE(created.value("strategy").toString(), QString("claude"));
        QCOMPARE(created.value("symbols").toString(), QString("BTC-USD"));
        QCOMPARE(created.value("market").toString(), QString("crypto"));
        // paper-only/disarmed invariant: a freshly created handler is never enabled.
        QVERIFY(!created.value("enabled").toBool());

        const QJsonObject shown = json_object_from_dispatch(
            QStringList{"--json", "ai", "handler", "show", "crypto-scout"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(shown.value("strategy").toString(), QString("claude"));
        QCOMPARE(shown.value("symbols").toString(), QString("BTC-USD"));
        QCOMPARE(shown.value("market").toString(), QString("crypto"));
        QVERIFY(!shown.value("enabled").toBool());

        const QJsonObject listed = json_object_from_dispatch(
            QStringList{"--json", "ai", "handler", "list"}, &rc);
        QCOMPARE(rc, 0);
        bool found = false;
        for (const QJsonValue& v : listed.value("handlers").toArray())
            if (v.toObject().value("name").toString() == "crypto-scout")
                found = true;
        QVERIFY(found);

        // enable/disable flip ONLY the saved-config flag.
        QCOMPARE(dispatch(QStringList{"ai", "handler", "enable", "crypto-scout"}), 0);
        QJsonObject after_enable = json_object_from_dispatch(
            QStringList{"--json", "ai", "handler", "show", "crypto-scout"}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(after_enable.value("enabled").toBool());

        QCOMPARE(dispatch(QStringList{"ai", "handler", "disable", "crypto-scout"}), 0);
        QJsonObject after_disable = json_object_from_dispatch(
            QStringList{"--json", "ai", "handler", "show", "crypto-scout"}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(!after_disable.value("enabled").toBool());

        QCOMPARE(dispatch(QStringList{"ai", "handler", "delete", "crypto-scout"}), 0);
        int show_rc = -1;
        json_object_from_dispatch(QStringList{"--json", "ai", "handler", "show", "crypto-scout"}, &show_rc);
        QVERIFY(show_rc != 0);
    }

    void ai_handler_create_rejects_unknown_strategy() {
        sandbox_test_home();
        int rc = -1;
        capture_stdout([&]() {
            rc = dispatch(QStringList{"ai", "handler", "create", "x", "--strategy", "bogus"});
            return rc;
        });
        QVERIFY2(rc != 0, "create with an unregistered strategy must fail");

        int show_rc = -1;
        json_object_from_dispatch(QStringList{"--json", "ai", "handler", "show", "x"}, &show_rc);
        QVERIFY2(show_rc != 0, "no row should have been written for the rejected create");
    }

    void ai_handler_claude_requires_a_persisted_market_scope() {
        sandbox_test_home();
        int missing_rc = -1;
        capture_stdout([&]() {
            missing_rc = dispatch(QStringList{"ai", "handler", "create", "unscoped", "--strategy", "claude"});
            return missing_rc;
        });
        QCOMPARE(missing_rc, 2);

        int bad_rc = -1;
        capture_stdout([&]() {
            bad_rc = dispatch(QStringList{"ai", "handler", "create", "bad-scope", "--strategy", "claude",
                                          "--market", "all"});
            return bad_rc;
        });
        QCOMPARE(bad_rc, 2);
    }

    // --- Task 4: `ai handler status` (read-only arm-state) + `ai handler run`
    // (PAPER-ONLY). SAFETY-CRITICAL: `status` must never mutate a gate; `run`
    // must refuse any non-paper mode. ---

    void ai_handler_status_reports_disarmed_gates() {
        sandbox_test_home();
        int rc = -1;
        const QJsonObject out = json_object_from_dispatch(
            QStringList{"--json", "ai", "handler", "status"}, &rc);
        QCOMPARE(rc, 0);
        // No live path exists in the plugin core: armed is ALWAYS false.
        QCOMPARE(out.value("armed").toBool(true), false);
        QVERIFY2(out.contains("gates"), "status must emit a gates object");
        const QJsonObject gates = out.value("gates").toObject();
        QVERIFY2(gates.contains("kill_switch"), "status gates must expose kill_switch");
        QVERIFY2(!out.value("disarmed_reason").toArray().isEmpty(),
                 "status must list at least one disarmed reason");
    }

    void ai_handler_status_is_read_only_on_gates() {
        sandbox_test_home();
        auto& settings = openmarketterminal::SettingsRepository::instance();
        // Arm a known kill-switch state so status has a non-default gate to read.
        QVERIFY(settings.set(QStringLiteral("cli.kill_switch"), QStringLiteral("true"),
                             QStringLiteral("test")).is_ok());

        // Snapshot ALL SIX gates status reads (not just kill_switch) so a stray
        // write to any of them is caught (default-deny false/empty when unset).
        const QStringList gate_keys{
            QStringLiteral("cli.kill_switch"),     QStringLiteral("cli.allow_paper_trading"),
            QStringLiteral("cli.allow_trading"),   QStringLiteral("cli.live_trading_armed"),
            QStringLiteral("cli.fast_live_armed"), QStringLiteral("cli.allowed_venues")};
        QMap<QString, QString> before_vals;
        for (const QString& k : gate_keys)
            before_vals[k] = settings.get(k, QString()).value();
        QCOMPARE(before_vals.value(QStringLiteral("cli.kill_switch")), QStringLiteral("true"));

        // Full row fingerprint (value AND updated_at) of every cli.* key — catches
        // an idempotent same-value INSERT-OR-REPLACE that bumps the timestamp, plus
        // any stray write to a cli.* key not in the six above.
        const QStringList before_fp = cli_settings_fingerprint();

        int rc = -1;
        const QJsonObject out = json_object_from_dispatch(
            QStringList{"--json", "ai", "handler", "status"}, &rc);
        QCOMPARE(rc, 0);
        // status must READ the engaged value...
        QCOMPARE(out.value("gates").toObject().value("kill_switch").toBool(false), true);

        // ...and MUST NOT have written ANY of the six gates (values unchanged)...
        for (const QString& k : gate_keys)
            QCOMPARE(settings.get(k, QString()).value(), before_vals.value(k));
        // ...nor mutated any cli.* row at all (value or timestamp) — gate immutability.
        QCOMPARE(cli_settings_fingerprint(), before_fp);

        // Reset shared state so later slots don't inherit an engaged kill switch.
        QVERIFY(settings.set(QStringLiteral("cli.kill_switch"), QStringLiteral("false"),
                             QStringLiteral("test")).is_ok());
    }

    void ai_handler_run_is_paper_only() {
        sandbox_test_home();
        // Belt-and-braces: make sure no earlier slot left the kill switch engaged,
        // otherwise the paper run would halt on tick 1 and pass for the wrong reason.
        openmarketterminal::SettingsRepository::instance().set(
            QStringLiteral("cli.kill_switch"), QStringLiteral("false"), QStringLiteral("test"));

        QCOMPARE(dispatch(QStringList{"ai", "handler", "create", "p1", "--strategy", "meanrev",
                                      "--symbols", "BTC-USD", "--venues", " Coinbase, kraken ",
                                      "--max-notional", "12.5", "--max-position", "0.75"}), 0);

        // Paper-only proof: a live run is REFUSED with exit 2 (no live path).
        int live_rc = -1;
        capture_stdout([&]() {
            live_rc = dispatch(QStringList{"ai", "handler", "run", "p1", "--mode", "live"});
            return live_rc;
        });
        QCOMPARE(live_rc, 2);

        // A bounded paper run completes cleanly and places NO order.
        int paper_rc = -1;
        const QString out = capture_stdout([&]() {
            paper_rc = dispatch(QStringList{"--headless", "ai", "handler", "run", "p1",
                                            "--mode", "paper", "--max-iters", "1", "--interval-sec", "0"});
            return paper_rc;
        });
        QCOMPARE(paper_rc, 0);
        QVERIFY2(out.contains("filled=0"),
                 qUtf8Printable("paper run must place no order; got: " + out));
        QVERIFY2(out.contains("caps_enforced=true max_notional=12.50 max_position=0.75 "
                              "allowed_venues=coinbase,kraken"),
                 qUtf8Printable("saved handler limits must reach RunConfig; got: " + out));

        dispatch(QStringList{"ai", "handler", "delete", "p1"});
    }

    void ai_handler_run_rejects_legacy_unscoped_claude_handler() {
        sandbox_test_home();
        AiHandler legacy;
        legacy.name = QStringLiteral("legacy-claude");
        legacy.strategy = QStringLiteral("claude");
        legacy.symbols = QStringLiteral("BTC-USD");
        QVERIFY(AiHandlerRepository::instance().save(legacy).is_ok());

        int rc = -1;
        capture_stdout([&]() {
            rc = dispatch(QStringList{"ai", "handler", "run", "legacy-claude", "--paper", "--max-iters", "1"});
            return rc;
        });
        QCOMPARE(rc, 2);
        QVERIFY(AiHandlerRepository::instance().remove(QStringLiteral("legacy-claude")).is_ok());
    }

    // `ai run strategy claude` resolves ENTER direction from the deterministic
    // edge (edge_direction_of) and scopes the floor's assess_fn the same way;
    // without --market that lookup is unscoped and can bleed a direction across
    // venues (Codex P1, #38). --market is therefore REQUIRED for 'claude' (exit
    // 2 without it); 'meanrev' is deterministic (no edge direction) and stays
    // unaffected.
    void ai_run_strategy_claude_requires_market() {
        sandbox_test_home();
        // Belt-and-braces: an engaged kill switch would halt tick 1 for the
        // wrong reason and mask what this test actually checks.
        openmarketterminal::SettingsRepository::instance().set(
            QStringLiteral("cli.kill_switch"), QStringLiteral("false"), QStringLiteral("test"));

        // WITHOUT --market: refused before any transport bring-up (exit 2).
        int rc_missing = -1;
        capture_stdout([&]() {
            rc_missing = dispatch(QStringList{"ai", "run", "strategy", "claude", "--mode", "paper",
                                              "--max-iters", "1", "--interval-sec", "0"});
            return rc_missing;
        });
        QCOMPARE(rc_missing, 2);

        // WITH --market crypto: accepted; a bounded headless paper run completes.
        int rc_with_market = -1;
        capture_stdout([&]() {
            rc_with_market = dispatch(QStringList{"--headless", "ai", "run", "strategy", "claude",
                                                  "--mode", "paper", "--market", "crypto",
                                                  "--max-iters", "1", "--interval-sec", "0"});
            return rc_with_market;
        });
        QCOMPARE(rc_with_market, 0);

        // meanrev is deterministic (no edge direction) -- --market stays optional.
        int rc_meanrev = -1;
        capture_stdout([&]() {
            rc_meanrev = dispatch(QStringList{"--headless", "ai", "run", "strategy", "meanrev",
                                              "--mode", "paper", "--max-iters", "1", "--interval-sec", "0"});
            return rc_meanrev;
        });
        QCOMPARE(rc_meanrev, 0);
    }

    // LOAD-BEARING: the firewalled advisory challenge's blind context must
    // NEVER leak market pricing / model-conclusion fields. build_advise_open()
    // is a pure (no-DB) function so this test asserts on the emitted object
    // directly, without process/stdout capture -- see CommandDispatch.h.
    //
    // The fixture mirrors kalshi_auto_current_snapshot()'s real nested shape
    // (contract/horizon/execution/flow/spot_microstructure) and additionally
    // plants decoy forbidden-named fields at every level the open-handler's
    // extraction logic actually reads from (top level, inside "contract",
    // inside "execution", inside "flow") -- exactly the places a sloppy
    // "copy everything except a denylist" implementation would leak from.
    void advise_open_json_never_leaks_price() {
        const QJsonObject horizon{
            {"spot", 65000.0}, {"floor_strike", 64000.0}, {"cap_strike", 66000.0},
            {"reference_strike", 64000.0}, {"distance_from_strike", 1000.0},
            {"required_move_bps", 153.8}, {"realized_move_30s_bps", 12.0},
            {"seconds_left", QStringLiteral("900")}, {"settlement_band", "final_15m"},
            {"settlement_source", "CF Benchmarks BRTI"}, {"eligible_context", true}};
        const QJsonObject contract{
            {"question", "BTC above $64,000?"}, {"event_ticker", "KXBTC-TEST"},
            {"close_time", "2026-07-20T13:00:00Z"}, {"seconds_left", 900},
            {"horizon", horizon}, {"yes_mid", 0.62}, {"yes_change_30s_cents", 1.0},
            // Decoys directly inside "contract" -- a sibling of the fields the
            // handler legitimately reads (seconds_left, horizon, yes_mid).
            {"fair_yes", 0.63}, {"fair_no", 0.37}, {"model_weight", 0.5}};
        const QJsonObject flow{
            {"windows", QJsonObject{}},
            {"divergence", QJsonObject{{"label", "ALIGNED"}, {"spot_change_bps", 0.1}}}};
        const QJsonObject execution{
            {"yes", QJsonObject{{"bid", 0.60}, {"ask", 0.63}, {"bid_size", 120.0},
                                {"ask_size", 80.0}, {"fee_per_contract", 0.01}}},
            {"no", QJsonObject{{"bid", 0.37}, {"ask", 0.40}, {"bid_size", 90.0},
                               {"ask_size", 110.0}, {"fee_per_contract", 0.01}}}};
        const QJsonObject spot_microstructure{
            {"schema_version", 1}, {"event", "crypto_microstructure_snapshot"},
            {"call", "TRADE CANDIDATE"}, {"direction", "up"},
            {"reference_price", 65000.0}, {"microprice", 65001.5},
            {"best_bid", 64999.0}, {"best_ask", 65003.0}, {"confidence", "HIGH"},
            // Decoys inside spot_microstructure -- this object is copied
            // WHOLESALE into the blind context (it's on adv::kAllowlist as a
            // whole key), so unlike "horizon" (flattened to a plain label) it
            // has no per-field extraction to guard it. A future
            // CryptoMicrostructureRadar::to_json() field addition could leak
            // straight through unless the wholesale copy is itself sanitized.
            {"model_weight", 0.9},
            {"divergence", QJsonObject{{"label", "DIVERGENCE"}, {"price_diverges", true}}}};
        const QJsonObject snapshot{
            {"schema", 3}, {"ticker", "KXBTC-TEST"}, {"observed_at_ms", QString::number(recent_ms())},
            {"contract", contract}, {"flow", flow}, {"execution", execution},
            {"spot_microstructure", spot_microstructure},
            // Decoys at the snapshot top level, siblings of "contract"/"execution".
            {"market_implied_probability", 0.777}, {"market_curve_probability", 0.8},
            {"daemon_probability", 0.71}, {"calibrated_probability", 0.65},
            {"cost_net_edge", 0.09}};

        const QJsonObject built = build_advise_open(snapshot, QStringLiteral("KXBTC-TEST"),
                                                     recent_ms(0));
        QVERIFY2(!built.contains(QStringLiteral("error")),
                 qUtf8Printable(built.value(QStringLiteral("error")).toString()));
        QVERIFY(built.value(QStringLiteral("PRICE_WITHHELD")).toBool());
        const QJsonObject context = built.value(QStringLiteral("context")).toObject();
        QVERIFY(!context.isEmpty());
        for (const QString& key : openmarketterminal::adv::kBlindForbiddenKeys())
            QVERIFY2(!json_contains_key_deep(context, key), qUtf8Printable("leaked " + key));
    }

    // End-to-end wiring: `kalshi auto advise open|commit-blind|reveal|
    // commit-post` through the real router/dispatch() path (catches wiring
    // bugs the pure build_advise_open() test above cannot -- argument
    // parsing, repository persistence, state-machine transitions). Writes a
    // real kalshi-ws-books.json fixture into the sandboxed HOME's evidence
    // dir exactly like the daemon would.
    void kalshi_advise_open_commit_blind_reveal_commit_post_round_trip() {
        const QString home = sandbox_test_home();
        const QString ticker = QStringLiteral("KXBTC-RT");
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const QJsonObject horizon{
            {"spot", 65000.0}, {"floor_strike", 64000.0}, {"cap_strike", 66000.0},
            {"distance_from_strike", 1000.0}, {"required_move_bps", 153.8},
            {"realized_move_30s_bps", 12.0}, {"seconds_left", QStringLiteral("900")},
            {"settlement_band", "final_15m"}, {"settlement_source", "CF Benchmarks BRTI"},
            {"eligible_context", true}};
        const QJsonObject contract{
            {"question", "BTC above $64,000?"}, {"event_ticker", "KXBTC-RT-EVT"},
            {"seconds_left", 900}, {"horizon", horizon}, {"yes_mid", 0.62}};
        const QJsonObject execution{
            {"yes", QJsonObject{{"bid", 0.60}, {"ask", 0.63}, {"bid_size", 120.0}}},
            {"no", QJsonObject{{"bid", 0.37}, {"ask", 0.40}, {"bid_size", 90.0}}}};
        const QJsonObject flow{{"divergence", QJsonObject{{"label", "ALIGNED"}}}};
        const QJsonObject snapshot{
            {"schema", 3}, {"ticker", ticker}, {"observed_at_ms", QString::number(now)},
            {"contract", contract}, {"execution", execution}, {"flow", flow}};
        const QJsonObject payload{
            {"schema", 3}, {"updated_at_ms", QString::number(now)},
            {"snapshots", QJsonObject{{ticker, snapshot}}}};
        QFile evidence(QDir(home).filePath(QStringLiteral("kalshi-evidence/kalshi-ws-books.json")));
        QVERIFY(evidence.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(evidence.write(QJsonDocument(payload).toJson(QJsonDocument::Compact)),
                 QJsonDocument(payload).toJson(QJsonDocument::Compact).size());
        evidence.close();

        int rc = -1;
        const QJsonObject opened = json_object_from_dispatch(
            {"--json", "--headless", "kalshi", "auto", "advise", "open", "--ticker", ticker}, &rc);
        QCOMPARE(rc, 0);
        QVERIFY(opened.value(QStringLiteral("PRICE_WITHHELD")).toBool());
        const QString challenge_id = opened.value(QStringLiteral("challenge_id")).toString();
        QVERIFY(!challenge_id.isEmpty());
        const QJsonObject context = opened.value(QStringLiteral("context")).toObject();
        for (const QString& key : openmarketterminal::adv::kBlindForbiddenKeys())
            QVERIFY2(!json_contains_key_deep(context, key), qUtf8Printable("leaked " + key));

        const QJsonObject blind = json_object_from_dispatch(
            {"--json", "--headless", "kalshi", "auto", "advise", "commit-blind",
             "--challenge", challenge_id, "--commit-id", "rt-1", "--probability", "0.7"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(blind.value(QStringLiteral("state")).toString(), QStringLiteral("COMMITTED_BLIND"));
        QVERIFY(!blind.contains(QStringLiteral("market_implied_probability")));

        const QJsonObject revealed = json_object_from_dispatch(
            {"--json", "--headless", "kalshi", "auto", "advise", "reveal",
             "--challenge", challenge_id}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(revealed.value(QStringLiteral("state")).toString(), QStringLiteral("REVEALED"));
        QVERIFY(revealed.contains(QStringLiteral("withheld_market")));
        QCOMPARE(revealed.value(QStringLiteral("withheld_market")).toObject()
                     .value(QStringLiteral("market_implied_probability")).toDouble(), 0.62);

        const QJsonObject posted = json_object_from_dispatch(
            {"--json", "--headless", "kalshi", "auto", "advise", "commit-post",
             "--challenge", challenge_id, "--commit-id", "rt-2", "--probability", "0.68"}, &rc);
        QCOMPARE(rc, 0);
        QCOMPARE(posted.value(QStringLiteral("state")).toString(), QStringLiteral("COMMITTED_POST"));
    }
};
QTEST_MAIN(TstCommandDispatch)
#include "tst_command_dispatch.moc"
