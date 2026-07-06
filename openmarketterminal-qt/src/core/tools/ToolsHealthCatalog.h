#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace openmarketterminal::tools_health {

struct Entry {
    const char* screen_id;
    const char* title;
    const char* status;
    const char* lane;
    bool gui;
    bool cli;
    bool mcp;
    bool ai;
    int tool_count;
    const char* command;
    const char* notes;
};

inline const QList<Entry>& tools_menu_entries() {
    static const QList<Entry> rows = {
        {"tools_health", "Tools Health", "WORKING", "diagnostics", true, true, false, false, 0,
         "tools status; coverage --gaps",
         "Self-audit surface for menu health, CLI parity, MCP readiness, and stale screens."},
        {"agent_config", "Agent Config", "WORKING", "strategic", true, true, true, true, 50,
         "agent list|discover|configs|run|team; mcp search agent",
         "Strong AI/agent control surface. Keep expanding team workflows and guardrails."},
        {"mcp_servers", "MCP Servers", "WORKING", "strategic", true, true, true, true, 17,
         "mcp list|search|describe|call; mcp_health",
         "Core tool/server hub. Needs health badges for connected, failed, and setup-required servers."},
        {"data_mapping", "Data Mapping", "PARTIAL", "needs-mcp-create", true, true, false, true, 0,
         "data mapping list|show|run|delete; open data_mapping",
         "CLI can operate saved mappings. GUI remains best for creation; MCP wrappers are still missing."},
        {"data_sources", "Data Sources", "WORKING", "data", true, true, true, true, 12,
         "data connectors|connections|stats; mcp search ds_",
         "Useful connector inventory and saved connection manager. Good candidate for daemon source checks."},
        {"report_builder", "Report Builder", "WORKING", "research-output", true, true, true, true, 18,
         "report state|add|bulk|save|load|tool",
         "High value if fed by decision journal, broker events, and DuckDB evidence tables."},
        {"excel", "Excel", "WORKING", "export", true, true, true, true, 18,
         "excel read|write|append|clear",
         "Useful spreadsheet bridge for portfolios, strategy scorecards, and research exports."},
        {"trade_viz", "Trade Intelligence", "WORKING", "macro-trade", true, true, false, true, 3,
         "tradeviz flow; open trade_viz",
         "Live/fallback trade-flow intelligence with UN Comtrade source, market-impact hints, and CLI parity."},
        {"file_manager", "File Manager", "WORKING", "local-files", true, true, true, true, 13,
         "files list|search|read|write|import|download|delete",
         "Good local artifact manager for datasets, models, reports, and notebooks."},
        {"notes", "Notes", "PARTIAL", "needs-mcp-depth", true, true, true, true, 3,
         "notes list|search|show|create|update|export",
         "Useful, but MCP coverage is thinner than GUI. Add tags, archive, favorite, and update parity."},
    };
    return rows;
}

inline QJsonObject entry_to_json(const Entry& e) {
    return QJsonObject{{"screen_id", QString::fromLatin1(e.screen_id)},
                       {"title", QString::fromLatin1(e.title)},
                       {"status", QString::fromLatin1(e.status)},
                       {"lane", QString::fromLatin1(e.lane)},
                       {"gui", e.gui},
                       {"cli", e.cli},
                       {"mcp", e.mcp},
                       {"ai", e.ai},
                       {"tool_count", e.tool_count},
                       {"command", QString::fromLatin1(e.command)},
                       {"notes", QString::fromLatin1(e.notes)}};
}

inline QJsonArray entries_to_json() {
    QJsonArray arr;
    for (const auto& e : tools_menu_entries())
        arr.append(entry_to_json(e));
    return arr;
}

inline QJsonObject summary_json() {
    int working = 0;
    int partial = 0;
    int potential = 0;
    int static_count = 0;
    int cli_ready = 0;
    int mcp_ready = 0;
    for (const auto& e : tools_menu_entries()) {
        const QString status = QString::fromLatin1(e.status);
        if (status == "WORKING")
            ++working;
        else if (status == "PARTIAL")
            ++partial;
        else if (status == "POTENTIAL")
            ++potential;
        else if (status == "STATIC")
            ++static_count;
        if (e.cli)
            ++cli_ready;
        if (e.mcp)
            ++mcp_ready;
    }
    return QJsonObject{{"total", tools_menu_entries().size()},
                       {"working", working},
                       {"partial", partial},
                       {"potential", potential},
                       {"static", static_count},
                       {"cli_ready", cli_ready},
                       {"mcp_ready", mcp_ready}};
}

} // namespace openmarketterminal::tools_health
