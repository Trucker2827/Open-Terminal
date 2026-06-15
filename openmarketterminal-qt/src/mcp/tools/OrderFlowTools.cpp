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
#include "mcp/tools/LivePnl.h"
#include "mcp/tools/PmPaperEngine.h"
#include "mcp/tools/PredictionTools.h"
#include "mcp/tools/SettingsGate.h"
#include "services/prediction/PredictionTypes.h"
#include "storage/repositories/OrderDraftRepository.h"
#include "storage/repositories/PmPaperRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/repositories/TradeAuditRepository.h"
#include "trading/AccountManager.h"
#include "trading/TradingTypes.h"
#include "trading/UnifiedTrading.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

#include <limits>
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
    // The daily-loss cap is RECORDED (returned + audited) but NOT yet enforced as a
    // running-loss check — that needs a P&L tally across fills, which is Phase C.
    // It is surfaced now so the configured ceiling is visible in the audit trail.
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

// ── prediction-market path (asset_class == "prediction") ────────────────────
//
// Resolve the best executable price + market metadata from the LIVE order book
// via the shared READ-ONLY bridge (pm_fetch_market / pm_fetch_order_book — the
// fetch helpers never touch the adapter's live order methods), then run a
// DETERMINISTIC PM risk floor. Mirrors the equity floor: caps come ONLY from
// GUI-only cli.risk.* keys with finite defaults.

namespace pred = openmarketterminal::services::prediction;

struct PmResolved {
    bool ok = false;
    QString reason;
    double price = 0.0;     // executable price for `side` (buy→best_ask, sell→best_bid)
    double best_bid = 0.0;  // 0.0 ⇒ no bid side present
    double best_ask = 0.0;  // 0.0 ⇒ no ask side present
    double liquidity = 0.0;
    QString category;
    QString end_date_iso;
};

PmResolved pm_resolve(const QString& venue, const QString& market_id, const QString& asset_id,
                      const QString& side) {
    PmResolved r;

    const auto market = pm_fetch_market(venue, market_id);
    if (!market) {
        r.reason = QStringLiteral("market not found");
        return r;
    }
    r.category = market->category;
    r.liquidity = market->liquidity;
    r.end_date_iso = market->end_date_iso;

    const auto book = pm_fetch_order_book(venue, asset_id);
    if (!book || (book->bids.isEmpty() && book->asks.isEmpty())) {
        r.reason = QStringLiteral("no price available for risk check");
        return r;
    }

    bool has_bid = false, has_ask = false;
    double best_bid = -std::numeric_limits<double>::infinity();
    double best_ask = std::numeric_limits<double>::infinity();
    for (const auto& lvl : book->bids)
        if (lvl.price > best_bid) {
            best_bid = lvl.price;
            has_bid = true;
        }
    for (const auto& lvl : book->asks)
        if (lvl.price < best_ask) {
            best_ask = lvl.price;
            has_ask = true;
        }
    r.best_bid = has_bid ? best_bid : 0.0;
    r.best_ask = has_ask ? best_ask : 0.0;

    const bool buy = (side == QLatin1String("buy"));
    if ((buy && !has_ask) || (!buy && !has_bid)) {
        r.reason = QStringLiteral("no price available for risk check");
        return r;
    }
    r.price = buy ? best_ask : best_bid;
    r.ok = true;
    return r;
}

