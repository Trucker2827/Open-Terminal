// OrderFlowTools.cpp — AI-trading two-phase order flow MCP tools (Phase A).
//
// Task 3: `prepare_order` — a NON-destructive (auth None, is_destructive=false)
// tool that validates a structured trade intent, runs a DETERMINISTIC risk floor
// (caps read ONLY from GUI-only `cli.risk.*` keys with finite defaults, so the
// AI can never bypass or raise them), persists an OrderDraft, appends a
// trade_audit row, and returns a verdict. It NEVER executes — submission is
// Task 4 (`submit_order` will be ADDed to get_order_flow_tools() below).

#include "mcp/tools/OrderFlowTools.h"

#include "core/logging/Logger.h"
#include "mcp/ToolSchemaBuilder.h"
#include "mcp/tools/DataHubPeekHelpers.h"
#include "mcp/tools/SettingsGate.h"
#include "storage/repositories/OrderDraftRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/repositories/TradeAuditRepository.h"
#include "trading/TradingTypes.h"
#include "trading/UnifiedTrading.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

#include <optional>

namespace openmarketterminal::mcp::tools {

static constexpr const char* TAG = "OrderFlowTools";

using openmarketterminal::trading::OrderSide;
using openmarketterminal::trading::OrderType;
using openmarketterminal::trading::UnifiedOrder;

namespace {

// ── enum parsing (string → enum; reject unknown) ───────────────────────────

std::optional<OrderSide> parse_side(const QString& s) {
    const QString v = s.trimmed().toLower();
    if (v == "buy")
        return OrderSide::Buy;
    if (v == "sell")
        return OrderSide::Sell;
    return std::nullopt;
}

std::optional<OrderType> parse_order_type(const QString& s) {
    const QString v = s.trimmed().toLower();
    if (v == "market")
        return OrderType::Market;
    if (v == "limit")
        return OrderType::Limit;
    if (v == "stop")
        return OrderType::StopLoss;
    if (v == "stop_limit")
        return OrderType::StopLossLimit;
    return std::nullopt;
}

// ── deterministic risk floor ───────────────────────────────────────────────

struct RiskVerdict {
    bool ok = false;
    QString reason;
    double order_value = 0.0;
    double max_loss = 0.0;
    // The resolved caps, recorded into the snapshot for the audit trail.
    double max_order_value = 0.0;
    double max_position_qty = 0.0;
};

/// Read a cap from a GUI-only `cli.risk.*` key. A missing / empty / garbage /
/// non-positive value falls back to the conservative finite default — NEVER to
/// "no cap" — so an emptied GUI field can't silently re-enable unlimited risk.
double read_cap(const QString& key, double default_val) {
    auto r = SettingsRepository::instance().get(key, QString());
    if (!r.is_ok())
        return default_val;
    bool parsed_ok = false;
    const double v = r.value().toDouble(&parsed_ok);
    if (!parsed_ok || v <= 0.0)
        return default_val;
    return v;
}

RiskVerdict risk_floor_check(const UnifiedOrder& o, double resolved_price) {
    RiskVerdict rv;
    // Caps come ONLY from the GUI-only cli.risk.* namespace (the AI cannot write
    // these — see SettingsGate::is_gui_only_setting), each with a finite default.
    rv.max_order_value = read_cap(QStringLiteral("cli.risk.max_order_value"), 25000.0);
    rv.max_position_qty = read_cap(QStringLiteral("cli.risk.max_position_qty"), 10000.0);
    rv.max_loss = read_cap(QStringLiteral("cli.risk.max_daily_loss"), 5000.0);

    rv.order_value = o.quantity * resolved_price;

    if (rv.order_value > rv.max_order_value) {
        rv.ok = false;
        rv.reason = QStringLiteral("exceeds max order value (%1 > %2)")
                        .arg(rv.order_value, 0, 'f', 2)
                        .arg(rv.max_order_value, 0, 'f', 2);
        return rv;
    }
    if (o.quantity > rv.max_position_qty) {
        rv.ok = false;
        rv.reason = QStringLiteral("exceeds max position size (%1 > %2)")
                        .arg(o.quantity, 0, 'f', 2)
                        .arg(rv.max_position_qty, 0, 'f', 2);
        return rv;
    }

    rv.ok = true;
    rv.reason = QStringLiteral("within risk limits");
    return rv;
}

// ── shared intent → order builder (reused by prepare_order AND submit_order) ─
//
// Parses + validates a trade-intent JSON object, builds the UnifiedOrder, and
// resolves the price used by the risk floor (limit price for limit/stop_limit,
// else the freshest cached last price). Returns `ok=false` with `error` set for
// MALFORMED input (caller maps to fail/rejected). `price_available=false` means
// the order is unpriceable and the value cap can't be enforced.
struct OrderBuild {
    bool ok = false;
    QString error;
    UnifiedOrder order;
    QString account;
    bool needs_limit = false;
    double resolved_price = 0.0;
    bool price_available = false;
};

OrderBuild build_order_from_intent(const QJsonObject& args) {
    OrderBuild b;

    const QString symbol = args["symbol"].toString().trimmed();
    if (symbol.isEmpty()) {
        b.error = QStringLiteral("Missing 'symbol'");
        return b;
    }
    const double quantity = args["quantity"].toDouble(0.0);
    if (quantity <= 0) {
        b.error = QStringLiteral("'quantity' must be > 0");
        return b;
    }
    const auto side = parse_side(args["side"].toString());
    if (!side) {
        b.error = QStringLiteral("Invalid 'side' (expected buy/sell)");
        return b;
    }
    const auto otype = parse_order_type(args["order_type"].toString("market"));
    if (!otype) {
        b.error = QStringLiteral("Invalid 'order_type' (expected market/limit/stop/stop_limit)");
        return b;
    }
    const bool needs_limit = (*otype == OrderType::Limit || *otype == OrderType::StopLossLimit);
    double limit_price = 0.0;
    const bool has_limit = args.contains("limit_price") && args["limit_price"].isDouble();
    if (has_limit)
        limit_price = args["limit_price"].toDouble();
    if (needs_limit && limit_price <= 0) {
        b.error = QStringLiteral("'limit_price' must be > 0 for limit/stop_limit orders");
        return b;
    }

    const QString account_in = args["account"].toString().trimmed();
    b.account = account_in.isEmpty() ? QStringLiteral("paper-default") : account_in;
    b.needs_limit = needs_limit;

    b.order.symbol = symbol;
    b.order.exchange = args["exchange"].toString();
    b.order.side = *side;
    b.order.order_type = *otype;
    b.order.quantity = quantity;
    b.order.price = needs_limit ? limit_price : 0.0;

    // Resolve the price for the risk check.
    if (needs_limit) {
        b.resolved_price = limit_price;
        b.price_available = true;
    } else if (const auto q = detail::peek_quote(symbol); q && q->price > 0.0) {
        b.resolved_price = q->price;
        b.price_available = true;
    } else {
        b.price_available = false; // unpriceable: caller must reject
    }

    b.ok = true;
    return b;
}

QJsonObject verdict_to_json(const RiskVerdict& rv) {
    return QJsonObject{
        {"ok", rv.ok},
        {"reason", rv.reason},
        {"order_value", rv.order_value},
        {"max_loss", rv.max_loss},
        {"max_order_value", rv.max_order_value},
        {"max_position_qty", rv.max_position_qty},
    };
}

// ── audit helper ───────────────────────────────────────────────────────────

void audit_append(const QString& phase, const QString& tool, const QString& account,
                  const QString& mode, const QJsonObject& intent, const QString& decision,
                  const QString& reason, const RiskVerdict& rv) {
    TradeAuditRow row;
    row.ts = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    row.phase = phase;
    row.tool = tool;
    row.account = account;
    row.mode = mode;
    row.intent_json = QString::fromUtf8(QJsonDocument(intent).toJson(QJsonDocument::Compact));
    row.decision = decision;
    row.reason = reason;
    row.risk_snapshot_json =
        QString::fromUtf8(QJsonDocument(verdict_to_json(rv)).toJson(QJsonDocument::Compact));
    auto r = TradeAuditRepository::instance().append(row);
    if (!r.is_ok())
        LOG_WARN(TAG, "trade_audit append failed: " + QString::fromStdString(r.error()));
}

void audit_prepare(const QString& account, const QJsonObject& intent, const QString& decision,
                   const QString& reason, const RiskVerdict& rv) {
    audit_append(QStringLiteral("prepare"), QStringLiteral("prepare_order"), account,
                 QStringLiteral("paper"), intent, decision, reason, rv);
}

void audit_submit(const QString& account, const QString& mode, const QJsonObject& intent,
                  const QString& decision, const QString& reason, const RiskVerdict& rv) {
    audit_append(QStringLiteral("submit"), QStringLiteral("submit_order"), account, mode, intent,
                 decision, reason, rv);
}

} // namespace

std::vector<ToolDef> get_order_flow_tools() {
    std::vector<ToolDef> tools;

    // ── prepare_order ──────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "prepare_order";
        t.description =
            "Prepare (validate + risk-check + draft) a trade intent WITHOUT executing it. "
            "Runs a deterministic risk floor against GUI-owned caps, persists a draft, and "
            "audits the decision. Returns status 'prepared' (with a draft_id for a later "
            "submit_order) or 'rejected' (with the risk reason). Side buy/sell; order_type "
            "market/limit/stop/stop_limit; limit_price required for limit/stop_limit.";
        t.category = "trading";
        t.auth_required = AuthLevel::None;
        t.is_destructive = false; // non-destructive: drafts + audits only, never executes
        t.input_schema =
            ToolSchemaBuilder()
                .string("symbol", "Trading symbol (e.g. AAPL)").required().length(1, 32)
                .string("side", "Order side").required().enums({"buy", "sell"})
                .number("quantity", "Order quantity (must be > 0)").required().min(0.0)
                .string("order_type", "Order type").default_str("market")
                    .enums({"market", "limit", "stop", "stop_limit"})
                .number("limit_price", "Limit price (required for limit/stop_limit orders)")
                .string("exchange", "Exchange / venue (optional)")
                .string("account", "Account identifier (optional)")
                .string("strategy", "Originating strategy name (optional)")
                .string("reason", "Free-text rationale for the intent (optional)")
                .build();
        t.handler = [](const QJsonObject& args) -> ToolResult {
            // 1-2. Parse + validate + build the UnifiedOrder (malformed → fail).
            const OrderBuild b = build_order_from_intent(args);
            if (!b.ok)
                return ToolResult::fail(b.error);
            const QString account = b.account;

            // 3. Resolve price for the risk check, then run the deterministic floor.
            if (!b.price_available) {
                // No price ⇒ the value cap can't be enforced. Reject rather than
                // letting an unpriceable order bypass the floor.
                RiskVerdict rv;
                rv.ok = false;
                rv.reason = QStringLiteral("no price available for risk check");
                rv.max_loss = read_cap(QStringLiteral("cli.risk.max_daily_loss"), 5000.0);
                audit_prepare(account, args, "rejected", rv.reason, rv);
                return ToolResult::ok_data(QJsonObject{
                    {"status", "rejected"},
                    {"reason", rv.reason},
                    {"checks", QJsonArray{rv.reason}},
                });
            }

            const RiskVerdict rv = risk_floor_check(b.order, b.resolved_price);

            // 4. Risk FAIL → audit "rejected" and return a rejection verdict.
            if (!rv.ok) {
                audit_prepare(account, args, "rejected", rv.reason, rv);
                LOG_INFO(TAG, "prepare_order rejected: " + rv.reason);
                return ToolResult::ok_data(QJsonObject{
                    {"status", "rejected"},
                    {"reason", rv.reason},
                    {"risk_status", "failed"},
                    {"order_value", rv.order_value},
                    {"max_loss", rv.max_loss},
                    {"checks", QJsonArray{rv.reason}},
                });
            }

            // 5. PASS → persist a draft, audit "prepared", return the verdict.
            const QString draft_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            const QString expires = QDateTime::currentDateTimeUtc().addSecs(5 * 60).toString(Qt::ISODate);

            OrderDraft draft;
            draft.draft_id = draft_id;
            draft.intent_json = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
            draft.risk_verdict_json =
                QString::fromUtf8(QJsonDocument(verdict_to_json(rv)).toJson(QJsonDocument::Compact));
            draft.account = account;
            draft.mode_hint = "paper";
            draft.status = "prepared";
            draft.created_at = now;
            draft.expires_at = expires;

            auto ins = OrderDraftRepository::instance().insert(draft);
            if (!ins.is_ok())
                return ToolResult::fail("Failed to persist draft: " +
                                        QString::fromStdString(ins.error()));

            audit_prepare(account, args, "prepared", rv.reason, rv);
            LOG_INFO(TAG, "prepare_order prepared draft " + draft_id);

            return ToolResult::ok_data(QJsonObject{
                {"status", "prepared"},
                {"draft_id", draft_id},
                {"risk_status", "passed"},
                {"order_value", rv.order_value},
                {"max_loss", rv.max_loss},
                {"expires_at", expires},
                {"checks", QJsonArray{rv.reason}},
            });
        };
        tools.push_back(std::move(t));
    }

