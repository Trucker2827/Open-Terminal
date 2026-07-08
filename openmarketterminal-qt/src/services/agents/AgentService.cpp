// src/services/agents/AgentService.cpp
//
// Singleton + state for the agent layer: instance accessor, cache, payload
// construction (build_api_keys / build_payload), Python-process invocation
// (run_python_light / run_python_stdin), and the DataHub publish surface
// (ensure_registered_with_hub / publish_agent_*). Higher-level entry points
// live in:
//   - AgentService_Discovery.cpp    — agent/tool/model discovery + configs
//   - AgentService_Execution.cpp    — run_agent, run_team, routing, multi
//   - AgentService_Workflows.cpp    — plans, portfolio analytics, etc.
//   - AgentService_Repositories.cpp — memory, sessions, paper trading
#include "services/agents/AgentService.h"

#include "services/llm/LlmService.h"
#include "auth/AuthManager.h"
#include "core/logging/Logger.h"
#include "mcp/McpProvider.h"
#include "mcp/McpTypes.h"
#include "mcp/TerminalMcpBridge.h"
#include "mcp/ToolConfirmationGate.h"
#include "mcp/tools/SettingsGate.h"
#include "python/PythonRunner.h"
#include "storage/cache/CacheManager.h"
#include "storage/repositories/LlmConfigRepository.h"

#    include "datahub/DataHub.h"
#    include "datahub/TopicPolicy.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QUuid>
#include <QVariant>

#include <memory>

namespace openmarketterminal::services {

namespace {

// Render a destructive tool call into a human-readable (title, detail) for the
// confirmation modal so the user sees EXACTLY what they are approving — most
// importantly for the money/code tools. Falls back to the raw tool name + a
// compact JSON of the args.
std::pair<QString, QString> describe_tool_action(const QString& tool, const QJsonObject& args) {
    auto s = [&](const char* k) { return args.value(QLatin1String(k)).toVariant().toString(); };

    if (tool == QLatin1String("live_place_order")) {
        const QString qty = args.contains("quantity") ? s("quantity") : s("qty");
        return {QObject::tr("Place a LIVE order with REAL money?"),
                QObject::tr("%1 %2 %3 on %4 (%5).\nThis sends a real order to your broker.")
                    .arg(s("action"), qty, s("symbol"),
                         s("exchange").isEmpty() ? QStringLiteral("broker") : s("exchange"),
                         s("order_type").isEmpty() ? QStringLiteral("MARKET") : s("order_type"))};
    }
    if (tool == QLatin1String("run_python_script") || tool == QLatin1String("run_python")) {
        return {QObject::tr("Run a Python script on your machine?"),
                QObject::tr("script: %1\nargs: %2")
                    .arg(s("script"),
                         QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)).left(400))};
    }
    if (tool == QLatin1String("save_workflow") || tool == QLatin1String("save_agent_config")) {
        return {QObject::tr("Let the AI save an agent/workflow config?"),
                QObject::tr("%1 — a saved workflow/agent can later run code or place orders.")
                    .arg(tool)};
    }
    // Generic destructive tool.
    return {QObject::tr("Approve AI action: %1?").arg(tool),
            QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)).left(600)};
}

}  // namespace

// ── Singleton ────────────────────────────────────────────────────────────────

AgentService& AgentService::instance() {
    static AgentService inst;
    return inst;
}

