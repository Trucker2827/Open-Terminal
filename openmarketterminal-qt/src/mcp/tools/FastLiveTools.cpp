// FastLiveTools.cpp — Fast Live Mode (Phase D) live READ tools + shared gate.
//
// Three gated, read-only wrappers over the broker, scoped to cli.allowed_account:
//
//   • get_positions   → broker->get_positions  → {positions:[...]}
//   • get_open_orders → broker->get_orders      → {orders:[...]}  (non-terminal)
//   • get_fills       → broker->get_orders      → {fills:[...]}   (filled status)
//
// Every tool calls fast_live_gate() FIRST: kill switch → the three arms →
// allowed-account resolution. The host auth-checker already requires the three
// arms before the handler runs; the in-handler gate adds the revocable kill
// switch + allowed-account checks and is the single place those reasons surface.
//
// get_fills sourcing: there is no distinct broker fills/array accessor — the
// only trade-book op (IBroker::get_trade_book) returns an opaque,
// broker-specific QJsonObject, not the {fills:[...]} array the shape calls for.
// Per the task's escape hatch we derive fills from get_orders() filtered to a
// filled status (the project normalises BrokerOrderInfo.status to lowercase
// "filled"/"open"/"cancelled"/"rejected"; see tradier/ibkr/metaapi adapters).
//
// Tools NEVER throw and NEVER take an account argument; a closed gate or a
// broker error is reported as ok_data({status:"rejected", reason:...}) so the
// agent sees a structured refusal rather than a tool failure.

#include "mcp/tools/FastLiveTools.h"

#include "core/logging/Logger.h"
#include "mcp/ToolSchemaBuilder.h"
#include "mcp/tools/SettingsGate.h"
#include "storage/repositories/TradeAuditRepository.h"
#include "trading/AccountManager.h"
#include "trading/BrokerInterface.h"
#include "trading/TradingTypes.h"
#include "trading/UnifiedTrading.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace openmarketterminal::mcp::tools {

static constexpr const char* TAG = "FastLiveTools";