RiskVerdict pm_risk_floor_check(const QString& category, const QString& /*side*/, double contracts,
                                double fill_price, double best_bid, double best_ask,
                                double liquidity, const QString& end_date_iso) {
    RiskVerdict rv;
    // Caps from the GUI-only cli.risk.* namespace, each with a finite default.
    const double max_order_value = read_cap(QStringLiteral("cli.risk.max_order_value"), 25000.0);
    const double max_qty = read_cap(QStringLiteral("cli.risk.max_position_qty"), 10000.0);
    const double max_topic = read_cap(QStringLiteral("cli.risk.max_exposure_per_topic"), 10000.0);
    const double min_liq = read_cap(QStringLiteral("cli.risk.pm_min_liquidity"), 1000.0);
    const double max_spread = read_cap(QStringLiteral("cli.risk.pm_max_spread"), 0.10);
    const double min_hours = read_cap(QStringLiteral("cli.risk.pm_min_hours_to_resolution"), 1.0);

    const double stake = contracts * fill_price;
    rv.max_order_value = max_order_value;
    rv.max_position_qty = max_qty;
    rv.order_value = stake;
    rv.max_loss = stake;  // a long prediction can lose its full stake.

    if (stake > max_order_value) {
        rv.reason = QStringLiteral("exceeds max order value (%1 > %2)")
                        .arg(stake, 0, 'f', 2).arg(max_order_value, 0, 'f', 2);
        return rv;
    }
    if (contracts > max_qty) {
        rv.reason = QStringLiteral("exceeds max position size (%1 > %2)")
                        .arg(contracts, 0, 'f', 2).arg(max_qty, 0, 'f', 2);
        return rv;
    }
    const auto open_stake = PmPaperRepository::instance().open_stake_in_category(category);
    const double existing = open_stake.is_ok() ? open_stake.value() : 0.0;
    if (existing + stake > max_topic) {
        rv.reason = QStringLiteral("exceeds max exposure for topic '%1' (%2 > %3)")
                        .arg(category).arg(existing + stake, 0, 'f', 2).arg(max_topic, 0, 'f', 2);
        return rv;
    }
    if (liquidity < min_liq) {
        rv.reason = QStringLiteral("market too illiquid (liquidity %1 < %2)")
                        .arg(liquidity, 0, 'f', 2).arg(min_liq, 0, 'f', 2);
        return rv;
    }
    if (best_bid > 0.0 && best_ask > 0.0 && (best_ask - best_bid) > max_spread) {
        rv.reason = QStringLiteral("spread too wide (%1 > %2)")
                        .arg(best_ask - best_bid, 0, 'f', 4).arg(max_spread, 0, 'f', 4);
        return rv;
    }
    if (!end_date_iso.trimmed().isEmpty()) {
        const QDateTime end = QDateTime::fromString(end_date_iso.trimmed(), Qt::ISODate);
        if (end.isValid()) {
            const double hours = QDateTime::currentDateTimeUtc().secsTo(end) / 3600.0;
            if (hours < min_hours) {
                rv.reason = QStringLiteral("too close to resolution (%1h < %2h)")
                                .arg(hours, 0, 'f', 2).arg(min_hours, 0, 'f', 2);
                return rv;
            }
        }
        // Unparseable end_date ⇒ treat as OK (don't block on missing data).
    }

    rv.ok = true;
    rv.reason = QStringLiteral("within risk limits");
    return rv;
}