AgentService::AgentService(QObject* parent) : QObject(parent) {
    // ONE-SHOT-SAFETY GUARD (Phase 2b): the constructor side effects below
    // (binding a localhost TCP server via TerminalMcpBridge, writing a
    // bridge.json discovery file, and installing the interactive GUI auth
    // checker) must run ONLY in a real interactive GUI process. A headless
    // host (HeadlessRuntime / openterminalcli, a QCoreApplication) now
    // constructs this singleton too — via register_all_data_services() — so
    // without this gate a one-shot CLI would bind a socket and leave a stale
    // bridge.json. The agent:* DataHub producer is still wired unconditionally
    // by ensure_registered_with_hub() (called by the helper); only the
    // feed/bridge side effects are GUI-gated. In headless, HeadlessRuntime
    // installs its own (deny-by-default) auth checker, so skipping the GUI one
    // here is correct. A genuine GUI run (QApplication) keeps the exact prior
    // behavior. inherits("QApplication") is a runtime metaobject check, so
    // openterminal_core stays Widgets-free.
    auto* qcoreapp = QCoreApplication::instance();
    const bool gui_mode = qcoreapp && qcoreapp->inherits("QApplication");
    if (gui_mode) {
    // Phase 1/2 wiring — start the local HTTP bridge that exposes internal
    // MCP tools to the Python finagent subprocess, and install the auth
    // checker that gates agent-originated tool calls. Both are idempotent;
    // failure to bind the port is logged but not fatal — agents simply lose
    // terminal-tool access.
    if (mcp::TerminalMcpBridge::instance().start()) {
        LOG_INFO("AgentService", "Terminal MCP bridge started: " +
                                     mcp::TerminalMcpBridge::instance().endpoint());
    } else {
        LOG_WARN("AgentService", "Terminal MCP bridge failed to start — agents will run "
                                 "without internal tool access");
    }

    // Auth checker — process-wide. Gates every MCP/AI-originated tool call
    // (chat assistant, autonomous agents, and external MCP clients). It does
    // NOT gate the user's own UI actions or the algo engine (DeploymentRunner/
    // AlgoEngine call the brokers directly in C++, never through MCP), so a
    // user-configured autonomous strategy keeps working for the user. Rules:
    //   - AuthLevel >= Verified  → always deny (no path can prove the
    //     interactive gate non-interactively)
    //   - is_destructive + opted-in autonomous agent (is_destructive_allowed()
    //     is a thread_local set TRUE only inside an agent config that explicitly
    //     enabled allow_destructive_tools) → allow without prompting; the user
    //     deliberately authorized that agent to act on its own.
    //   - is_destructive on any other path (chat assistant, external MCP client,
    //     non-opted agent) → ASK THE USER via ToolConfirmationGate. The gate is
    //     FAIL-CLOSED: if no confirmation UI is installed, the prompt is
    //     dismissed, or anything goes wrong, it returns false (deny). So the
    //     deny-by-default floor still holds — an AI steered by prompt-injected /
    //     poisoned content can never place a live order, run Python, or plant a
    //     workflow/agent without an explicit human Approve click.
    //   - everything else        → allow (read-only tools)
    mcp::McpProvider::instance().set_auth_checker(
        [](const QString& tool_name, const QJsonObject& args, mcp::AuthLevel required,
           bool is_destructive) -> bool {
            if (required >= mcp::AuthLevel::Verified)
                return false;
            // submit_order carve-out — identical to the daemon (ServeCommand) and
            // headless (HeadlessRuntime) checkers, so the AI-trading substrate is
            // reachable the same way from every host. Paper submits reach the
            // handler (which enforces cli.allow_paper_trading + the risk floor +
            // the kill switch — all checked LIVE there), with NO per-trade
            // confirmation dialog; live requires the GUI-only arm toggles. Every
            // OTHER destructive tool still falls through to the confirmation gate.
            if (tool_name == QLatin1String("submit_order")) {
                const QString mode = args.value("mode").toString().trimmed().toLower();
                if (mode == QLatin1String("paper"))
                    return true;
                return mcp::cli_trading_allowed() && mcp::cli_live_armed();
            }
            // Fast-live carve-out (Phase D) — IDENTICAL predicate in all three
            // hosts (daemon/headless/GUI), so the fast-trading substrate is
            // reachable the same way everywhere. The fast-live tool set is allowed
            // ONLY when fully armed: base trading + base live arm + the SECOND fast
            // arm (cli.fast_live_armed, GUI-only). It bypasses the per-call
            // confirmation gate below exactly like the submit_order carve-out; the
            // fast handlers re-enforce kill-switch + allowed-account. (When the fast
            // tools are built they must NOT be classified live-execution, since the
            // other hosts deny those outright.)
            if (mcp::is_fast_live_tool(tool_name))
                return mcp::cli_trading_allowed() && mcp::cli_live_armed() && mcp::cli_fast_live_armed();
            // Raw live-execution tools (live_place_order / live_cancel_* /
            // live_close_* — category "live-trading" + destructive) are NEVER
            // reachable by ANY AI/CLI caller, even with a destructive grant (the
            // in-app agent's destructive token). They bypass the Phase-C
            // constitution (arm / allowed-account / kill-switch / daily-loss), so
            // the AI's only live path is the gated submit_order / fast-live
            // carve-outs above. Mirrors HeadlessRuntime + ServeCommand.
            if (mcp::is_live_execution_tool(tool_name))
                return false;
            if (is_destructive && !mcp::TerminalMcpBridge::is_destructive_allowed()) {
                const auto desc = describe_tool_action(tool_name, args);
                return mcp::ToolConfirmationGate::instance().request(desc.first, desc.second);
            }
            return true;
        });
    }  // gui_mode
    connect(&mcp::TerminalMcpBridge::instance(), &mcp::TerminalMcpBridge::tool_called, this,
            &AgentService::record_agent_tool_call, Qt::UniqueConnection);
}

