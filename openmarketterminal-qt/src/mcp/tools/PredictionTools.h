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
#include "services/prediction/PredictionTypes.h"

#include <optional>
#include <vector>

#include <QString>

namespace openmarketterminal::mcp::tools {

/// The prediction-market read tool group: pm_search_markets, pm_get_market,
/// pm_get_order_book, pm_list_markets, pm_paper_portfolio.
std::vector<ToolDef> get_prediction_tools();

// ── Shared async→sync bridge fetchers (Task 2 de-dup, reused by Task 4) ───────
//
// These wrap the adapter's CORRELATED + TIMED (15s) broadcast-signal bridge so
// other tool files (OrderFlowTools' prepare_order PM path) can read live PM data
// WITHOUT re-rolling the QMutex/QWaitCondition/connection plumbing. Both are
// READ-ONLY (fetch_market / fetch_order_book only — NEVER place_order). They
// return nullopt on unknown venue, no/empty data, adapter error, or 15s timeout
// — callers treat that as "data unavailable", never a crash.

/// Fetch a single market by (venue, market_id), correlated by market_id.
std::optional<openmarketterminal::services::prediction::PredictionMarket>
pm_fetch_market(const QString& venue, const QString& market_id);

/// Fetch the live order book for (venue, asset_id), correlated by asset_id.
std::optional<openmarketterminal::services::prediction::PredictionOrderBook>
pm_fetch_order_book(const QString& venue, const QString& asset_id);

} // namespace openmarketterminal::mcp::tools
