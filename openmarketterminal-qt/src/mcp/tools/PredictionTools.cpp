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
#include "mcp/tools/PmPaperEngine.h"
#include "mcp/tools/ThreadHelper.h"
#include "services/prediction/PredictionExchangeAdapter.h"
#include "services/prediction/PredictionExchangeRegistry.h"
#include "services/prediction/PredictionTypes.h"
#include "storage/repositories/PmPaperRepository.h"

#include <optional>

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

// Best-effort current mid price for an asset via the adapter order book, using
// the shared pm_fetch_order_book bridge. Returns nullopt when the book/adapter
// is unavailable, the request times out, or no two-sided market exists —
// callers must treat that as "price not available", NOT a hard failure (one
// position's network error must never break the portfolio read).
std::optional<double> best_effort_mid(const QString& venue, const QString& asset_id) {
    const auto book = pm_fetch_order_book(venue, asset_id);
    if (!book)
        return std::nullopt;

    double best_bid = -std::numeric_limits<double>::infinity();
    bool has_bid = false;
    for (const auto& lvl : book->bids)
        if (lvl.price > best_bid) {
            best_bid = lvl.price;
            has_bid = true;
        }
    double best_ask = std::numeric_limits<double>::infinity();
    bool has_ask = false;
    for (const auto& lvl : book->asks)
        if (lvl.price < best_ask) {
            best_ask = lvl.price;
            has_ask = true;
        }

    if (has_bid && has_ask)
        return (best_bid + best_ask) / 2.0;
    if (has_bid)
        return best_bid;
    if (has_ask)
        return best_ask;
    return std::nullopt;
}

} // namespace

// ── Shared async→sync bridge fetchers (exported; see PredictionTools.h) ───────
//
// Centralize the CORRELATED + TIMED broadcast bridge once so both the read
// tools below AND OrderFlowTools' prepare_order PM path share a single audited
// implementation. nullopt on unknown venue / no data / error / 15s timeout.

std::optional<pred::PredictionOrderBook> pm_fetch_order_book(const QString& venue,
                                                             const QString& asset_id) {
    const QString v = venue.trimmed().toLower();
    if (v.isEmpty() || asset_id.isEmpty())
        return std::nullopt;
    auto* a = pred::PredictionExchangeRegistry::instance().adapter(v);
    if (!a)
        return std::nullopt;

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

    if (!st->ok)
        return std::nullopt;
    return book;
}

std::optional<pred::PredictionMarket> pm_fetch_market(const QString& venue,
                                                      const QString& market_id) {
    const QString v = venue.trimmed().toLower();
    if (v.isEmpty() || market_id.isEmpty())
        return std::nullopt;
    auto* a = pred::PredictionExchangeRegistry::instance().adapter(v);
    if (!a)
        return std::nullopt;

    auto st = std::make_shared<BridgeState>();
    pred::PredictionMarket market;
    detail::run_async_wait(a, [a, v, market_id, st, &market](auto signal_done) {
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
        key.exchange_id = v;
        key.market_id = market_id;
        a->fetch_market(key);
    });

    if (!st->ok)
        return std::nullopt;
    return market;
}

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

            const auto market = pm_fetch_market(venue, market_id);
            if (!market)
                return ToolResult::fail("No data for pm_get_market on " + venue);
            return ToolResult::ok_data(market_to_json(*market));
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

            const auto book = pm_fetch_order_book(venue, asset_id);
            if (!book)
                return ToolResult::fail("No data for pm_get_order_book on " + venue);
            return ToolResult::ok_data(order_book_to_json(*book));
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

    // ── pm_paper_portfolio ─────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "pm_paper_portfolio";
        t.description =
            "Read the prediction-market PAPER portfolio: cash balance plus every "
            "open paper position with its cost basis, and — best-effort — the live "
            "current price (order-book mid) and unrealized P&L. Positions whose live "
            "book is unavailable are returned without a current_price/unrealized_pnl.";
        t.category = "prediction";
        t.auth_required = AuthLevel::None;
        t.is_destructive = false;
        t.input_schema = ToolSchemaBuilder().build();
        t.handler = [](const QJsonObject&) -> ToolResult {
            auto& repo = PmPaperRepository::instance();

            auto cash = repo.cash();
            if (cash.is_err())
                return ToolResult::fail(QString::fromStdString(cash.error()));

            auto open = repo.list_open();
            if (open.is_err())
                return ToolResult::fail(QString::fromStdString(open.error()));

            QJsonArray positions;
            double total_unrealized = 0.0;
            for (const PmPosition& p : open.value()) {
                QJsonObject row{
                    {"venue", p.venue},
                    {"market_id", p.market_id},
                    {"asset_id", p.asset_id},
                    {"outcome", p.outcome},
                    {"contracts", p.contracts},
                    {"avg_price", p.avg_price},
                    {"cost_basis", p.cost_basis},
                };
                // Best-effort live price — a failure on one position must not break
                // the whole portfolio read.
                if (auto mid = best_effort_mid(p.venue, p.asset_id)) {
                    const double pnl = mark_to_market(p, *mid);
                    row["current_price"] = *mid;
                    row["unrealized_pnl"] = pnl;
                    total_unrealized += pnl;
                }
                positions.append(row);
            }

            return ToolResult::ok_data(QJsonObject{
                {"cash", cash.value()},
                {"positions", positions},
                {"total_unrealized", total_unrealized},
            });
        };
        tools.push_back(std::move(t));
    }

    return tools;
}

} // namespace openmarketterminal::mcp::tools
