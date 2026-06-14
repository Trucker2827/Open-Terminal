#pragma once
// DataHubScreenContext.h — GUI-coupled "screen context" helpers + tool.
//
// These read the active WindowFrame / focused screen (QtWidgets) in addition to
// the DataHub, so they CANNOT live in the Core-clean DataHubPeekHelpers TU. The
// `get_terminal_context` MCP tool is registered via register_gui_tools() only —
// register_core_tools() must never reach build_terminal_context_json().

#include "mcp/McpTypes.h"

#include <QJsonObject>
#include <QString>

#include <vector>

namespace openmarketterminal::mcp::tools {

namespace detail {

/// Compact prefix injected into Quick Chat when hub has live quote topics.
/// (Reads only DataHub today, but is grouped here as screen-context and is only
/// reachable from GUI callers — kept out of the Core peek TU by design.)
QString build_screen_context_brief();

/// JSON snapshot for the `get_terminal_context` MCP tool. Reads the active
/// WindowFrame / focused screen via QtWidgets — GUI-only.
QJsonObject build_terminal_context_json();

} // namespace detail

/// GUI-only getter: registers the `get_terminal_context` tool. Backed by
/// build_terminal_context_json() (QtWidgets-coupled), so this is registered from
/// register_gui_tools() and lives in the GUI executable only.
std::vector<ToolDef> get_terminal_context_tools();

} // namespace openmarketterminal::mcp::tools
