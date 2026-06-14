#include "services/DataServices.h"

#include "services/agents/AgentService.h"
#include "services/dbnomics/DBnomicsService.h"
#include "services/economics/EconomicsService.h"
#include "services/economics/MacroCalendarService.h"
#include "services/geopolitics/GeopoliticsService.h"
#include "services/gov_data/GovDataService.h"
#include "services/ma_analytics/MAAnalyticsService.h"
#include "services/maritime/MaritimeService.h"
#include "services/markets/MarketDataService.h"
#include "services/news/NewsService.h"

// NOTE on RelationshipMapService: the GUI registers it (main.cpp) as an 11th
// data producer, but it lives in the GUI tier (SCREEN_SOURCES) and transitively
// depends on Qt6::Gui (QColor, via screens/relationship_map/RelationshipMapTypes.h),
// so it CANNOT be linked into the Widgets/Gui-free openterminal_core that this
// helper belongs to. It is also consumed only by its own GUI screen — no MCP
// tool in the headless catalog reads its geopolitics:relationship_graph:*
// producer — so wiring it headless would be dead wiring even if it linked.
// Hence the headless-registerable set is these 10 data services; the GUI keeps
// registering RelationshipMapService itself.

namespace openmarketterminal::services {

void register_all_data_services() {
    // Each call is idempotent (guarded by the service's hub_registered_ flag)
    // and cheap (producer + policy-pattern wiring only). Order is irrelevant.
    MarketDataService::instance().ensure_registered_with_hub();
    NewsService::instance().ensure_registered_with_hub();
    EconomicsService::instance().ensure_registered_with_hub();
    MacroCalendarService::instance().ensure_registered_with_hub();
    geo::GeopoliticsService::instance().ensure_registered_with_hub();
    maritime::MaritimeService::instance().ensure_registered_with_hub();
    ma::MAAnalyticsService::instance().ensure_registered_with_hub();
    DBnomicsService::instance().ensure_registered_with_hub();
    GovDataService::instance().ensure_registered_with_hub();
    AgentService::instance().ensure_registered_with_hub();
}

}  // namespace openmarketterminal::services