namespace {

using namespace openmarketterminal::trading;

// Outcome of the shared fast-live runtime gate.
struct FastGate {
    bool ok = false;
    QString reason;  // caller-friendly refusal reason when !ok
    QString account; // resolved cli.allowed_account when ok
};

// The shared fast-live runtime gate, applied by EVERY fast-live tool before it
// touches the broker. Mirrors the constitution's order of precedence:
//   1. kill switch — the panic button overrides everything.
//   2. the three arms — base trading + base live arm + the SECOND fast arm.
//      (The host auth-checker already enforces this, but re-checking here keeps
//      the handler safe if ever reached on a non-gated path and keeps the gate
//      self-contained.)
//   3. allowed account — the AI may only ever read the single GUI-named account;
//      it can never supply its own. All reads hit LIVE broker state (revocable).
FastGate fast_live_gate() {
    if (mcp::cli_kill_switch_engaged())
        return {false, "kill switch engaged", {}};
    if (!(mcp::cli_trading_allowed() && mcp::cli_live_armed() && mcp::cli_fast_live_armed()))
        return {false, "fast live mode not armed — arm in GUI Settings", {}};
    const QString acct = mcp::cli_allowed_account();
    if (acct.isEmpty() || !AccountManager::instance().has_account(acct))
        return {false, "no allowed account configured for AI trading", {}};
    return {true, {}, acct};
}

// Structured refusal: a closed gate / unavailable broker / broker error is NOT a
// tool failure — the agent gets {status:"rejected", reason:...} so it can react.
ToolResult rejected(const QString& reason) {
    return ToolResult::ok_data(QJsonObject{{"status", "rejected"}, {"reason", reason}});
}

// Project-normalised order status vocabulary (see broker adapters). Defined ONCE
// so the get_fills / get_open_orders partitions can never drift apart. Match
// case-insensitively and tolerate the common synonyms across broker vocabularies.
bool is_filled_status(const QString& status) {
    const QString u = status.trimmed().toLower();
    return u == "filled" || u == "complete" || u == "completed" || u == "executed" || u == "traded";
}

bool is_terminal_status(const QString& status) {
    if (is_filled_status(status))
        return true;
    const QString u = status.trimmed().toLower();
    return u == "cancelled" || u == "canceled" || u == "rejected" || u == "expired";
}

QJsonObject position_to_json(const BrokerPosition& p) {
    return QJsonObject{{"symbol", p.symbol},
                       {"exchange", p.exchange},
                       {"product_type", p.product_type},
                       {"quantity", p.quantity},
                       {"avg_price", p.avg_price},
                       {"ltp", p.ltp},
                       {"pnl", p.pnl},
                       {"pnl_pct", p.pnl_pct},
                       {"day_pnl", p.day_pnl},
                       {"side", p.side}};
}

QJsonObject order_to_json(const BrokerOrderInfo& o) {
    return QJsonObject{{"order_id", o.order_id},
                       {"exchange_order_id", o.exchange_order_id},
                       {"symbol", o.symbol},
                       {"exchange", o.exchange},
                       {"side", o.side},
                       {"order_type", o.order_type},
                       {"product_type", o.product_type},
                       {"quantity", o.quantity},
                       {"price", o.price},
                       {"trigger_price", o.trigger_price},
                       {"filled_qty", o.filled_qty},
                       {"avg_price", o.avg_price},
                       {"status", o.status},
                       {"timestamp", o.timestamp},
                       {"message", o.message}};
}

// Resolve the broker for the gated account. On failure returns nullptr and sets
// `reason`; callers turn that into a structured rejection (never a throw).
IBroker* resolve_gated_broker(const QString& account, BrokerCredentials& creds, QString& reason) {
    auto& mgr = AccountManager::instance();
    IBroker* broker = mgr.broker_for(account);
    if (!broker) {
        reason = "broker unavailable";
        return nullptr;
    }
    creds = mgr.load_credentials(account);
    return broker;
}

// Append one append-only trade_audit row for a fast-live de-risking action. Every
// terminal path (gate-denied or broker-returned) records a row; `reason` must be
// non-empty (trade_audit.reason is NOT NULL and a blank reason is useless in the
// log). Risk snapshot is empty — these tools only REDUCE exposure, so there is no
// risk floor to record.
void fast_derisk_audit(const QString& tool, const QString& account, const QString& decision,
                       const QString& reason, const QJsonObject& intent) {
    TradeAuditRow row;
    row.ts = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    row.phase = QStringLiteral("fast-live");
    row.tool = tool;
    row.account = account;
    row.mode = QStringLiteral("live");
    row.intent_json = QString::fromUtf8(QJsonDocument(intent).toJson(QJsonDocument::Compact));
    row.decision = decision;
    row.reason = reason;
    row.risk_snapshot_json = QStringLiteral("{}");
    auto r = TradeAuditRepository::instance().append(row);
    if (!r.is_ok())
        LOG_WARN(TAG, "fast trade_audit append failed: " + QString::fromStdString(r.error()));
}

} // namespace

