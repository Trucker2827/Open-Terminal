// McpInitGui.cpp — GUI-only MCP tool registration + full system bring-up.
//
// This translation unit references the QtWidgets-coupled tool getters
// (navigation, dashboard, workspace, excel) whose backing .cpp files depend on
// UI/SCREEN/APP symbols, so it must NOT be part of the headless
// `openterminal_core` library — it compiles into the GUI executable only (Task 3
// places it in MCP_GUI_SOURCES). McpInit.cpp keeps the Core-clean
// register_core_tools()/shutdown_mcp().
//
// initialize_all_tools() preserves its original external behaviour exactly:
// register every tool (core + gui), audit schema sizes, and start the unified
// MCP service. The GUI calls it during startup as before.

#include "mcp/McpInit.h"

#include "core/logging/Logger.h"
#include "mcp/McpProvider.h"
#include "mcp/McpService.h"
#include "mcp/tools/DashboardTools.h"
#include "mcp/tools/DataHubScreenContext.h"
#include "mcp/tools/ExcelTools.h"
#include "mcp/tools/NavigationTools.h"
#include "mcp/tools/NotebookTools.h"
#include "mcp/tools/ReportBuilderTools.h"
#include "mcp/tools/WebTools.h"
#include "mcp/tools/WorkspaceTools.h"

#include <QJsonDocument>

namespace openmarketterminal::mcp {

static constexpr const char* TAG = "McpInitGui";

// One-shot diagnostic: walk every registered ToolDef and warn on any whose
// serialized schema exceeds kSchemaSizeWarnBytes. Common culprits: huge
// enum lists (e.g. exhaustive country/currency codes), verbose multi-line
// descriptions, or deeply nested object schemas. Every byte of schema
// multiplies across every LLM turn that includes the tool.
//
// Logs at INFO if all schemas are within budget; logs WARN with offenders
// (sorted largest-first) otherwise. Runs once at startup; cheap.
static constexpr int kSchemaSizeWarnBytes = 2048;

static void audit_tool_schema_sizes() {
    const auto tools = McpProvider::instance().list_all_tools();
    struct Offender { QString name; int bytes; };
    QVector<Offender> over_budget;
    int total_bytes = 0;
    for (const auto& t : tools) {
        const int bytes = QJsonDocument(t.input_schema).toJson(QJsonDocument::Compact).size();
        total_bytes += bytes;
        if (bytes > kSchemaSizeWarnBytes)
            over_budget.append({t.name, bytes});
    }
    std::sort(over_budget.begin(), over_budget.end(),
              [](const Offender& a, const Offender& b) { return a.bytes > b.bytes; });

    LOG_INFO(TAG, QString("Tool schema audit: %1 tools, %2 KB total schema bytes")
                      .arg(tools.size()).arg(total_bytes / 1024));
    if (over_budget.isEmpty())
        return;

    LOG_WARN(TAG, QString("Tool schema audit: %1 tools exceed %2 B budget — every "
                          "byte multiplies across every LLM turn. Consider trimming "
                          "enums / descriptions / nested objects.")
                      .arg(over_budget.size()).arg(kSchemaSizeWarnBytes));
    for (const auto& o : over_budget)
        LOG_WARN(TAG, QString("  %1 — %2 B").arg(o.name).arg(o.bytes));
}

// Register the GUI-only tool set. These getters are backed by QtWidgets-coupled
// .cpp files (NavigationTools, DashboardTools, WorkspaceTools*, ExcelTools) that
// stay in the GUI exe (excluded from openterminal_core), so this must only be
// called from the GUI app.
void register_gui_tools() {
    auto& provider = McpProvider::instance();

    // navigation
    provider.register_tools(tools::get_navigation_tools());

    // dashboard — widget catalog, layout CRUD, per-widget config, ticker bar
    provider.register_tools(tools::get_dashboard_tools());

    // workspace — monitors, windows, panels, layouts, snapshots, symbol groups, actions, command-bar
    provider.register_tools(tools::get_workspace_tools());

    // excel — sheets, cells, data, rows/cols, CSV export
    provider.register_tools(tools::get_excel_tools());

    // report builder tab — live LLM-driven report authoring (uses ReportBuilderScreen)
    provider.register_tools(tools::get_report_builder_tools());

    // notebooks tab — live AI-authored analysis notebooks (create + open; user runs)
    provider.register_tools(tools::get_notebook_tools());

    // web — headless Chromium search (DuckDuckGo) and fetch
    provider.register_tools(tools::get_web_tools());

    // terminal context — reads the active WindowFrame / focused screen
    provider.register_tools(tools::get_terminal_context_tools());
}

void initialize_all_tools() {
    auto& provider = McpProvider::instance();

    register_core_tools();
    register_gui_tools();

    LOG_INFO(TAG, QString("Registered %1 internal MCP tools").arg(provider.tool_count()));

    // Audit schema sizes once after registration — surfaces bloated tools
    // before they bleed prompt tokens on every turn.
    audit_tool_schema_sizes();

    // Initialize unified service (starts external servers in background)
    McpService::instance().initialize();
}

} // namespace openmarketterminal::mcp