// ── Cache helpers ────────────────────────────────────────────────────────────

void AgentService::clear_cache() {
    openmarketterminal::CacheManager::instance().clear_category("agents");
    LOG_INFO("AgentService", "Cache cleared");
}

QVector<AgentInfo> AgentService::cached_agents() const {
    const QVariant cv = openmarketterminal::CacheManager::instance().get("agents:list");
    if (cv.isNull())
        return {};
    const QJsonObject root = QJsonDocument::fromJson(cv.toString().toUtf8()).object();
    QVector<AgentInfo> agents;
    for (const auto& v : root["agents"].toArray()) {
        const QJsonObject o = v.toObject();
        AgentInfo info;
        info.id = o["id"].toString();
        info.name = o["name"].toString();
        info.description = o["description"].toString();
        info.category = o["category"].toString();
        info.provider = o["provider"].toString();
        info.version = o["version"].toString();
        info.config = o["config"].toObject();
        for (const auto& c : o["capabilities"].toArray())
            info.capabilities.append(c.toString());
        for (const auto& ac : o["asset_classes"].toArray())
            info.asset_classes.append(ac.toString());
        agents.append(info);
    }
    return agents;
}

int AgentService::cached_agent_count() const {
    const QVariant cv = openmarketterminal::CacheManager::instance().get("agents:list");
    if (cv.isNull())
        return 0;
    return QJsonDocument::fromJson(cv.toString().toUtf8()).object()["agents"].toArray().size();
}

// ── API key builder ──────────────────────────────────────────────────────────

QJsonObject AgentService::build_api_keys() const {
    // Python's _get_api_key() looks up keys by env-var name (e.g. "ANTHROPIC_API_KEY"),
    // not by lowercase provider name. Send both forms so the lookup always succeeds.
    static const QMap<QString, QString> kEnvVarNames = {
        {"anthropic", "ANTHROPIC_API_KEY"},
        {"openai", "OPENAI_API_KEY"},
        {"google", "GOOGLE_API_KEY"},
        {"gemini", "GOOGLE_API_KEY"},
        {"groq", "GROQ_API_KEY"},
        {"deepseek", "DEEPSEEK_API_KEY"},
        {"financial_datasets", "FINANCIAL_DATASETS_API_KEY"},
        {"tavily", "TAVILY_API_KEY"},
        {"mistral", "MISTRAL_API_KEY"},
        {"cohere", "COHERE_API_KEY"},
        {"xai", "XAI_API_KEY"},
        {"kimi", "MOONSHOT_API_KEY"},
        {"moonshot", "MOONSHOT_API_KEY"},
    };

    QJsonObject keys;
    auto providers = LlmConfigRepository::instance().list_providers();
    if (providers.is_ok()) {
        for (const auto& p : providers.value()) {
            if (p.api_key.isEmpty())
                continue;
            const QString lower = p.provider.toLower();
            keys[lower] = p.api_key; // lowercase form
            const QString env_name = kEnvVarNames.value(lower);
            if (!env_name.isEmpty())
                keys[env_name] = p.api_key; // env-var form Python expects
            // Provider aliases: send canonical form so Python ModelsRegistry resolves correctly
            // e.g. "gemini" -> also set "google" so key lookup works after alias normalisation
            static const QMap<QString, QString> kProviderAliases = {
                {"gemini", "google"},
                {"claude", "anthropic"},
            };
            const QString alias = kProviderAliases.value(lower);
            if (!alias.isEmpty())
                keys[alias] = p.api_key;
            // Also ship base_url so super_agent/_llm_classify can use the right
            // endpoint (e.g. MiniMax OpenAI-compat behind "anthropic" provider).
            if (!p.base_url.isEmpty()) {
                keys[lower + "_base_url"] = p.base_url;
                keys[lower.toUpper() + "_BASE_URL"] = p.base_url;
            }
        }
    }

    // Always include OpenMarketTerminal session API key (from login) so agents can use
    // the openmarketterminal provider even if user hasn't manually configured it in Settings.
    if (!keys.contains("openmarketterminal")) {
        const auto& session = auth::AuthManager::instance().session();
        if (!session.api_key.isEmpty()) {
            keys["openmarketterminal"] = session.api_key;
            keys["OPENMARKETTERMINAL_API_KEY"] = session.api_key;
        }
    }

    return keys;
}

// ── Payload builder ──────────────────────────────────────────────────────────