std::vector<ToolDef> get_fast_live_tools() {
    std::vector<ToolDef> tools;

    // ── get_positions ──────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_positions";
        t.description =
            "Fast Live Mode: current open positions for the AI's allowed live account.";
        t.category = "fast-live";
        t.auth_required = AuthLevel::None;
        t.is_destructive = false;
        t.handler = [](const QJsonObject&) -> ToolResult {
            auto g = fast_live_gate();
            if (!g.ok)
                return rejected(g.reason);

            BrokerCredentials creds;
            QString reason;
            IBroker* broker = resolve_gated_broker(g.account, creds, reason);
            if (!broker)
                return rejected(reason);

            auto resp = broker->get_positions(creds);
            if (!resp.success)
                return rejected(resp.error.isEmpty() ? "broker error fetching positions"
                                                     : resp.error);

            QJsonArray positions;
            for (const auto& p : resp.data.value_or(QVector<BrokerPosition>{}))
                positions.append(position_to_json(p));
            LOG_INFO(TAG, QString("fast get_positions: %1 (%2)")
                              .arg(positions.size()).arg(g.account));
            return ToolResult::ok_data(QJsonObject{{"positions", positions}});
        };
        tools.push_back(std::move(t));
    }

    // ── get_open_orders ────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_open_orders";
        t.description =
            "Fast Live Mode: working (non-terminal) orders for the AI's allowed live account.";
        t.category = "fast-live";
        t.auth_required = AuthLevel::None;
        t.is_destructive = false;
        t.handler = [](const QJsonObject&) -> ToolResult {
            auto g = fast_live_gate();
            if (!g.ok)
                return rejected(g.reason);

            BrokerCredentials creds;
            QString reason;
            IBroker* broker = resolve_gated_broker(g.account, creds, reason);
            if (!broker)
                return rejected(reason);

            auto resp = broker->get_orders(creds);
            if (!resp.success)
                return rejected(resp.error.isEmpty() ? "broker error fetching orders"
                                                     : resp.error);

            QJsonArray orders;
            for (const auto& o : resp.data.value_or(QVector<BrokerOrderInfo>{}))
                if (!is_terminal_status(o.status))
                    orders.append(order_to_json(o));
            LOG_INFO(TAG, QString("fast get_open_orders: %1 (%2)")
                              .arg(orders.size()).arg(g.account));
            return ToolResult::ok_data(QJsonObject{{"orders", orders}});
        };
        tools.push_back(std::move(t));
    }

    // ── get_fills ──────────────────────────────────────────────────────────
    // Sourced from get_orders() filtered to a filled status — there is no
    // distinct broker fills-array accessor (get_trade_book returns opaque
    // broker JSON). Filled is the complement-sharing predicate of get_open_orders.
    {
        ToolDef t;
        t.name = "get_fills";
        t.description =
            "Fast Live Mode: filled (executed) orders for the AI's allowed live account.";
        t.category = "fast-live";
        t.auth_required = AuthLevel::None;
        t.is_destructive = false;
        t.handler = [](const QJsonObject&) -> ToolResult {
            auto g = fast_live_gate();
            if (!g.ok)
                return rejected(g.reason);

            BrokerCredentials creds;
            QString reason;
            IBroker* broker = resolve_gated_broker(g.account, creds, reason);
            if (!broker)
                return rejected(reason);

            auto resp = broker->get_orders(creds);
            if (!resp.success)
                return rejected(resp.error.isEmpty() ? "broker error fetching fills"
                                                     : resp.error);

            QJsonArray fills;
            for (const auto& o : resp.data.value_or(QVector<BrokerOrderInfo>{}))
                if (is_filled_status(o.status))
                    fills.append(order_to_json(o));
            LOG_INFO(TAG, QString("fast get_fills: %1 (%2)")
                              .arg(fills.size()).arg(g.account));
            return ToolResult::ok_data(QJsonObject{{"fills", fills}});
        };
        tools.push_back(std::move(t));
    }

    // ── cancel_order (DE-RISKING, destructive) ───────────────────────────────
    // Cancel a single open LIVE order on the AI's allowed account. De-risking →
    // NO order-value risk floor, but the full fast-live gate still applies (the
    // host checker requires the three arms; the handler re-applies kill switch +
    // arms + allowed-account). Routes ONLY through UnifiedTrading — never a raw
    // adapter. Every terminal path writes a trade_audit row.
    {
        ToolDef t;
        t.name = "cancel_order";
        t.description =
            "Fast Live Mode: cancel a single open LIVE order on the AI's allowed account.";
        t.category = "fast-live";
        t.auth_required = AuthLevel::Authenticated;
        t.is_destructive = true;
        t.input_schema = ToolSchemaBuilder()
                             .string("order_id", "Broker order ID to cancel")
                             .required()
                             .build();
        t.handler = [](const QJsonObject& args) -> ToolResult {
            const QString order_id = args.value("order_id").toString().trimmed();
            const QJsonObject intent{{"order_id", order_id}};

            auto g = fast_live_gate();
            if (!g.ok) {
                fast_derisk_audit("cancel_order", g.account, "denied", g.reason, intent);
                return rejected(g.reason);
            }

            auto resp = trading::UnifiedTrading::instance().cancel_order(g.account, order_id);
            const QString status = resp.success ? QStringLiteral("cancelled")
                                                 : QStringLiteral("rejected");
            // resp.message is the broker error (empty on success); never audit a
            // blank reason — fall back to the terminal status word.
            const QString reason = resp.message.isEmpty() ? status : resp.message;
            fast_derisk_audit("cancel_order", g.account, status, reason, intent);
            LOG_INFO(TAG, QString("fast cancel_order: %1 -> %2 (%3)")
                              .arg(order_id, status, g.account));
            return ToolResult::ok_data(QJsonObject{{"status", status},
                                                   {"order_id", order_id},
                                                   {"message", resp.message},
                                                   {"mode", "live"}});
        };
        tools.push_back(std::move(t));
    }

    // ── exit_position (DE-RISKING, destructive) ──────────────────────────────
    // Close/reduce a single LIVE position by symbol (places a market counter-order
    // via UnifiedTrading). De-risking → NO order-value floor (it only reduces
    // exposure). Full fast-live gate applies. A missing position returns the
    // broker's clean "Position not found" error → rejected, never a crash.
    {
        ToolDef t;
        t.name = "exit_position";
        t.description =
            "Fast Live Mode: close (square off) a single LIVE position by symbol on "
            "the AI's allowed account.";
        t.category = "fast-live";
        t.auth_required = AuthLevel::Authenticated;
        t.is_destructive = true;
        t.input_schema = ToolSchemaBuilder()
                             .string("symbol", "Symbol of the position to close")
                             .required()
                             .length(1, 64)
                             .string("exchange", "Exchange of the position (optional)")
                             .string("product", "Product type filter (optional: MIS/CNC/NRML)")
                             .build();
        t.handler = [](const QJsonObject& args) -> ToolResult {
            const QString symbol = args.value("symbol").toString().trimmed();
            const QString exchange = args.value("exchange").toString().trimmed();
            const QString product = args.value("product").toString().trimmed();
            const QJsonObject intent{
                {"symbol", symbol}, {"exchange", exchange}, {"product", product}};

            auto g = fast_live_gate();
            if (!g.ok) {
                fast_derisk_audit("exit_position", g.account, "denied", g.reason, intent);
                return rejected(g.reason);
            }

            auto resp =
                trading::UnifiedTrading::instance().close_position(g.account, symbol, exchange, product);
            const bool ok = resp.success;
            const QString status = ok ? QStringLiteral("closed") : QStringLiteral("rejected");
            // ApiResponse carries the error in .error; on success it's empty —
            // record a non-empty reason either way (trade_audit.reason NOT NULL).
            const QString message = ok ? QStringLiteral("position closed")
                                       : (resp.error.isEmpty() ? QStringLiteral("close failed")
                                                               : resp.error);
            fast_derisk_audit("exit_position", g.account, status, message, intent);
            LOG_INFO(TAG, QString("fast exit_position: %1 -> %2 (%3)")
                              .arg(symbol, status, g.account));
            QJsonObject out{{"status", status},
                            {"symbol", symbol},
                            {"message", message},
                            {"mode", "live"}};
            if (ok && resp.data)
                out["order_id"] = resp.data->order_id;
            return ToolResult::ok_data(out);
        };
        tools.push_back(std::move(t));
    }

    return tools;
}

} // namespace openmarketterminal::mcp::tools
