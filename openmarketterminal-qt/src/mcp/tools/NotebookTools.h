#pragma once
#include "mcp/McpTypes.h"

#include <vector>

namespace openmarketterminal::mcp::tools {
// GUI tool: the in-app AI authors a runnable Python notebook into the Notebooks
// screen (analysis the user can run/edit). Touches CodeEditorScreen, so it is a
// GUI-registered tool (register_gui_tools). The user runs the cells.
std::vector<ToolDef> get_notebook_tools();
} // namespace openmarketterminal::mcp::tools
