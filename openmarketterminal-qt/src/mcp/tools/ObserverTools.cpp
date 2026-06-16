// ObserverTools.cpp — read-only MCP access to the headless observer's journal.
// Mirrors the `observe` CLI: one tool, `get_observations`, backed by the same
// ObserverJournalService. Renders exactly what the Python observer wrote — no
// recomputation, no order path. Core-clean (no QtWidgets) so it registers in
// register_core_tools() and is safe headless.

#include "mcp/tools/ObserverTools.h"
#include "services/observer/ObserverJournalService.h"

#include <QJsonArray>
#include <QJsonObject>

namespace openmarketterminal::mcp::tools {

using services::ObserverJournalService;

static QJsonObject block_json(const ObserverJournalService::Block& b) {
    return QJsonObject{{"title", b.title}, {"markdown", b.markdown},
                       {"alert", b.alert.isEmpty() ? QJsonValue() : QJsonValue(b.alert)}};
}

std::vector<ToolDef> get_observer_tools() {
    std::vector<ToolDef> tools;
    {
        ToolDef t;
        t.name = "get_observations";
        t.description =
            "Read-only view of the headless market observer's journal for BTC/ETH: the "
            "daily trade-lesson (regime, relative strength, volatility, watch-levels, a "
            "PAPER-only idea + the honest no-action reason), the weekly review, or recent "
            "alerts. Rendered exactly as the observer wrote it — no recomputation, no "
            "trading. view: 'latest' (newest daily, default) | 'week' | 'alerts'.";
        t.category = "observer";
        t.input_schema.properties = QJsonObject{
            {"view", QJsonObject{{"type", "string"},
                                 {"enum", QJsonArray{"latest", "week", "alerts"}},
                                 {"description", "latest | week | alerts (default latest)"}}},
            {"limit", QJsonObject{{"type", "integer"},
                                  {"description", "max alerts to return for the alerts view (default 10)"}}}};
        t.input_schema.required = {};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            auto& svc = ObserverJournalService::instance();
            if (!svc.available())
                return ToolResult::fail("observer journal not found at " + svc.journalDir() +
                                        "; set OPENTERMINAL_OBSERVER_DIR to the trading-mcp-server directory");

            const QString view = args.value("view").toString("latest");

            if (view == "week") {
                const auto b = svc.latestWeekly();
                QJsonObject o{{"view", "week"}};
                if (b) { o["title"] = b->title; o["markdown"] = b->markdown; }
                else o["message"] = "no weekly review yet";
                return ToolResult::ok_data(o);
            }
            if (view == "alerts") {
                int limit = args.value("limit").toInt(10);
                if (limit <= 0) limit = 10;
                QJsonArray arr;
                for (const auto& b : svc.recentAlerts(limit)) arr.append(block_json(b));
                return ToolResult::ok_data(QJsonObject{{"view", "alerts"}, {"alerts", arr}});
            }
            // default: latest daily, with the matching machine-readable history record
            const auto b = svc.latestDaily();
            QJsonObject o{{"view", "latest"}};
            if (b) {
                o["title"]    = b->title;
                o["markdown"] = b->markdown;
                o["alert"]    = b->alert.isEmpty() ? QJsonValue() : QJsonValue(b->alert);
                const QJsonArray h = svc.history(1);
                o["history"]  = h.isEmpty() ? QJsonValue() : h.last();
            } else {
                o["message"] = "no daily observations yet";
            }
            return ToolResult::ok_data(o);
        };
        tools.push_back(std::move(t));
    }
    return tools;
}

} // namespace openmarketterminal::mcp::tools