    // ── submit_order ─────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "submit_order";
        t.description =
            "Submit a previously prepared order draft for execution. Loads the draft, RE-RUNS "
            "the deterministic risk floor against FRESH GUI-owned caps (revocable — a lowered "
            "cap rejects here even if prepare passed), gates by mode, and audits the decision. "
            "mode=paper executes on the paper engine IFF paper trading is enabled in GUI "
            "Settings; mode=live is hard-disabled in Phase A (paper-first) and NEVER reaches a "
            "broker. Returns status filled/rejected.";
        t.category = "trading";
        t.auth_required = AuthLevel::Authenticated;
        t.is_destructive = true;
        t.input_schema =
            ToolSchemaBuilder()
                .string("draft_id", "Draft id returned by prepare_order").required().length(1, 128)
                .string("mode", "Execution mode").required().enums({"paper", "live"})
                .build();
        t.handler = [](const QJsonObject& args) -> ToolResult {
            // Malformed input → fail (decisions use ok_data; only bad input fails).
            const QString draft_id = args["draft_id"].toString().trimmed();
            if (draft_id.isEmpty())
                return ToolResult::fail("Missing 'draft_id'");
            const QString mode = args["mode"].toString().trimmed().toLower();

            const RiskVerdict empty_rv; // for audits before a verdict exists

            // 1. Load the draft.
            auto dr = OrderDraftRepository::instance().get(draft_id);
            if (!dr.is_ok()) {
                const QString audit_mode = mode.isEmpty() ? QStringLiteral("paper") : mode;
                audit_submit(QStringLiteral("paper-default"), audit_mode,
                             QJsonObject{{"draft_id", draft_id}}, "rejected", "draft not found",
                             empty_rv);
                return ToolResult::ok_data(
                    QJsonObject{{"status", "rejected"}, {"reason", "draft not found"}});
            }
            const OrderDraft draft = dr.value();
            const QString audit_mode = mode.isEmpty() ? QStringLiteral("paper") : mode;

            // 2. Must be a fresh, unused, unexpired draft.
            const QDateTime expires = QDateTime::fromString(draft.expires_at, Qt::ISODate);
            const bool is_expired =
                expires.isValid() && expires <= QDateTime::currentDateTimeUtc();
            if (draft.status != QLatin1String("prepared") || is_expired) {
                audit_submit(draft.account, audit_mode, QJsonObject{{"draft_id", draft_id}},
                             "rejected", "draft expired or already used", empty_rv);
                return ToolResult::ok_data(QJsonObject{
                    {"status", "rejected"}, {"reason", "draft expired or already used"}});
            }

            // 3. Re-parse the stored intent, rebuild the order, RE-RUN the floor
            //    against FRESH caps — the stored verdict is NOT trusted (revocable).
            const QJsonObject intent =
                QJsonDocument::fromJson(draft.intent_json.toUtf8()).object();
            const OrderBuild b = build_order_from_intent(intent);
            if (!b.ok) {
                audit_submit(draft.account, audit_mode, intent, "rejected",
                             "invalid draft intent: " + b.error, empty_rv);
                return ToolResult::ok_data(QJsonObject{
                    {"status", "rejected"}, {"reason", "invalid draft intent: " + b.error}});
            }
            if (!b.price_available) {
                RiskVerdict rv;
                rv.ok = false;
                rv.reason = QStringLiteral("no price available for risk check");
                rv.max_loss = read_cap(QStringLiteral("cli.risk.max_daily_loss"), 5000.0);
                audit_submit(draft.account, audit_mode, intent, "rejected", rv.reason, rv);
                return ToolResult::ok_data(
                    QJsonObject{{"status", "rejected"}, {"reason", rv.reason}});
            }
            const RiskVerdict rv = risk_floor_check(b.order, b.resolved_price);
            if (!rv.ok) {
                audit_submit(draft.account, audit_mode, intent, "rejected", rv.reason, rv);
                LOG_INFO(TAG, "submit_order risk re-check rejected: " + rv.reason);
                return ToolResult::ok_data(QJsonObject{
                    {"status", "rejected"},
                    {"reason", rv.reason},
                    {"risk_status", "failed"},
                    {"order_value", rv.order_value},
                    {"max_loss", rv.max_loss},
                });
            }

            // 4. Gate by mode.
            if (mode == QLatin1String("paper")) {
                // The handler is the final authority: re-check the GUI toggle LIVE
                // (revocable). The checker only opened the door to reach here.
                if (!mcp::cli_paper_trading_allowed()) {
                    audit_submit(draft.account, QStringLiteral("paper"), intent, "denied",
                                 "paper trading disabled — enable in GUI Settings", rv);
                    return ToolResult::ok_data(QJsonObject{
                        {"status", "rejected"},
                        {"reason", "paper trading disabled — enable in GUI Settings"}});
                }
                // Execute on the PAPER rail via the SESSION overload (NOT the
                // account overload, which is the LIVE broker path).
                auto& ut = trading::UnifiedTrading::instance();
                auto sess = ut.get_session();
                if (!sess || sess->mode != QLatin1String("paper"))
                    ut.init_session("paper", "paper"); // $100k paper portfolio
                trading::UnifiedOrderResponse resp = ut.place_order(b.order);

                OrderDraftRepository::instance().update_status(draft_id, "submitted");
                const QString decision = resp.success ? QStringLiteral("filled")
                                                      : QStringLiteral("rejected");
                audit_submit(draft.account, QStringLiteral("paper"), intent, decision,
                             resp.message, rv);
                LOG_INFO(TAG, "submit_order paper " + decision + " draft " + draft_id);
                return ToolResult::ok_data(QJsonObject{
                    {"status", decision},
                    {"order_id", resp.order_id},
                    {"message", resp.message},
                    {"mode", "paper"},
                });
            }
            if (mode == QLatin1String("live")) {
                // HARD-OFF: live never reaches a broker in Phase A (paper-first).
                // No UnifiedTrading / broker call on this path — defense in depth
                // behind the checker, which also denies live at Phase A defaults.
                audit_submit(draft.account, QStringLiteral("live"), intent, "denied",
                             "live trading disabled (paper-first; not yet enabled)", rv);
                LOG_WARN(TAG, "submit_order live HARD-OFF for draft " + draft_id);
                return ToolResult::ok_data(QJsonObject{
                    {"status", "rejected"},
                    {"reason", "live trading disabled (paper-first; not yet enabled)"},
                    {"mode", "live"},
                });
            }

            // Schema enum should catch this; defensive only.
            return ToolResult::ok_data(
                QJsonObject{{"status", "rejected"}, {"reason", "unknown mode"}});
        };
        tools.push_back(std::move(t));
    }

    return tools;
}

} // namespace openmarketterminal::mcp::tools