ToolResult prepare_prediction_order(const QJsonObject& args) {
    const QString venue = args.value("venue").toString().trimmed().toLower();
    const QString market_id = args.value("market_id").toString().trimmed();
    const QString asset_id = args.value("asset_id").toString().trimmed();
    const QString outcome = args.value("outcome").toString().trimmed();
    const QString side = args.value("side").toString().trimmed().toLower();
    const double contracts = args.value("contracts").toDouble(0.0);
    const QString account = QStringLiteral("pm-paper");
    const RiskVerdict empty_rv;

    // Malformed input → fail (only DECISIONS use ok_data).
    if (venue.isEmpty())
        return ToolResult::fail("Missing 'venue'");
    if (asset_id.isEmpty())
        return ToolResult::fail("Missing 'asset_id'");
    if (outcome.isEmpty())
        return ToolResult::fail("Missing 'outcome'");
    if (contracts <= 0.0)
        return ToolResult::fail("'contracts' must be > 0");
    if (side != QLatin1String("buy") && side != QLatin1String("sell"))
        return ToolResult::fail("Invalid 'side' (expected buy/sell)");
    if (args.contains("limit_price") && args.value("limit_price").isDouble()) {
        const double lp = args.value("limit_price").toDouble();
        if (lp < 0.0 || lp > 1.0)
            return ToolResult::fail("'limit_price' must be in [0,1] for prediction orders");
    }

    auto reject = [&](const QString& reason, const RiskVerdict& rv) -> ToolResult {
        audit_prepare(account, args, "rejected", reason, rv);
        LOG_INFO(TAG, "prepare_order (prediction) rejected: " + reason);
        return ToolResult::ok_data(QJsonObject{
            {"status", "rejected"}, {"reason", reason}, {"checks", QJsonArray{reason}}});
    };

    // Venue allow-list (policy decision, not malformed input).
    if (!mcp::cli_venue_allowed(venue))
        return reject(QStringLiteral("venue '%1' not in allowed venues — enable in GUI Settings")
                          .arg(venue),
                      empty_rv);

    // SELL requires an existing open position — short-open is not enabled in Phase B.
    if (side == QLatin1String("sell")) {
        auto open = PmPaperRepository::instance().get_open(venue, asset_id);
        const bool has_pos = open.is_ok() && open.value().has_value();
        if (!has_pos)
            return reject(
                QStringLiteral("no open position to sell; short-open is not enabled in Phase B"),
                empty_rv);
    }

    // Resolve best price + market metadata from the LIVE book (read-only bridge).
    const PmResolved r = pm_resolve(venue, market_id, asset_id, side);
    if (!r.ok)
        return reject(r.reason, empty_rv);

    // Deterministic PM risk floor.
    const RiskVerdict rv = pm_risk_floor_check(r.category, side, contracts, r.price, r.best_bid,
                                               r.best_ask, r.liquidity, r.end_date_iso);
    if (!rv.ok) {
        audit_prepare(account, args, "rejected", rv.reason, rv);
        LOG_INFO(TAG, "prepare_order (prediction) rejected: " + rv.reason);
        return ToolResult::ok_data(QJsonObject{
            {"status", "rejected"},
            {"reason", rv.reason},
            {"risk_status", "failed"},
            {"stake", rv.order_value},
            {"max_loss", rv.max_loss},
            {"fill_price", r.price},
            {"checks", QJsonArray{rv.reason}},
        });
    }

    // PASS → persist a draft carrying the normalized PM intent, audit "prepared".
    const QString draft_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    const QString expires = QDateTime::currentDateTimeUtc().addSecs(5 * 60).toString(Qt::ISODate);

    QJsonObject intent{
        {"asset_class", "prediction"}, {"venue", venue},   {"market_id", market_id},
        {"asset_id", asset_id},        {"outcome", outcome}, {"side", side},
        {"contracts", contracts},      {"category", r.category},
    };
    if (args.contains("limit_price") && args.value("limit_price").isDouble())
        intent["limit_price"] = args.value("limit_price").toDouble();

    OrderDraft draft;
    draft.draft_id = draft_id;
    draft.intent_json = QString::fromUtf8(QJsonDocument(intent).toJson(QJsonDocument::Compact));
    draft.risk_verdict_json =
        QString::fromUtf8(QJsonDocument(verdict_to_json(rv)).toJson(QJsonDocument::Compact));
    draft.account = account;
    draft.mode_hint = "paper";
    draft.status = "prepared";
    draft.created_at = now;
    draft.expires_at = expires;

    auto ins = OrderDraftRepository::instance().insert(draft);
    if (!ins.is_ok())
        return ToolResult::fail("Failed to persist draft: " + QString::fromStdString(ins.error()));

    audit_prepare(account, intent, "prepared", rv.reason, rv);
    LOG_INFO(TAG, "prepare_order (prediction) prepared draft " + draft_id);

    return ToolResult::ok_data(QJsonObject{
        {"status", "prepared"},
        {"draft_id", draft_id},
        {"risk_status", "passed"},
        {"stake", rv.order_value},
        {"max_loss", rv.max_loss},
        {"fill_price", r.price},
        {"expires_at", expires},
        {"checks", QJsonArray{rv.reason}},
    });
}

