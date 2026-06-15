// PredictionTools.cpp — Prediction-market READ MCP tools (Phase B, Task 2).
//
// READ-ONLY: list_markets / search / fetch_market / fetch_order_book only.
// These tools NEVER call the adapter's live order-placement / order-cancel
// methods (which would submit real orders).
//
// Each tool bridges the adapter's ASYNC broadcast-signal interface to a SYNC
// ToolResult via detail::run_async_wait. The bridge is CORRELATED (accept only
// the emission whose payload id matches the request; ignore the rest) and TIMED
// (a 15s QTimer::singleShot guarantees the single daemon worker can never wedge
// permanently on a never-matching fetch). Lifetime safety: the result storage is
// worker-stack captured by reference, but every slot/timer guards on a heap
// `finished` flag taken under a shared_ptr mutex BEFORE touching it — the first
// terminal event writes the result then signal_done()s (the worker is still
// blocked at that point), and every later/queued slot observes `finished` and
// returns without dereferencing the (possibly unwound) stack. The value-captured
// shared_ptr `signal_done`/state keep the wait alive for a late timer.

#include "mcp/tools/PredictionTools.h"

#include "mcp/ToolSchemaBuilder.h"
#include "mcp/tools/ThreadHelper.h"
#include "services/prediction/PredictionExchangeAdapter.h"
#include "services/prediction/PredictionExchangeRegistry.h"
#include "services/prediction/PredictionTypes.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>
#include <QVector>

#include <limits>
#include <memory>

