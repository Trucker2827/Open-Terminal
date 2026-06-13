#pragma once

namespace openmarketterminal::workflow {
class NodeRegistry;

/// Register control flow node types (IfElse, Switch, Loop, Split, Merge, Wait, etc.)
void register_control_flow_nodes(NodeRegistry& registry);

} // namespace openmarketterminal::workflow