// ── prediction submit path (asset_class == "prediction") ─────────────────────
//
// Re-validate FRESH (re-resolve the book, RE-RUN the PM risk floor — revocable),
// gate by mode, then for paper execute on the PM PAPER engine (BUY-to-open /
// SELL-to-close — pure DB writes, NEVER the adapter's live order methods). LIVE
// is a HARD-OFF: a string rejection with ZERO engine/adapter calls.
ToolResult submit_prediction_order(const OrderDraft& draft, const QJsonObject& intent,
                                   const QString& mode, const QString& draft_id,
                                   const QString& audit_mode) {
    const RiskVerdict empty_rv;
    const QString venue = intent.value("venue").toString().trimmed().toLower();
    const QString market_id = intent.value("market_id").toString().trimmed();
    const QString asset_id = intent.value("asset_id").toString().trimmed();
    const QString outcome = intent.value("outcome").toString().trimmed();
    const QString side = intent.value("side").toString().trimmed().toLower();
    const double contracts = intent.value("contracts").toDouble(0.0);

    // 1. Re-resolve the executable price + market metadata from the LIVE book.
    const PmResolved r = pm_resolve(venue, market_id, asset_id, side);
    if (!r.ok) {
        audit_submit(draft.account, audit_mode, intent, "rejected", r.reason, empty_rv);
        LOG_INFO(TAG, "submit_order (prediction) rejected: " + r.reason);
        return ToolResult::ok_data(QJsonObject{{"status", "rejected"}, {"reason", r.reason}});
    }

    // 2. RE-RUN the deterministic PM risk floor against FRESH caps + book
    //    (revocable — the stored verdict is NOT trusted).
    const RiskVerdict rv = pm_risk_floor_check(r.category, side, contracts, r.price, r.best_bid,
                                               r.best_ask, r.liquidity, r.end_date_iso);
    if (!rv.ok) {
        audit_submit(draft.account, audit_mode, intent, "rejected", rv.reason, rv);
        LOG_INFO(TAG, "submit_order (prediction) risk re-check rejected: " + rv.reason);
        return ToolResult::ok_data(QJsonObject{
            {"status", "rejected"},
            {"reason", rv.reason},
            {"risk_status", "failed"},
            {"stake", rv.order_value},
            {"max_loss", rv.max_loss},
        });
    }

    // 3. Gate by mode.
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
        if (!mcp::cli_venue_allowed(venue)) {
            const QString reason =
                QStringLiteral("venue '%1' not in allowed venues").arg(venue);
            audit_submit(draft.account, QStringLiteral("paper"), intent, "denied", reason, rv);
            return ToolResult::ok_data(
                QJsonObject{{"status", "rejected"}, {"reason", reason}});
        }
        // Atomically reserve the draft BEFORE executing (compare-and-set
        // prepared→submitting). A concurrent or duplicate submit loses the race
        // and is rejected, so a fill can never be double-executed.
        auto reserve = OrderDraftRepository::instance().reserve_for_submit(draft_id);
        if (!reserve.is_ok() || !reserve.value()) {
            audit_submit(draft.account, QStringLiteral("paper"), intent, "rejected",
                         "draft already used or not reservable", rv);
            return ToolResult::ok_data(QJsonObject{
                {"status", "rejected"}, {"reason", "draft already used or not reservable"}});
        }
        // Execute on the PM PAPER engine using the RESOLVED book price (NOT the
        // AI's limit_price). Category is market-derived (from pm_resolve). These
        // are pure DB writes — NEVER the adapter's place_order/cancel_order.
        const PmFill fill =
            (side == QLatin1String("buy"))
                ? buy_to_open(venue, market_id, asset_id, outcome, r.category, contracts, r.price)
                : sell_to_close(venue, asset_id, contracts, r.price);

        OrderDraftRepository::instance().update_status(
            draft_id, fill.ok ? "submitted" : "submit_failed");
        const QString decision = fill.ok ? QStringLiteral("filled") : QStringLiteral("rejected");
        // A successful PmFill carries an EMPTY reason; the trade_audit.reason
        // column is NOT NULL, so synthesize an informative non-empty reason.
        QString fill_reason = fill.reason.trimmed();
        if (fill_reason.isEmpty())
            fill_reason = fill.ok ? QStringLiteral("%1 %2 @ %3")
                                        .arg(fill.action)
                                        .arg(fill.contracts, 0, 'f', 2)
                                        .arg(fill.fill_price, 0, 'f', 4)
                                  : QStringLiteral("rejected");
        audit_submit(draft.account, QStringLiteral("paper"), intent, decision, fill_reason, rv);
        LOG_INFO(TAG, "submit_order (prediction) paper " + decision + " draft " + draft_id);
        return ToolResult::ok_data(QJsonObject{
            {"status", decision},
            {"action", fill.action},
            {"contracts", fill.contracts},
            {"fill_price", fill.fill_price},
            {"cash_after", fill.cash_after},
            {"mode", "paper"},
        });
    }
    if (mode == QLatin1String("live")) {
        // HARD-OFF: live NEVER reaches an engine/adapter in Phase B (paper-first).
        // ZERO PmPaperEngine / adapter calls on this path.
        audit_submit(draft.account, QStringLiteral("live"), intent, "denied",
                     "live trading disabled (paper-first; not yet enabled)", rv);
        LOG_WARN(TAG, "submit_order (prediction) live HARD-OFF for draft " + draft_id);
        return ToolResult::ok_data(QJsonObject{
            {"status", "rejected"},
            {"reason", "live trading disabled (paper-first; not yet enabled)"},
            {"mode", "live"},
        });
    }

    // Schema enum should catch this; defensive only.
    audit_submit(draft.account, audit_mode, intent, "rejected", "unknown mode", rv);
    return ToolResult::ok_data(
        QJsonObject{{"status", "rejected"}, {"reason", "unknown mode"}});
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
                // ── shared discriminator ──
                .string("asset_class", "Asset class: equity (default) or prediction")
                    .enums({"equity", "prediction"})
                .string("side", "Order side").required().enums({"buy", "sell"})
                .number("limit_price",
                        "Equity: limit price (required for limit/stop_limit). "
                        "Prediction: probability in [0,1] (optional)")
                // ── equity fields (optional at schema; validated in the equity path) ──
                .string("symbol", "Equity trading symbol (e.g. AAPL)").length(1, 32)
                .number("quantity", "Equity order quantity (must be > 0)").min(0.0)
                .string("order_type", "Equity order type").default_str("market")
                    .enums({"market", "limit", "stop", "stop_limit"})
                .string("exchange", "Exchange / venue (optional)")
                .string("account", "Account identifier (optional)")
                .string("strategy", "Originating strategy name (optional)")
                .string("reason", "Free-text rationale for the intent (optional)")
                // ── prediction fields (optional at schema; validated in the PM path) ──
                .string("venue", "Prediction venue (polymarket | kalshi) — prediction only")
                .string("market_id", "Prediction market id — prediction only")
                .string("asset_id", "Prediction outcome asset id — prediction only")
                .string("outcome", "Prediction outcome name — prediction only")
                .number("contracts", "Prediction contracts (must be > 0) — prediction only").min(0.0)
                .build();
        t.handler = [](const QJsonObject& args) -> ToolResult {
            // -1. KILL SWITCH FIRST (Phase C): the human-owned panic button halts
            //     ALL AI trading — paper AND live, equity AND prediction — before
            //     any parse / price-resolve / draft. Record the refusal, then
            //     short-circuit. (account unknown this early → "" ; mode is forced
            //     "paper" by audit_prepare; intent = raw args.)
            if (mcp::cli_kill_switch_engaged()) {
                // account unknown this early — pass empty-but-NON-NULL ("" not
                // QString()) so the trade_audit.account NOT NULL bind succeeds and
                // the refusal is actually recorded.
                audit_prepare(QStringLiteral(""), args, QStringLiteral("kill_switch"),
                              QStringLiteral("kill switch engaged"), RiskVerdict{});
                LOG_WARN(TAG, "prepare_order rejected: kill switch engaged");
                return ToolResult::ok_data(QJsonObject{
                    {"status", "rejected"}, {"reason", "kill switch engaged"}});
            }

            // 0. Discriminate by asset_class. Prediction → the PM path; anything
            //    else → the UNCHANGED equity path below.
            const QString asset_class =
                args.value("asset_class").toString("equity").trimmed().toLower();
            if (asset_class == QLatin1String("prediction"))
                return prepare_prediction_order(args);

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
            // KILL SWITCH FIRST (Phase C): the human-owned panic button halts ALL
            // AI trading — paper AND live, equity AND prediction — BEFORE the draft
            // load or any execution. Record the refusal, then short-circuit.
            // (account unknown this early → "" ; mode from args, default "paper".)
            if (mcp::cli_kill_switch_engaged()) {
                const QString km = args.value("mode").toString().trimmed().toLower();
                // empty-but-NON-NULL account ("" not QString()) so the
                // trade_audit.account NOT NULL bind succeeds and the refusal records.
                audit_submit(QStringLiteral(""), km.isEmpty() ? QStringLiteral("paper") : km, args,
                             QStringLiteral("kill_switch"), QStringLiteral("kill switch engaged"),
                             RiskVerdict{});
                LOG_WARN(TAG, "submit_order rejected: kill switch engaged");
                return ToolResult::ok_data(QJsonObject{
                    {"status", "rejected"}, {"reason", "kill switch engaged"}});
            }

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
            const OrderDraft& draft = dr.value();
            const QString audit_mode = mode.isEmpty() ? QStringLiteral("paper") : mode;

            // 2. Must be a fresh, unused, unexpired draft.
            const QDateTime expires = QDateTime::fromString(draft.expires_at, Qt::ISODate);
            // Fail closed: a missing/malformed expiry (never written by a real
            // prepare_order, which always stamps a valid ISODate) is treated as
            // expired rather than silently non-expired.
            const bool is_expired =
                !expires.isValid() || expires <= QDateTime::currentDateTimeUtc();
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

            // Discriminate by asset_class. Prediction → the PM submit path
            // (re-resolve fresh, RE-RUN the PM floor, gate, paper-execute on the
            // PM paper engine / live hard-off). Anything else → the UNCHANGED
            // equity submit path below.
            const QString asset_class =
                intent.value("asset_class").toString("equity").trimmed().toLower();
            if (asset_class == QLatin1String("prediction"))
                return submit_prediction_order(draft, intent, mode, draft_id, audit_mode);

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
                // Atomically reserve the draft BEFORE executing (compare-and-set
                // prepared→submitting). Only the winner proceeds; a concurrent or
                // duplicate submit loses the race and is rejected, so a fill can
                // never be double-executed and a failed status write can never
                // silently follow a fill. (Critical once a live path exists;
                // correct-and-cheap for paper.)
                auto reserve = OrderDraftRepository::instance().reserve_for_submit(draft_id);
                if (!reserve.is_ok() || !reserve.value()) {
                    audit_submit(draft.account, QStringLiteral("paper"), intent, "rejected",
                                 "draft already used or not reservable", rv);
                    return ToolResult::ok_data(QJsonObject{
                        {"status", "rejected"},
                        {"reason", "draft already used or not reservable"}});
                }
                // Execute on the PAPER rail via the SESSION overload (NOT the
                // account overload, which is the LIVE broker path).
                // NOTE: init_session mutates the process-global UnifiedTrading
                // session_. In the daemon/headless host this is benign — only this
                // tool touches session_, so it is always "paper". In a GUI process
                // with an AI client attached, GUI live trading uses the
                // account-aware place_order(account_id, …) overload that ignores
                // session_, so this can only ever flip the shared session toward
                // paper, never toward unintended live.
                auto& ut = trading::UnifiedTrading::instance();
                auto sess = ut.get_session();
                if (!sess || sess->mode != QLatin1String("paper"))
                    ut.init_session("paper", "paper"); // $100k paper portfolio
                trading::UnifiedOrderResponse resp = ut.place_order(b.order);

                OrderDraftRepository::instance().update_status(
                    draft_id, resp.success ? "submitted" : "submit_failed");
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
                // ── REAL-MONEY EQUITY LIVE PATH (Phase C, Task 3) ──────────────
                // The kill switch was already enforced at the handler top (Task 1)
                // and the equity risk floor (rv) was RE-RUN fresh above. Every
                // remaining gate below is re-read LIVE (revocable) and the real
                // broker call — place_order(account, order) — is reachable ONLY
                // after ALL of them pass, in this order:
                //   armed → allowed-account → daily-loss → reserve → place_order.

                // (1) ARMED — BOTH the trading gate AND the live-arming flag must
                //     be on. The checker may have opened the door; the handler is
                //     the final authority (a human can un-arm mid-flight).
                if (!(mcp::cli_trading_allowed() && mcp::cli_live_armed())) {
                    const QString reason =
                        QStringLiteral("live trading not armed — arm in GUI Settings");
                    audit_submit(draft.account, QStringLiteral("live"), intent, "denied",
                                 reason, rv);
                    LOG_WARN(TAG, "submit_order live denied (not armed) draft " + draft_id);
                    return ToolResult::ok_data(QJsonObject{
                        {"status", "rejected"}, {"reason", reason}, {"mode", "live"}});
                }

                // (2) ALLOWED ACCOUNT — the single human-named live account. The AI
                //     only references this id; it never configures the account/mode.
                const QString acct = mcp::cli_allowed_account();
                if (acct.isEmpty()
                    || !trading::AccountManager::instance().has_account(acct)) {
                    const QString reason =
                        QStringLiteral("no allowed account configured for AI trading");
                    audit_submit(draft.account, QStringLiteral("live"), intent, "denied",
                                 reason, rv);
                    return ToolResult::ok_data(QJsonObject{
                        {"status", "rejected"}, {"reason", reason}, {"mode", "live"}});
                }
                // If the stored intent explicitly NAMES a different account, refuse:
                // the AI may trade only the human-named allowed account. (Checked on
                // the raw intent field, NOT draft.account — which defaults to
                // "paper-default" when no account was named, and would otherwise
                // reject every legitimate no-account submit.)
                const QString intent_account = intent.value("account").toString().trimmed();
                if (!intent_account.isEmpty() && intent_account != acct) {
                    const QString reason =
                        QStringLiteral("account not allowed for AI trading");
                    audit_submit(draft.account, QStringLiteral("live"), intent, "denied",
                                 reason, rv);
                    return ToolResult::ok_data(QJsonObject{
                        {"status", "rejected"}, {"reason", reason}, {"mode", "live"}});
                }

                // (3) DAILY-LOSS FLOOR — the running realized-loss tally plus this
                //     order's max_loss must stay within the GUI-only daily cap.
                if (!mcp::tools::daily_loss_ok(rv.max_loss)) {
                    const QString reason = QStringLiteral("daily loss limit reached");
                    audit_submit(acct, QStringLiteral("live"), intent, "denied", reason, rv);
                    LOG_WARN(TAG, "submit_order live denied (daily loss) draft " + draft_id);
                    return ToolResult::ok_data(QJsonObject{
                        {"status", "rejected"}, {"reason", reason}, {"mode", "live"}});
                }

                // (4) RESERVE — atomically claim the draft BEFORE the irreversible
                //     broker call (compare-and-set prepared→submitting). A duplicate
                //     or concurrent submit loses the race, so a real fill can never
                //     be double-executed.
                auto reserve =
                    OrderDraftRepository::instance().reserve_for_submit(draft_id);
                if (!reserve.is_ok() || !reserve.value()) {
                    const QString reason =
                        QStringLiteral("draft already used or not reservable");
                    audit_submit(acct, QStringLiteral("live"), intent, "rejected", reason, rv);
                    return ToolResult::ok_data(QJsonObject{
                        {"status", "rejected"}, {"reason", reason}, {"mode", "live"}});
                }

                // (5) FIRE — every gate passed. Route to the broker via the account
                //     overload: place_order(account_id, order) dispatches by the
                //     account's trading_mode (live/sandbox/demo → broker; else paper
                //     sim). Reuse the order built + risk-checked above.
                trading::UnifiedOrderResponse resp =
                    trading::UnifiedTrading::instance().place_order(acct, b.order);

                OrderDraftRepository::instance().update_status(
                    draft_id, resp.success ? "submitted" : "submit_failed");

                // Record the fill in the LIVE realized-P&L ledger. The broker
                // response carries NO fill price, so we record at the price the
                // equity floor resolved/used for rv (documented approximation).
                // Venue is "equity"; instrument is the symbol. BUY opens/adds,
                // SELL closes/reduces.
                if (resp.success) {
                    if (b.order.side == OrderSide::Buy)
                        mcp::tools::record_open(acct, QStringLiteral("equity"),
                                                b.order.symbol, b.order.quantity,
                                                b.resolved_price);
                    else
                        mcp::tools::record_close(acct, QStringLiteral("equity"),
                                                 b.order.symbol, b.order.quantity,
                                                 b.resolved_price);
                }

                const QString decision = resp.success ? QStringLiteral("filled")
                                                      : QStringLiteral("rejected");
                // trade_audit.reason is NOT NULL — synthesize a non-empty reason if
                // the broker returned an empty message.
                QString msg = resp.message.trimmed();
                if (msg.isEmpty())
                    msg = resp.success ? QStringLiteral("filled")
                                       : QStringLiteral("rejected");
                audit_submit(acct, QStringLiteral("live"), intent, decision, msg, rv);
                LOG_WARN(TAG, "submit_order LIVE " + decision + " draft " + draft_id);
                return ToolResult::ok_data(QJsonObject{
                    {"status", decision},
                    {"order_id", resp.order_id},
                    {"account", acct},
                    {"mode", "live"},
                    {"message", msg},
                });
            }

            // Schema enum should catch this; defensive only. Audited like every
            // other terminal path for completeness.
            audit_submit(draft.account, audit_mode, intent, "rejected", "unknown mode", rv);
            return ToolResult::ok_data(
                QJsonObject{{"status", "rejected"}, {"reason", "unknown mode"}});
        };
        tools.push_back(std::move(t));
    }

    return tools;
}

} // namespace openmarketterminal::mcp::tools