QJsonObject AgentService::build_payload(const QString& action, const QJsonObject& params,
                                        const QJsonObject& config) const {
    QJsonObject payload;
    payload["action"] = action;
    payload["api_keys"] = build_api_keys();

    // Inject the resolved LLM config as active_llm so Python can build the exact
    // model instance without re-resolving credentials.
    // Priority: config["model"] (per-agent resolved profile) > global active LLM.
    // This means: if the caller already embedded a resolved profile in config["model"]
    // (as build_config_from_editor() now does), Python gets the right per-agent creds.
    {
        if (config.contains("model") && !config["model"].toObject()["provider"].toString().isEmpty()) {
            // Use the per-agent resolved profile already embedded in the config
            payload["active_llm"] = config["model"].toObject();
        } else {
            auto& llm = ai_chat::LlmService::instance();
            if (llm.is_configured()) {
                QJsonObject active_llm;
                active_llm["provider"] = llm.active_provider();
                active_llm["model_id"] = llm.active_model();
                active_llm["api_key"] = llm.active_api_key();
                active_llm["base_url"] = llm.active_base_url();
                active_llm["temperature"] = llm.active_temperature();
                active_llm["max_tokens"] = llm.active_max_tokens();
                payload["active_llm"] = active_llm;
            }
        }
    }

    // Resolve user_id for per-persona SQLite isolation on the Python side.
    // Priority: params["user_id"] (caller override) > config["user_id"] > session-derived.
    // Session: user_info.id > 0 → QString::number(id); id == 0 (guest/unauth) → "guest".
    QJsonObject enriched_params = params;
    if (!enriched_params.contains("user_id") || enriched_params["user_id"].toString().isEmpty()) {
        QString uid;
        if (config.contains("user_id") && !config["user_id"].toString().isEmpty()) {
            uid = config["user_id"].toString();
        } else {
            const auto& session = auth::AuthManager::instance().session();
            uid = session.user_info.id > 0 ? QString::number(session.user_info.id)
                                           : QStringLiteral("guest");
        }
        enriched_params["user_id"] = uid;
    }

    // Phase 3 — inject the local MCP bridge endpoint + filtered tool catalog
    // into the config the Python toolkit reads. Skip if the caller already set
    // these (lets explicit overrides win) or if the bridge is not running.
    QJsonObject enriched_config = config;
    auto& bridge = mcp::TerminalMcpBridge::instance();
    // Honour an explicit opt-out from the agent config: if the agent author
    // set `terminal_tools_enabled=false`, skip all bridge wiring and let the
    // agent run with only its declared `tools` (yfinance/duckduckgo/etc.).
    const bool terminal_tools_enabled = enriched_config.value("terminal_tools_enabled").toBool(true);

    if (bridge.is_active() && terminal_tools_enabled) {
        if (!enriched_config.contains("terminal_mcp_endpoint"))
            enriched_config["terminal_mcp_endpoint"] = bridge.endpoint();
        if (!enriched_config.contains("terminal_mcp_token"))
            enriched_config["terminal_mcp_token"] = bridge.token();
        // Capability token — only injected when the agent config opts in.
        // Without this header on each request the bridge will block any
        // `is_destructive=true` tool call even if the agent's LLM tries one.
        if (enriched_config.value("allow_destructive_tools").toBool(false) &&
            !enriched_config.contains("terminal_mcp_destructive_token")) {
            enriched_config["terminal_mcp_destructive_token"] = bridge.destructive_token();
        }
        if (!enriched_config.contains("terminal_tools")) {
            // Per-agent override via config["tool_filter"] supports:
            //   categories[]              — whitelist (empty = all enabled)
            //   exclude_categories[]      — blacklist, ON TOP of defaults below
            //   name_patterns[]           — regex include on tool name
            //   exclude_name_patterns[]   — regex exclude on tool name
            //   max_tools (int)           — hard cap
            // Default excludes UI-only and recursive categories so agents
            // don't drive the UI or call the chat LLM.
            mcp::ToolFilter filter;
            const QStringList default_excludes = {"navigation", "system", "settings", "ai-chat", "meta"};
            filter.exclude_categories = default_excludes;
            const QJsonObject tf = enriched_config.value("tool_filter").toObject();
            if (!tf.isEmpty()) {
                if (tf.contains("categories")) {
                    filter.categories.clear();
                    for (const auto& v : tf["categories"].toArray())
                        filter.categories.append(v.toString());
                }
                if (tf.contains("exclude_categories")) {
                    // User excludes are ADDITIVE on top of defaults — defaults
                    // are non-negotiable (UI tools are never safe for agents).
                    for (const auto& v : tf["exclude_categories"].toArray()) {
                        const QString cat = v.toString().trimmed();
                        if (!cat.isEmpty() && !filter.exclude_categories.contains(cat))
                            filter.exclude_categories.append(cat);
                    }
                }
                if (tf.contains("name_patterns")) {
                    for (const auto& v : tf["name_patterns"].toArray()) {
                        const QString p = v.toString().trimmed();
                        if (!p.isEmpty()) filter.name_patterns.append(p);
                    }
                }
                if (tf.contains("exclude_name_patterns")) {
                    for (const auto& v : tf["exclude_name_patterns"].toArray()) {
                        const QString p = v.toString().trimmed();
                        if (!p.isEmpty()) filter.exclude_name_patterns.append(p);
                    }
                }
                filter.max_tools = tf.value("max_tools").toInt(0);
            }
            const bool include_external = enriched_config.value("include_external_mcp").toBool(true);
            enriched_config["terminal_tools"] = bridge.tool_definitions(filter, include_external);
        }

        // Dry-run mode is opt-in and read by the Python TerminalToolkit. When
        // true, the toolkit short-circuits each call and returns a synthetic
        // result without crossing the bridge — useful for testing prompts /
        // agent loops without touching real state. We just propagate the
        // flag; nothing on the C++ side changes.
        // (No-op here — `tools_dry_run` already lives in enriched_config if
        // the agent set it; CreateAgentPanel writes the key at save time.)
    }

    if (!enriched_params.isEmpty())
        payload["params"] = enriched_params;
    if (!enriched_config.isEmpty())
        payload["config"] = enriched_config;
    return payload;
}

