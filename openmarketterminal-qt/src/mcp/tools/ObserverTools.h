#pragma once
#include "mcp/McpTypes.h"

#include <vector>

namespace openmarketterminal::mcp::tools {
// Read-only MCP tool over ObserverJournalService — exposes the headless observer's
// journal (daily lesson / weekly review / alerts). No write, no recompute, no trading.
std::vector<ToolDef> get_observer_tools();
} // namespace openmarketterminal::mcp::tools
