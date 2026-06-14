// McpInit.cpp — Register the non-GUI ("core") MCP tools.
//
// This translation unit is Core-clean: it references only tool getters whose
// backing .cpp files are free of QtWidgets/UI/SCREEN/APP coupling, so it can be
// compiled into the headless `openterminal_core` library and linked into a
// process with no GUI. The GUI-only registration (register_gui_tools) and the
// full bring-up (initialize_all_tools) live in McpInitGui.cpp, which references
// the QtWidgets-coupled tool files and therefore stays in the GUI executable.

#include "mcp/McpInit.h"

#include "core/logging/Logger.h"
#include "mcp/McpProvider.h"
#include "mcp/McpService.h"
#include "mcp/tools/AgentsTools.h"
#include "mcp/tools/AiChatTools.h"
#include "mcp/tools/AltInvestmentsTools.h"
#include "mcp/tools/CryptoTradingTools.h"
#include "mcp/tools/DataHubTools.h"
#include "mcp/tools/DataSourcesTools.h"
#include "mcp/tools/DBnomicsTools.h"
#include "mcp/tools/EdgarTools.h"
#include "mcp/tools/EquityResearchTools.h"
#include "mcp/tools/FileManagerTools.h"
#include "mcp/tools/GeopoliticsTools.h"
#include "mcp/tools/GovDataTools.h"
#include "mcp/tools/LiveTradingTools.h"
#include "mcp/tools/MAAnalyticsTools.h"
#include "mcp/tools/MarketsTools.h"
#include "mcp/tools/McpServersTools.h"
#include "mcp/tools/MetaTools.h"
#include "mcp/tools/NewsTools.h"
#include "mcp/tools/AgenticMemoryTools.h"
#include "mcp/tools/NotesTools.h"
#include "mcp/tools/PaperTradingTools.h"
#include "mcp/tools/PortfolioTools.h"
#include "mcp/tools/ProfileTools.h"
#include "mcp/tools/PythonTools.h"
#include "mcp/tools/QuantLabTools.h"
#include "mcp/tools/SettingsTools.h"
#include "mcp/tools/SurfaceAnalyticsTools.h"
#include "mcp/tools/SystemTools.h"
#include "mcp/tools/WatchlistTools.h"

namespace openmarketterminal::mcp {

static constexpr const char* TAG = "McpInit";

// Register the non-GUI ("core") tool set. Every getter here is backed by a
// Core-clean .cpp (no QtWidgets) that compiles into openterminal_core, so this
// function is safe to call from a headless process. The four GUI-coupled tool
// groups (navigation, dashboard, workspace, excel) are registered separately by
// register_gui_tools() (see McpInitGui.cpp).
void register_core_tools() {
    auto& provider = McpProvider::instance();

    // news
    provider.register_tools(tools::get_news_tools());

    // markets tab (quotes, symbol search)
    provider.register_tools(tools::get_markets_tools());

    // watchlist tab
    provider.register_tools(tools::get_watchlist_tools());

    // portfolio tab (holdings + named portfolios/assets/transactions/snapshots)
    provider.register_tools(tools::get_portfolio_tools());

    // notes tab
    provider.register_tools(tools::get_notes_tools());

    // agentic mode — Letta tier-3 archival memory (agent-callable mid-step)
    provider.register_tools(tools::get_agentic_memory_tools());

    // ai chat tab
    provider.register_tools(tools::get_ai_chat_tools());

    // crypto trading tab
    provider.register_tools(tools::get_crypto_trading_tools());

    // paper trading tab
    provider.register_tools(tools::get_paper_trading_tools());

    // live broker trading (order placement/cancel, account state, market data)
    provider.register_tools(tools::get_live_trading_tools());

    // sec edgar (CIK resolution, XBRL financials, filing search)
    provider.register_tools(tools::get_edgar_tools());

    // m&a analytics tab
    provider.register_tools(tools::get_ma_analytics_tools());

    // alt investments tab
    provider.register_tools(tools::get_alt_investments_tools());

    // data sources tab
    provider.register_tools(tools::get_data_sources_tools());

    // profile tab
    provider.register_tools(tools::get_profile_tools());

    // file manager tab
    provider.register_tools(tools::get_file_manager_tools());

    // settings, python, system
    provider.register_tools(tools::get_settings_tools());
    provider.register_tools(tools::get_python_tools());
    provider.register_tools(tools::get_system_tools());

    // datahub introspection (Phase 9)
    provider.register_tools(tools::get_datahub_tools());

    // external mcp server management (list/install/start/stop/call-through)
    provider.register_tools(tools::get_mcp_servers_tools());

    // ai quant lab — 24-module quantitative research platform (96 specific + 3 generic)
    provider.register_tools(tools::get_quant_lab_tools());

    // agent studio — discovery, execution, planner, memory, config CRUD
    provider.register_tools(tools::get_agents_tools());

    // dbnomics — economic data series (providers/datasets/series/observations/search)
    provider.register_tools(tools::get_dbnomics_tools());

    // gov-data — 10 government providers (US Treasury/Congress, France, HK, UK, Australia, ...)
    provider.register_tools(tools::get_gov_data_tools());

    // equity-research — symbol search, load, financials, technicals, peers, news, sentiment
    provider.register_tools(tools::get_equity_research_tools());

    // geopolitics — events, HDX, trade analysis, geolocations
    provider.register_tools(tools::get_geopolitics_tools());

    // surface-analytics — 35-surface capability catalog + Databento fetches
    provider.register_tools(tools::get_surface_analytics_tools());

    // Phase 6: meta tools — tool_list, tool_describe, mcp_health.
    // Always exposed so the LLM can lazy-discover specialised tools.
    provider.register_tools(tools::get_meta_tools());
}

void shutdown_mcp() {
    McpService::instance().shutdown();
    LOG_INFO(TAG, "MCP system shut down");
}

} // namespace openmarketterminal::mcp