// ── Python lightweight runner (via PythonRunner args) ─────────────────────────

void AgentService::run_python_light(const QString& action, const QJsonObject& params,
                                    std::function<void(bool, QJsonObject)> on_result) {
    QJsonObject payload = build_payload(action, params);
    QString payload_str = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));

    QPointer<AgentService> self = this;
    python::PythonRunner::instance().run(
        "agents/finagent_core/main.py", {payload_str}, [self, action, on_result](python::PythonResult pr) {
            if (!self)
                return;
            if (!pr.success) {
                LOG_ERROR("AgentService", QString("%1 failed: %2").arg(action, pr.error.left(200)));
                on_result(false, QJsonObject{{"error", pr.error}});
                return;
            }
            LOG_INFO("AgentService", QString("%1: raw output length=%2, first 300 chars: %3")
                .arg(action).arg(pr.output.size()).arg(pr.output.left(300)));
            QJsonDocument doc = QJsonDocument::fromJson(pr.output.toUtf8());
            if (doc.isNull()) {
                LOG_ERROR("AgentService", QString("%1: invalid JSON response").arg(action));
                on_result(false, QJsonObject{{"error", "Invalid JSON response from Python"}});
                return;
            }
            LOG_INFO("AgentService", QString("%1: parsed JSON keys: %2")
                .arg(action, QStringList(doc.object().keys()).join(", ")));
            on_result(true, doc.object());
        });
}

// ── Python stdin runner (for large payloads) ─────────────────────────────────

void AgentService::run_python_stdin(const QString& action, const QJsonObject& params, const QJsonObject& config,
                                    std::function<void(bool, QJsonObject)> on_result) {
    auto& py = python::PythonRunner::instance();
    if (!py.is_available()) {
        on_result(false, QJsonObject{{"error", "Python not available"}});
        return;
    }

    QJsonObject payload = build_payload(action, params, config);
    QByteArray payload_bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    QString python_path = py.python_path();
    QString script_path = py.scripts_dir() + "/agents/finagent_core/main.py";

    // Spawn QProcess directly for stdin writing (P4 exception like ExchangeService)
    auto* proc = new QProcess(this);
    // Share the standard Python env + cwd + Windows console suppression with
    // PythonRunner so every finagent spawn sees the same OPENMARKETTERMINAL_DATA_DIR,
    // FINAGENT_DATA_DIR, and PYTHONPATH.
    proc->setProcessEnvironment(py.build_python_env());
    proc->setWorkingDirectory(py.scripts_dir());
#ifdef _WIN32
    proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* cpa) {
        cpa->flags |= 0x08000000; // CREATE_NO_WINDOW
    });