namespace openmarketterminal::mcp::tools {

namespace {

namespace pred = openmarketterminal::services::prediction;

constexpr int kBridgeTimeoutMs = 15000;

// ── JSON shaping ─────────────────────────────────────────────────────────────

QJsonObject market_to_json(const pred::PredictionMarket& m) {
    QJsonArray outcomes;
    for (const auto& o : m.outcomes) {
        outcomes.append(QJsonObject{
            {"name", o.name},
            {"asset_id", o.asset_id},
            {"price", o.price},
        });
    }
    return QJsonObject{
        {"market_id", m.key.market_id},
        {"question", m.question},
        {"category", m.category},
        {"end_date_iso", m.end_date_iso},
        {"liquidity", m.liquidity},
        {"volume", m.volume},
        {"outcomes", outcomes},
    };
}

QJsonObject order_book_to_json(const pred::PredictionOrderBook& b) {
    QJsonArray bids;
    bool has_bid = false;
    double best_bid = -std::numeric_limits<double>::infinity();
    for (const auto& lvl : b.bids) {
        bids.append(QJsonObject{{"price", lvl.price}, {"size", lvl.size}});
        if (lvl.price > best_bid) {
            best_bid = lvl.price;
            has_bid = true;
        }
    }

    QJsonArray asks;
    bool has_ask = false;
    double best_ask = std::numeric_limits<double>::infinity();
    for (const auto& lvl : b.asks) {
        asks.append(QJsonObject{{"price", lvl.price}, {"size", lvl.size}});
        if (lvl.price < best_ask) {
            best_ask = lvl.price;
            has_ask = true;
        }
    }

    QJsonObject out{
        {"asset_id", b.asset_id},
        {"tick_size", b.tick_size},
        {"bids", bids},
        {"asks", asks},
    };
    out["best_bid"] = has_bid ? QJsonValue(best_bid) : QJsonValue();
    out["best_ask"] = has_ask ? QJsonValue(best_ask) : QJsonValue();
    out["spread"] = (has_bid && has_ask) ? QJsonValue(best_ask - best_bid) : QJsonValue();
    return out;
}

// ── Bridge result envelope ───────────────────────────────────────────────────
//
// Heap-allocated so the connected slots / timeout timer (which outlive the
// worker's run_async_wait return when a fast match leaves the 15s timer pending)
// only ever touch this shared state, never freed worker stack.
struct BridgeState {
    QMutex m;
    bool finished = false;  // first terminal event wins
    bool ok = false;
    bool timed_out = false;
    QString error;
};

// Resolve the adapter for a venue, or return a clean failure.
pred::PredictionExchangeAdapter* resolve_adapter(const QJsonObject& args, QString& venue_out,
                                                 ToolResult& fail_out) {
    const QString venue = args["venue"].toString().trimmed().toLower();
    if (venue.isEmpty()) {
        fail_out = ToolResult::fail("Missing 'venue'");
        return nullptr;
    }
    venue_out = venue;
    auto* a = pred::PredictionExchangeRegistry::instance().adapter(venue);
    if (!a)
        fail_out = ToolResult::fail("Unknown venue: " + venue);
    return a;
}

// Turn a finished BridgeState into a failure ToolResult if it did not succeed.
// Returns true (and fills `out`) when the call failed; false when it succeeded.
bool bridge_failed(const std::shared_ptr<BridgeState>& st, const QString& what, ToolResult& out) {
    if (st->timed_out) {
        out = ToolResult::fail(what + " timed out after 15s");
        return true;
    }
    if (!st->error.isEmpty()) {
        out = ToolResult::fail(st->error);
        return true;
    }
    if (!st->ok) {
        out = ToolResult::fail("No data for " + what);
        return true;
    }
    return false;
}

} // namespace

std::vector<ToolDef> get_prediction_tools() {
    std::vector<ToolDef> tools;

    // ── pm_search_markets ──────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "pm_search_markets";
        t.description =
            "Search prediction markets on a venue (polymarket, kalshi) by free-text "
            "query. Returns matching markets with their outcomes and asset_ids.";
        t.category = "prediction";
        t.auth_required = AuthLevel::None;
        t.is_destructive = false;
        t.input_schema = ToolSchemaBuilder()
                             .string("venue", "Exchange id (polymarket | kalshi)").required()
                             .string("query", "Free-text search query").required()
                             .integer("limit", "Max results (default 20)").default_int(20).between(1, 100)
                             .build();
        t.handler = [](const QJsonObject& args) -> ToolResult {
            QString venue;
            ToolResult fail;
            auto* a = resolve_adapter(args, venue, fail);
            if (!a)
                return fail;
            const QString query = args["query"].toString().trimmed();
            if (query.isEmpty())
                return ToolResult::fail("Missing 'query'");
            int limit = args["limit"].toInt(20);
            if (limit < 1) limit = 1;

            auto st = std::make_shared<BridgeState>();
            QVector<pred::PredictionMarket> markets;
            detail::run_async_wait(a, [a, query, limit, st, &markets](auto signal_done) {
                auto conns = std::make_shared<QVector<QMetaObject::Connection>>();
                auto finalize = [st, conns, signal_done]() {
                    for (const auto& c : *conns)
                        QObject::disconnect(c);
                    conns->clear();
                    signal_done();
                };
                // No request id for search → accept the first emission.
                conns->append(QObject::connect(
                    a, &pred::PredictionExchangeAdapter::search_results_ready, a,
                    [st, &markets, finalize](const QVector<pred::PredictionMarket>& m,
                                             const QVector<pred::PredictionEvent>&) {
                        QMutexLocker lk(&st->m);
                        if (st->finished)
                            return;
                        markets = m;
                        st->ok = true;
                        st->finished = true;
                        finalize();
                    }));
                conns->append(QObject::connect(
                    a, &pred::PredictionExchangeAdapter::error_occurred, a,
                    [st, finalize](const QString& ctx, const QString& msg) {
                        QMutexLocker lk(&st->m);
                        if (st->finished)
                            return;
                        st->error = msg.isEmpty() ? ctx : (ctx + ": " + msg);
                        st->finished = true;
                        finalize();
                    }));
                QTimer::singleShot(kBridgeTimeoutMs, a, [st, finalize]() {
                    QMutexLocker lk(&st->m);
                    if (st->finished)
                        return;
                    st->timed_out = true;
                    st->finished = true;
                    finalize();
                });
                a->search(query, limit);
            });

            ToolResult out;
            if (bridge_failed(st, "pm_search_markets", out))
                return out;

            QJsonArray arr;
            for (const auto& m : markets)
                arr.append(market_to_json(m));
            return ToolResult::ok_data(QJsonObject{{"markets", arr}});
        };
        tools.push_back(std::move(t));
    }

