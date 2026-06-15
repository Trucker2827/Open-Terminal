#pragma once
#include "mcp/McpTypes.h"

#include <vector>

// FastLiveTools — Fast Live Mode (Phase D) read tools.
//
// The fast-live tool set is reachable ONLY when fully fast-armed
// (cli_trading_allowed() && cli_live_armed() && cli_fast_live_armed()); the
// three host auth-checkers enforce that predicate up front (Task 1). These
// tools add a second, revocable runtime gate (kill switch + allowed account)
// and route EXCLUSIVELY to cli.allowed_account — never an arg-supplied account.
//
// Category MUST be "fast-live" (NOT "live-trading") and the reads are
// is_destructive=false, so is_live_execution_tool ("live-trading" &&
// destructive) does NOT deny them before the fast-arm gate fires.
namespace openmarketterminal::mcp::tools {
std::vector<ToolDef> get_fast_live_tools();
} // namespace openmarketterminal::mcp::tools