#endif
    QPointer<AgentService> self = this;
    auto timer = std::make_shared<QElapsedTimer>();
    timer->start();

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [self, proc, action, on_result, timer](int exit_code, QProcess::ExitStatus) {
                int elapsed = timer->elapsed();

                QString stdout_str = QString::fromUtf8(proc->readAllStandardOutput());
                QString stderr_str = QString::fromUtf8(proc->readAllStandardError());
                proc->deleteLater();

                if (!self)
                    return;

                // Extract JSON from output
                QString json_str = python::extract_json(stdout_str);
                if (json_str.isEmpty() && exit_code == 0) {
                    json_str = stdout_str.trimmed();
                }

                QJsonDocument doc = QJsonDocument::fromJson(json_str.toUtf8());
                if (exit_code != 0 || doc.isNull()) {
                    LOG_ERROR("AgentService", QString("%1 failed (exit=%2, %3ms): %4")
                                                  .arg(action)
                                                  .arg(exit_code)
                                                  .arg(elapsed)
                                                  .arg(stderr_str.left(200)));
                    QString err = stderr_str.isEmpty() ? "Agent execution failed" : stderr_str.left(500);
                    on_result(false, QJsonObject{{"error", err}});
                    return;
                }

                LOG_INFO("AgentService", QString("%1 completed in %2ms").arg(action).arg(elapsed));
                QJsonObject result = doc.object();
                result["execution_time_ms"] = elapsed;
                on_result(true, result);
            });

    connect(proc, &QProcess::errorOccurred, this, [self, proc, action, on_result, timer](QProcess::ProcessError) {
        QString err = proc->errorString();
        proc->deleteLater();
        if (!self)
            return;
        LOG_ERROR("AgentService", QString("%1 process error: %2").arg(action, err));
        on_result(false, QJsonObject{{"error", "Process error: " + err}});
    });

    LOG_INFO("AgentService", QString("Running %1 via stdin (%2 bytes)").arg(action).arg(payload_bytes.size()));
    proc->start(python_path, {script_path, "--stdin"});
    proc->write(payload_bytes);
    proc->closeWriteChannel();
}

// ── Agent discovery ──────────────────────────────────────────────────────────


QString AgentService::make_cache_key(const QString& action, const QJsonObject& params) const {
    return action + "|" + QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact));
}

bool AgentService::get_cached_response(const QString& key, QJsonObject& out) const {
    const QVariant cv = openmarketterminal::CacheManager::instance().get("agents:resp:" + key);
    if (cv.isNull())
        return false;
    out = QJsonDocument::fromJson(cv.toString().toUtf8()).object();
    return true;
}

void AgentService::set_cached_response(const QString& key, const QJsonObject& data) {
    openmarketterminal::CacheManager::instance().put(
        "agents:resp:" + key, QVariant(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact))),
        kResponseCacheTtlSec, "agents");
}

void AgentService::clear_response_cache() {
    openmarketterminal::CacheManager::instance().remove_prefix("agents:resp:");
    LOG_INFO("AgentService", "Response cache cleared");
}

QJsonObject AgentService::get_cache_stats() const {
    QJsonObject stats;
    stats["agents_cached"] = cached_agent_count();
    stats["total_entries"] = openmarketterminal::CacheManager::instance().entry_count();
    return stats;
}

// ── DataHub integration (Phase 9) ─────────────────────────────────────────────

QStringList AgentService::topic_patterns() const {
    // AgentService is push-only: it owns these families but never services a
    // pull-through `refresh()`. Patterns are declared so set_policy_pattern()
    // entries bind to this producer for introspection/stats.
    return {
        QStringLiteral("agent:output:*"),
        QStringLiteral("agent:stream:*"),
        QStringLiteral("agent:status:*"),
        QStringLiteral("agent:routing:*"),
        QStringLiteral("agent:error:*"),
        QStringLiteral("task:event:*"),
    };
}

void AgentService::refresh(const QStringList& /*topics*/) {
    // Push-only — no pull semantics. Run outputs materialise when a caller
    // triggers run_agent/run_team/run_workflow.
}

int AgentService::max_requests_per_sec() const {
    return 0; // push-only, no outbound rate cap
}