    // ── pm_get_market ──────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "pm_get_market";
        t.description =
            "Fetch a single prediction market by its market_id on a venue. Returns "
            "the question, category, end date, liquidity/volume and outcomes.";
        t.category = "prediction";
        t.auth_required = AuthLevel::None;
        t.is_destructive = false;
        t.input_schema = ToolSchemaBuilder()
                             .string("venue", "Exchange id (polymarket | kalshi)").required()
                             .string("market_id", "Exchange-scoped market id").required()
                             .build();
        t.handler = [](const QJsonObject& args) -> ToolResult {
            QString venue;
            ToolResult fail;
            auto* a = resolve_adapter(args, venue, fail);
            if (!a)
                return fail;
            const QString market_id = args["market_id"].toString().trimmed();
            if (market_id.isEmpty())
                return ToolResult::fail("Missing 'market_id'");

            auto st = std::make_shared<BridgeState>();
            pred::PredictionMarket market;
            detail::run_async_wait(a, [a, venue, market_id, st, &market](auto signal_done) {
                auto conns = std::make_shared<QVector<QMetaObject::Connection>>();
                auto finalize = [st, conns, signal_done]() {
                    for (const auto& c : *conns)
                        QObject::disconnect(c);
                    conns->clear();
                    signal_done();
                };
                conns->append(QObject::connect(
                    a, &pred::PredictionExchangeAdapter::market_detail_ready, a,
                    [st, market_id, &market, finalize](const pred::PredictionMarket& m) {
                        QMutexLocker lk(&st->m);
                        if (st->finished)
                            return;
                        if (m.key.market_id != market_id)
                            return;  // broadcast for another market → ignore, keep waiting
                        market = m;
                        st->ok = true;
                        st->finished = true;
                        finalize();
                    }));
                conns->append(QObject::connect(
                    a, &pred::PredictionExchangeAdapter::error_occurred, a,
                    [st, finalize](const QString& ctx, const QString& msg) {
                        QMutexLocker lk(&st->m);
                        if (st->finished)
                            return;
                        st->error = msg.isEmpty() ? ctx : (ctx + ": " + msg);
                        st->finished = true;
                        finalize();
                    }));
                QTimer::singleShot(kBridgeTimeoutMs, a, [st, finalize]() {
                    QMutexLocker lk(&st->m);
                    if (st->finished)
                        return;
                    st->timed_out = true;
                    st->finished = true;
                    finalize();
                });
                pred::MarketKey key;
                key.exchange_id = venue;
                key.market_id = market_id;
                a->fetch_market(key);
            });

            ToolResult out;
            if (bridge_failed(st, "pm_get_market", out))
                return out;
            return ToolResult::ok_data(market_to_json(market));
        };
        tools.push_back(std::move(t));
    }

    // ── pm_get_order_book ──────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "pm_get_order_book";
        t.description =
            "Fetch the live order book for a prediction-market asset (outcome) by "
            "asset_id on a venue. Returns best_bid/best_ask/spread and the bid/ask "
            "ladders.";
        t.category = "prediction";
        t.auth_required = AuthLevel::None;
        t.is_destructive = false;
        t.input_schema = ToolSchemaBuilder()
                             .string("venue", "Exchange id (polymarket | kalshi)").required()
                             .string("asset_id", "Exchange-opaque outcome asset id").required()
                             .build();
        t.handler = [](const QJsonObject& args) -> ToolResult {
            QString venue;
            ToolResult fail;
            auto* a = resolve_adapter(args, venue, fail);
            if (!a)
                return fail;
            const QString asset_id = args["asset_id"].toString().trimmed();
            if (asset_id.isEmpty())
                return ToolResult::fail("Missing 'asset_id'");

            auto st = std::make_shared<BridgeState>();
            pred::PredictionOrderBook book;
            detail::run_async_wait(a, [a, asset_id, st, &book](auto signal_done) {
                auto conns = std::make_shared<QVector<QMetaObject::Connection>>();
                auto finalize = [st, conns, signal_done]() {
                    for (const auto& c : *conns)
                        QObject::disconnect(c);
                    conns->clear();
                    signal_done();
                };
                conns->append(QObject::connect(
                    a, &pred::PredictionExchangeAdapter::order_book_ready, a,
                    [st, asset_id, &book, finalize](const pred::PredictionOrderBook& b) {
                        QMutexLocker lk(&st->m);
                        if (st->finished)
                            return;
                        if (b.asset_id != asset_id)
                            return;  // book for another asset → ignore, keep waiting
                        book = b;
                        st->ok = true;
                        st->finished = true;
                        finalize();
                    }));
                conns->append(QObject::connect(
                    a, &pred::PredictionExchangeAdapter::error_occurred, a,
                    [st, finalize](const QString& ctx, const QString& msg) {
                        QMutexLocker lk(&st->m);
                        if (st->finished)
                            return;
                        st->error = msg.isEmpty() ? ctx : (ctx + ": " + msg);
                        st->finished = true;
                        finalize();
                    }));
                QTimer::singleShot(kBridgeTimeoutMs, a, [st, finalize]() {
                    QMutexLocker lk(&st->m);
                    if (st->finished)
                        return;
                    st->timed_out = true;
                    st->finished = true;
                    finalize();
                });
                a->fetch_order_book(asset_id);
            });

            ToolResult out;
            if (bridge_failed(st, "pm_get_order_book", out))
                return out;
            return ToolResult::ok_data(order_book_to_json(book));
        };
        tools.push_back(std::move(t));
    }

    // ── pm_list_markets ────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "pm_list_markets";
        t.description =
            "List prediction markets on a venue, optionally filtered by category and "
            "sorted. Returns markets with their outcomes and asset_ids.";
        t.category = "prediction";
        t.auth_required = AuthLevel::None;
        t.is_destructive = false;
        t.input_schema = ToolSchemaBuilder()
                             .string("venue", "Exchange id (polymarket | kalshi)").required()
                             .string("category", "Category filter (optional)")
                             .string("sort_by", "Sort key (optional, venue-specific)")
                             .integer("limit", "Max results (default 20)").default_int(20).between(1, 100)
                             .build();
        t.handler = [](const QJsonObject& args) -> ToolResult {
            QString venue;
            ToolResult fail;
            auto* a = resolve_adapter(args, venue, fail);
            if (!a)
                return fail;
            const QString category = args["category"].toString();
            const QString sort_by = args["sort_by"].toString();
            int limit = args["limit"].toInt(20);
            if (limit < 1) limit = 1;

            auto st = std::make_shared<BridgeState>();
            QVector<pred::PredictionMarket> markets;
            detail::run_async_wait(a, [a, category, sort_by, limit, st, &markets](auto signal_done) {
                auto conns = std::make_shared<QVector<QMetaObject::Connection>>();
                auto finalize = [st, conns, signal_done]() {
                    for (const auto& c : *conns)
                        QObject::disconnect(c);
                    conns->clear();
                    signal_done();
                };
                // No request id for list → accept the first emission.
                conns->append(QObject::connect(
                    a, &pred::PredictionExchangeAdapter::markets_ready, a,
                    [st, &markets, finalize](const QVector<pred::PredictionMarket>& m) {
                        QMutexLocker lk(&st->m);
                        if (st->finished)
                            return;
                        markets = m;
                        st->ok = true;
                        st->finished = true;
                        finalize();
                    }));
                conns->append(QObject::connect(
                    a, &pred::PredictionExchangeAdapter::error_occurred, a,
                    [st, finalize](const QString& ctx, const QString& msg) {
                        QMutexLocker lk(&st->m);
                        if (st->finished)
                            return;
                        st->error = msg.isEmpty() ? ctx : (ctx + ": " + msg);
                        st->finished = true;
                        finalize();
                    }));
                QTimer::singleShot(kBridgeTimeoutMs, a, [st, finalize]() {
                    QMutexLocker lk(&st->m);
                    if (st->finished)
                        return;
                    st->timed_out = true;
                    st->finished = true;
                    finalize();
                });
                a->list_markets(category, sort_by, limit, 0);
            });

            ToolResult out;
            if (bridge_failed(st, "pm_list_markets", out))
                return out;

            QJsonArray arr;
            for (const auto& m : markets)
                arr.append(market_to_json(m));
            return ToolResult::ok_data(QJsonObject{{"markets", arr}});
        };
        tools.push_back(std::move(t));
    }

    return tools;
}

} // namespace openmarketterminal::mcp::tools
