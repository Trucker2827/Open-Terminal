#pragma once

namespace openmarketterminal::services {

/// Register the headless-registerable data-producing services with the DataHub.
/// This is the single source of truth for the data services the GUI-free core /
/// headless host brings up, and mirrors register_all_migrations().
///
/// This covers 10 of the GUI's 11 data services. RelationshipMapService is the
/// one omission: it lives in the GUI tier and transitively pulls in Qt6::Gui
/// (QColor), so it cannot link into the Widgets/Gui-free openterminal_core, and
/// no headless MCP tool consumes its producer. The GUI registers it separately.
/// See DataServices.cpp for the full rationale.
///
/// Registration is CHEAP producer-wiring only: each service's
/// ensure_registered_with_hub() does register_producer() + set_policy_pattern()
/// and nothing else (no fetch, no socket connect, no subprocess spawn, no
/// blocking wait). The actual feed/fetch starts lazily on explicit subscription
/// or request — which a one-shot CLI never triggers — so calling this from a
/// headless host is one-shot-safe. (AgentService's constructor additionally
/// guards its TCP-bridge bootstrap so it only runs in a real GUI process; see
/// AgentService.cpp.)
void register_all_data_services();

}  // namespace openmarketterminal::services