void AgentService::ensure_registered_with_hub() {
    if (hub_registered_) return;
    auto& hub = openmarketterminal::datahub::DataHub::instance();
    hub.register_producer(this);

    // Output: short-lived per-run topic, retired on completion.
    openmarketterminal::datahub::TopicPolicy output_policy;
    output_policy.push_only = true;
    output_policy.ttl_ms = 10 * 60 * 1000; // safety net if retire_topic is missed
    hub.set_policy_pattern(QStringLiteral("agent:output:*"), output_policy);

    // Stream: token firehose — coalesce at 50ms so subscribers don't drown.
    openmarketterminal::datahub::TopicPolicy stream_policy;
    stream_policy.push_only = true;
    stream_policy.coalesce_within_ms = 50;
    stream_policy.ttl_ms = 5 * 60 * 1000;
    hub.set_policy_pattern(QStringLiteral("agent:stream:*"), stream_policy);

    // Status: thinking/tool narration — same shape as stream.
    openmarketterminal::datahub::TopicPolicy status_policy;
    status_policy.push_only = true;
    status_policy.coalesce_within_ms = 100;
    status_policy.ttl_ms = 5 * 60 * 1000;
    hub.set_policy_pattern(QStringLiteral("agent:status:*"), status_policy);

    // Routing: one-shot decision per run.
    openmarketterminal::datahub::TopicPolicy routing_policy;
    routing_policy.push_only = true;
    routing_policy.ttl_ms = 10 * 60 * 1000;
    hub.set_policy_pattern(QStringLiteral("agent:routing:*"), routing_policy);

    // Error: rare but important — keep briefly for late subscribers.
    openmarketterminal::datahub::TopicPolicy error_policy;
    error_policy.push_only = true;
    error_policy.ttl_ms = 2 * 60 * 1000;
    hub.set_policy_pattern(QStringLiteral("agent:error:*"), error_policy);

    // Agentic Mode: per-task event stream. One topic per task; retired when
    // a terminal event (done/error/cancelled) lands. Coalesce step_end bursts
    // so high-frequency updates don't overwhelm subscribers.
    openmarketterminal::datahub::TopicPolicy task_event_policy;
    task_event_policy.push_only = true;
    task_event_policy.coalesce_within_ms = 50;
    task_event_policy.ttl_ms = 10 * 60 * 1000;
    hub.set_policy_pattern(QStringLiteral("task:event:*"), task_event_policy);

    hub_registered_ = true;
    LOG_INFO("AgentService", "Registered with DataHub (agent:*)");
}

void AgentService::publish_agent_result(const AgentExecutionResult& r, bool final) {
    if (!hub_registered_ || r.request_id.isEmpty()) return;
    QJsonObject obj{
        {"request_id", r.request_id},
        {"success", r.success},
        {"response", r.response},
        {"error", r.error},
        {"execution_time_ms", r.execution_time_ms},
        {"final", final},
    };
    if (!r.audit.isEmpty())
        obj["audit"] = r.audit;
    const QString topic = QStringLiteral("agent:output:") + r.request_id;
    openmarketterminal::datahub::DataHub::instance().publish(topic, QVariant(obj));
    if (final) {
        // Disposable per-run topic — drop cached state to bound hub memory.
        // Subscribers still pinned via owner remain attached.
        openmarketterminal::datahub::DataHub::instance().retire_topic(topic);
    }
}

static bool agent_sensitive_key(const QString& key) {
    const QString lower = key.toLower();
    return lower.contains("key") || lower.contains("token") || lower.contains("secret") ||
           lower.contains("password") || lower.contains("credential");
}

static QJsonValue agent_redact_json(const QJsonValue& value) {
    if (value.isObject()) {
        QJsonObject out;
        const QJsonObject obj = value.toObject();
        for (auto it = obj.begin(); it != obj.end(); ++it)
            out.insert(it.key(), agent_sensitive_key(it.key()) ? QJsonValue(QStringLiteral("[redacted]"))
                                                               : agent_redact_json(it.value()));
        return out;
    }
    if (value.isArray()) {
        QJsonArray out;
        const QJsonArray arr = value.toArray();
        for (const auto& item : arr)
            out.append(agent_redact_json(item));
        return out;
    }
    return value;
}

static QString agent_preview_json(const QJsonObject& obj, int max_chars = 700) {
    QString text = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    if (text.size() > max_chars)
        text = text.left(max_chars) + QStringLiteral("...");
    return text;
}

