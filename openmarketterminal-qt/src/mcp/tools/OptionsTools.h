#pragma once
// OptionsTools.h — read-only option-contract discovery MCP tool.

#include "mcp/McpTypes.h"

#include <vector>

namespace openmarketterminal::mcp::tools {

std::vector<ToolDef> get_options_tools();

} // namespace openmarketterminal::mcp::tools
