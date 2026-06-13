#pragma once

namespace openmarketterminal::workflow {
class NodeRegistry;

/// Register trigger node types (ManualTrigger, ScheduleTrigger, PriceAlert, etc.)
void register_trigger_nodes(NodeRegistry& registry);

} // namespace openmarketterminal::workflow