void AgentService::begin_agent_audit(const QString& run_id, const QString& action, const QString& query,
                                     const QJsonObject& payload) {
    if (run_id.isEmpty())
        return;

    const QJsonObject active = payload.value("active_llm").toObject();
    const QJsonObject cfg = payload.value("config").toObject();
    const QJsonArray terminal_tools = cfg.value("terminal_tools").toArray();
    const QString provider = active.value("provider").toString();
    const QString model = active.value("model_id").toString();
    const QString base_url = active.value("base_url").toString();
    const bool is_local = provider == QStringLiteral("ollama") ||
                          base_url.startsWith(QStringLiteral("http://127.0.0.1")) ||
                          base_url.startsWith(QStringLiteral("http://localhost"));

    QJsonArray exposed_names;
    for (const auto& v : terminal_tools) {
        const QString name = v.toObject().value("name").toString();
        if (!name.isEmpty())
            exposed_names.append(name);
    }

    QJsonObject audit{
        {"run_id", run_id},
        {"action", action},
        {"started_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"model_provider", provider},
        {"model_id", model},
        {"model_scope", is_local ? QStringLiteral("local") : QStringLiteral("cloud_or_remote")},
        {"terminal_tools_enabled", !terminal_tools.isEmpty()},
        {"tools_exposed_count", terminal_tools.size()},
        {"tools_exposed", exposed_names},
        {"tools_executed_count", 0},
        {"tools_executed", QJsonArray()},
        {"data_sources", QJsonArray()},
        {"query_preview", query.left(700)},
    };
    if (!base_url.isEmpty())
        audit["model_base_url"] = is_local ? base_url : QStringLiteral("[remote endpoint configured]");

    run_audits_.insert(run_id, audit);
}

void AgentService::record_agent_tool_call(const QString& run_id, const QString& tool_name, bool success,
                                          const QJsonObject& args, const QJsonObject& result) {
    if (run_id.isEmpty() || !run_audits_.contains(run_id))
        return;

    QJsonObject audit = run_audits_.value(run_id);
    QJsonArray calls = audit.value("tools_executed").toArray();
    QJsonArray sources = audit.value("data_sources").toArray();

    const QJsonObject safe_args = agent_redact_json(args).toObject();
    const QJsonObject safe_result = agent_redact_json(result).toObject();

    calls.append(QJsonObject{
        {"tool", tool_name},
        {"success", success},
        {"called_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"args", safe_args},
        {"result_preview", agent_preview_json(safe_result)},
    });
    sources.append(tool_name);

    audit["tools_executed"] = calls;
    audit["tools_executed_count"] = calls.size();
    audit["data_sources"] = sources;
    run_audits_.insert(run_id, audit);
}

QJsonObject AgentService::finish_agent_audit(const QString& run_id, const AgentExecutionResult& result) {
    QJsonObject audit = run_audits_.take(run_id);
    if (audit.isEmpty())
        return {};

    audit["finished_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    audit["success"] = result.success;
    audit["execution_time_ms"] = result.execution_time_ms;
    audit["final_answer_preview"] = result.response.left(900);
    if (!result.error.isEmpty())
        audit["error"] = result.error.left(500);
    audit["verdict"] = audit.value("tools_executed_count").toInt() > 0
                           ? QStringLiteral("verified_tool_run")
                           : QStringLiteral("draft_only_no_tools_executed");
    return audit;
}

void AgentService::publish_agent_token(const QString& run_id, const QString& token) {
    if (!hub_registered_ || run_id.isEmpty()) return;
    QJsonObject obj{{"request_id", run_id}, {"token", token}};
    openmarketterminal::datahub::DataHub::instance().publish(
        QStringLiteral("agent:stream:") + run_id, QVariant(obj));
}

void AgentService::publish_agent_status(const QString& run_id, const QString& status) {
    if (!hub_registered_ || run_id.isEmpty()) return;
    QJsonObject obj{{"request_id", run_id}, {"status", status}};
    openmarketterminal::datahub::DataHub::instance().publish(
        QStringLiteral("agent:status:") + run_id, QVariant(obj));
}

void AgentService::publish_routing_result(const RoutingResult& r) {
    if (!hub_registered_ || r.request_id.isEmpty()) return;
    QJsonObject obj{
        {"request_id", r.request_id},
        {"success", r.success},
        {"agent_id", r.agent_id},
        {"intent", r.intent},
        {"confidence", r.confidence},
    };
    openmarketterminal::datahub::DataHub::instance().publish(
        QStringLiteral("agent:routing:") + r.request_id, QVariant(obj));
}

void AgentService::publish_agent_error(const QString& context, const QString& message) {
    if (!hub_registered_) return;
    QJsonObject obj{{"context", context}, {"message", message}};
    openmarketterminal::datahub::DataHub::instance().publish(
        QStringLiteral("agent:error:") + context, QVariant(obj));
}

void AgentService::publish_task_event(const QString& task_id, const QJsonObject& event) {
    if (task_id.isEmpty()) return;
    emit task_event(task_id, event);
    if (!hub_registered_) return;
    const QString topic = QStringLiteral("task:event:") + task_id;
    openmarketterminal::datahub::DataHub::instance().publish(topic, QVariant(event));
    const QString kind = event.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("done") || kind == QStringLiteral("error") ||
        kind == QStringLiteral("cancelled")) {
        // Disposable per-task topic — drop cached state to bound hub memory.
        openmarketterminal::datahub::DataHub::instance().retire_topic(topic);
    }
}


} // namespace openmarketterminal::services
