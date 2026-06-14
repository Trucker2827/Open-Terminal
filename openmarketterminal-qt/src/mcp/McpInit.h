#pragma once
// McpInit.h — Registers all internal tools and initialises the MCP system.
// Call mcp::initialize_all_tools() during application startup (before QApplication::exec()).

namespace openmarketterminal::mcp {

/// Register only the non-GUI ("core") tools — safe in a headless process.
/// Their backing source files compile into the openterminal_core lib.
void register_core_tools();

/// Register the GUI-only tools (navigation, dashboard, workspace, excel) whose
/// source files are QtWidgets-coupled and live in the GUI exe. Requires a
/// running GUI; call only from the GUI app.
void register_gui_tools();

/// Register all built-in tools and start the MCP system (external servers in background).
/// Equivalent to register_core_tools() + register_gui_tools() plus the post-registration
/// schema audit and external MCP server startup.
void initialize_all_tools();

/// Shutdown the MCP system cleanly.
void shutdown_mcp();

} // namespace openmarketterminal::mcp
