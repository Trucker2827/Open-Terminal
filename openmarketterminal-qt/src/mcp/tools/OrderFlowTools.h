#pragma once
// OrderFlowTools.h — AI-trading two-phase order flow MCP tools (Phase A).
//
// Task 3 adds the NON-destructive `prepare_order` tool: it parses a structured
// trade intent, validates it, runs a deterministic risk floor, persists a draft,
// audits the decision, and returns a verdict — NO execution. Task 4 ADDs
// `submit_order` to the same group getter below.

#include "mcp/McpTypes.h"

#include <vector>

namespace openmarketterminal::mcp::tools {

/// The order-flow tool group (prepare_order today; submit_order in Task 4).
std::vector<ToolDef> get_order_flow_tools();

} // namespace openmarketterminal::mcp::tools
