#pragma once
// PredictionTools.h — Prediction-market READ MCP tools (Phase B, Task 2).
//
// Exposes browse/search/quote tools over the registered prediction-market
// exchange adapters (Polymarket, Kalshi). All tools are READ-ONLY: they call
// only the adapter's list/search/fetch_market/fetch_order_book endpoints —
// NEVER place_order/cancel_order.
//
// The adapter interface is async (signal/slot) and its *_ready signals are
// BROADCAST (the same signal fires for WebSocket pushes, retries and other
// in-flight fetches). Each tool therefore bridges to a synchronous result via
// detail::run_async_wait using a CORRELATED + TIMED handshake (match the
// payload id, ignore non-matching emissions, hard 15s timeout) so a tool can
// never return stale data or wedge the worker permanently.

#include "mcp/McpTypes.h"

#include <vector>

namespace openmarketterminal::mcp::tools {

/// The prediction-market read tool group: pm_search_markets, pm_get_market,
/// pm_get_order_book, pm_list_markets.
std::vector<ToolDef> get_prediction_tools();

} // namespace openmarketterminal::mcp::tools
