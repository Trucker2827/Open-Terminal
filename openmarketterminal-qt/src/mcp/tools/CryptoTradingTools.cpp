// CryptoTradingTools.cpp — Crypto Trading tab MCP tools (ticker, order book, candles, exchange info)

#include "mcp/tools/CryptoTradingTools.h"

#include "core/logging/Logger.h"
#include "mcp/tools/LivePnl.h"
#include "mcp/tools/SettingsGate.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/repositories/TradeAuditRepository.h"
#include "trading/CryptoRisk.h"
#include "trading/ExchangeService.h"

#include <QDateTime>
#include <QJsonDocument>

namespace openmarketterminal::mcp::tools {

namespace {
constexpr const char* kCryptoTag = "CryptoTradingTools";

// Mirrors FastLiveTools::read_cap (anon-ns there, not exported): a missing /
// empty / garbage / non-positive value falls back to the conservative finite
// default — NEVER "no cap".
double crypto_read_cap(const QString& key, double default_val) {
    auto r = SettingsRepository::instance().get(key, QString());
    if (!r.is_ok())
        return default_val;
    bool ok = false;
    const double v = r.value().toDouble(&ok);
    return (!ok || v <= 0.0) ? default_val : v;
}

// Mirrors FastLiveTools::fast_derisk_audit but phase="crypto-live". Every
// terminal path (gate-denied or exchange-returned) records an append-only row.
void crypto_exec_audit(const QString& tool, const QString& venue, const QString& decision,
                       const QString& reason, const QJsonObject& intent) {
    TradeAuditRow row;
    row.ts = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    row.phase = QStringLiteral("crypto-live");
    row.tool = tool;
    row.account = venue;
    row.mode = QStringLiteral("live");
    row.intent_json = QString::fromUtf8(QJsonDocument(intent).toJson(QJsonDocument::Compact));
    row.decision = decision;
    row.reason = reason;
    row.risk_snapshot_json = QStringLiteral("{}");
    auto r = TradeAuditRepository::instance().append(row);
    if (!r.is_ok())
        LOG_WARN(kCryptoTag, "crypto trade_audit append failed: " + QString::fromStdString(r.error()));
}
} // namespace

std::vector<ToolDef> get_crypto_trading_tools() {
    std::vector<ToolDef> tools;

    // ── get_ticker ─────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_ticker";
        t.description = "Get the latest price, volume, and change for a symbol from the configured exchange.";
        t.category = "crypto-trading";
        t.input_schema.properties = QJsonObject{
            {"symbol", QJsonObject{{"type", "string"}, {"description", "Trading pair (e.g. BTC/USDT, ETH/USDT)"}}}};
        t.input_schema.required = {"symbol"};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            QString symbol = args["symbol"].toString().trimmed();
            if (symbol.isEmpty())
                return ToolResult::fail("Missing 'symbol'");

            auto& svc = trading::ExchangeService::instance();
            if (svc.get_exchange().isEmpty())
                return ToolResult::fail("No exchange configured — set one in Crypto Trading first");

            try {
                auto ticker = svc.fetch_ticker(symbol);
                return ToolResult::ok_data(QJsonObject{{"symbol", ticker.symbol},
                                                       {"last", ticker.last},
                                                       {"bid", ticker.bid},
                                                       {"ask", ticker.ask},
                                                       {"high", ticker.high},
                                                       {"low", ticker.low},
                                                       {"open", ticker.open},
                                                       {"close", ticker.close},
                                                       {"change", ticker.change},
                                                       {"change_pct", ticker.percentage},
                                                       {"volume", ticker.base_volume},
                                                       {"quote_volume", ticker.quote_volume},
                                                       {"timestamp", static_cast<double>(ticker.timestamp)}});
            } catch (const std::exception& e) {
                return ToolResult::fail(e.what());
            }
        };
        tools.push_back(std::move(t));
    }

    // ── get_order_book ─────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_order_book";
        t.description = "Get the order book (bids and asks) for a symbol.";
        t.category = "crypto-trading";
        t.input_schema.properties =
            QJsonObject{{"symbol", QJsonObject{{"type", "string"}, {"description", "Trading pair"}}},
                        {"limit", QJsonObject{{"type", "integer"}, {"description", "Number of levels (default: 20)"}}}};
        t.input_schema.required = {"symbol"};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            QString symbol = args["symbol"].toString().trimmed();
            int limit = args["limit"].toInt(20);
            if (symbol.isEmpty())
                return ToolResult::fail("Missing 'symbol'");

            auto& svc = trading::ExchangeService::instance();
            if (svc.get_exchange().isEmpty())
                return ToolResult::fail("No exchange configured");

            try {
                auto ob = svc.fetch_orderbook(symbol, limit);
                QJsonArray bids, asks;
                for (const auto& level : ob.bids)
                    bids.append(QJsonObject{{"price", level.first}, {"amount", level.second}});
                for (const auto& level : ob.asks)
                    asks.append(QJsonObject{{"price", level.first}, {"amount", level.second}});

                return ToolResult::ok_data(QJsonObject{{"symbol", ob.symbol},
                                                       {"best_bid", ob.best_bid},
                                                       {"best_ask", ob.best_ask},
                                                       {"spread", ob.spread},
                                                       {"spread_pct", ob.spread_pct},
                                                       {"bids", bids},
                                                       {"asks", asks}});
            } catch (const std::exception& e) {
                return ToolResult::fail(e.what());
            }
        };
        tools.push_back(std::move(t));
    }

    // ── get_candles ────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_candles";
        t.description = "Get OHLCV candle data for a symbol. Timeframes: 1m, 5m, 15m, 1h, 4h, 1d.";
        t.category = "crypto-trading";
        t.input_schema.properties = QJsonObject{
            {"symbol", QJsonObject{{"type", "string"}, {"description", "Trading pair"}}},
            {"timeframe", QJsonObject{{"type", "string"}, {"description", "Candle interval (default: 1h)"}}},
            {"limit", QJsonObject{{"type", "integer"}, {"description", "Number of candles (default: 100)"}}}};
        t.input_schema.required = {"symbol"};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            QString symbol = args["symbol"].toString().trimmed();
            QString timeframe = args["timeframe"].toString("1h");
            int limit = args["limit"].toInt(100);
            if (symbol.isEmpty())
                return ToolResult::fail("Missing 'symbol'");

            auto& svc = trading::ExchangeService::instance();
            if (svc.get_exchange().isEmpty())
                return ToolResult::fail("No exchange configured");

            try {
                auto candles = svc.fetch_ohlcv(symbol, timeframe, limit);
                QJsonArray result;
                for (const auto& c : candles) {
                    result.append(QJsonObject{{"timestamp", static_cast<double>(c.timestamp)},
                                              {"open", c.open},
                                              {"high", c.high},
                                              {"low", c.low},
                                              {"close", c.close},
                                              {"volume", c.volume}});
                }
                return ToolResult::ok_data(result);
            } catch (const std::exception& e) {
                return ToolResult::fail(e.what());
            }
        };
        tools.push_back(std::move(t));
    }

    // ── get_exchange_info ──────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_exchange_info";
        t.description = "Get the currently configured exchange name and list all available exchange IDs.";
        t.category = "crypto-trading";
        t.handler = [](const QJsonObject&) -> ToolResult {
            auto& svc = trading::ExchangeService::instance();
            QString current = svc.get_exchange();

            try {
                auto ids = svc.list_exchange_ids();
                QJsonArray id_arr;
                for (const auto& id : ids)
                    id_arr.append(id);

                return ToolResult::ok_data(QJsonObject{{"current_exchange", current.isEmpty() ? "none" : current},
                                                       {"available_exchanges", id_arr}});
            } catch (...) {
                return ToolResult::ok_data(QJsonObject{{"current_exchange", current.isEmpty() ? "none" : current},
                                                       {"available_exchanges", QJsonArray{}}});
            }
        };
        tools.push_back(std::move(t));
    }

    // ── get_crypto_balance ─────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_crypto_balance";
        t.description = "Get account balances (per-currency free/used/total) from the configured crypto exchange.";
        t.category = "crypto-trading";
        t.handler = [](const QJsonObject&) -> ToolResult {
            auto& svc = trading::ExchangeService::instance();
            if (svc.get_exchange().isEmpty())
                return ToolResult::fail("No exchange configured");
            try {
                const QJsonObject r = svc.fetch_balance();
                if (r.contains("error"))
                    return ToolResult::fail(r.value("error").toString());
                return ToolResult::ok_data(r);
            } catch (const std::exception& e) {
                return ToolResult::fail(e.what());
            }
        };
        tools.push_back(std::move(t));
    }

    // ── get_crypto_open_orders ─────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_crypto_open_orders";
        t.description = "Get open orders on the configured crypto exchange (optionally filtered by symbol).";
        t.category = "crypto-trading";
        t.input_schema.properties = QJsonObject{
            {"symbol", QJsonObject{{"type", "string"}, {"description", "Optional pair filter (e.g. BTC/USD)"}}}};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            auto& svc = trading::ExchangeService::instance();
            if (svc.get_exchange().isEmpty())
                return ToolResult::fail("No exchange configured");
            try {
                const QJsonObject r = svc.fetch_open_orders_live(args.value("symbol").toString().trimmed());
                if (r.contains("error"))
                    return ToolResult::fail(r.value("error").toString());
                return ToolResult::ok_data(r);
            } catch (const std::exception& e) {
                return ToolResult::fail(e.what());
            }
        };
        tools.push_back(std::move(t));
    }

    // ── get_crypto_trades ──────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_crypto_trades";
        t.description = "Get recent personal fills for a symbol on the configured crypto exchange.";
        t.category = "crypto-trading";
        t.input_schema.properties = QJsonObject{
            {"symbol", QJsonObject{{"type", "string"}, {"description", "Trading pair (e.g. BTC/USD)"}}},
            {"limit", QJsonObject{{"type", "integer"}, {"description", "Max fills (default 50, cap 200)"}}}};
        t.input_schema.required = {"symbol"};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            const QString symbol = args.value("symbol").toString().trimmed();
            if (symbol.isEmpty())
                return ToolResult::fail("Missing 'symbol'");
            int limit = args.value("limit").toInt(50);
            if (limit <= 0 || limit > 200) limit = 50;
            auto& svc = trading::ExchangeService::instance();
            if (svc.get_exchange().isEmpty())
                return ToolResult::fail("No exchange configured");
            try {
                const QJsonObject r = svc.fetch_my_trades(symbol, limit);
                if (r.contains("error"))
                    return ToolResult::fail(r.value("error").toString());
                return ToolResult::ok_data(r);
            } catch (const std::exception& e) {
                return ToolResult::fail(e.what());
            }
        };
        tools.push_back(std::move(t));
    }

    // ── crypto_submit_order (GATED, real-money) ────────────────────────
    {
        ToolDef t;
        t.name = "crypto_submit_order";
        t.description = "Place a spot order on the configured crypto exchange (market or limit). "
                        "Gated: requires the live arms; enforces kill-switch, allowed-venue, and the "
                        "deterministic risk floor (cli.risk.max_order_value / max_daily_loss).";
        t.category = "crypto-trading";
        t.is_destructive = true;
        t.auth_required = AuthLevel::Authenticated;
        t.input_schema.properties = QJsonObject{
            {"symbol", QJsonObject{{"type", "string"}, {"description", "Pair (e.g. BTC/USD)"}}},
            {"side", QJsonObject{{"type", "string"}, {"description", "buy or sell"}}},
            {"quantity", QJsonObject{{"type", "number"}, {"description", "Base-asset amount (> 0)"}}},
            {"order_type", QJsonObject{{"type", "string"}, {"description", "market or limit (default market)"}}},
            {"limit_price", QJsonObject{{"type", "number"}, {"description", "Required for limit orders"}}}};
        t.input_schema.required = {"symbol", "side", "quantity"};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            const QString symbol = args.value("symbol").toString().trimmed();
            const QString side = args.value("side").toString().trimmed().toLower();
            const double quantity = args.value("quantity").toDouble(0.0);
            const QString otype = args.value("order_type").toString("market").trimmed().toLower();
            auto& svc = trading::ExchangeService::instance();
            const QString venue = svc.get_exchange();
            QJsonObject intent{{"symbol", symbol}, {"side", side}, {"quantity", quantity},
                               {"order_type", otype}};
            if (args.contains("limit_price")) intent["limit_price"] = args.value("limit_price").toDouble();

            if (mcp::cli_kill_switch_engaged()) {
                crypto_exec_audit("crypto_submit_order", venue, "denied", "kill switch engaged", intent);
                return ToolResult::fail("Refused: kill switch engaged");
            }
            if (!(mcp::cli_trading_allowed() && mcp::cli_live_armed() && mcp::cli_fast_live_armed())) {
                crypto_exec_audit("crypto_submit_order", venue, "denied", "not armed", intent);
                return ToolResult::fail("Refused: live trading not armed");
            }
            if (venue.isEmpty()) {
                crypto_exec_audit("crypto_submit_order", venue, "denied", "no exchange configured", intent);
                return ToolResult::fail("No exchange configured");
            }
            if (!mcp::cli_venue_allowed(venue)) {
                crypto_exec_audit("crypto_submit_order", venue, "denied", "venue not allowed: " + venue, intent);
                return ToolResult::fail("Refused: venue not allowed (" + venue + ")");
            }
            if (side != "buy" && side != "sell") {
                crypto_exec_audit("crypto_submit_order", venue, "denied", "invalid side", intent);
                return ToolResult::fail("side must be buy or sell");
            }

            double price = 0.0;
            if (otype == "limit") {
                price = args.value("limit_price").toDouble(0.0);
            } else {
                price = svc.get_cached_price(symbol).last;
                if (price <= 0.0) {
                    try { price = svc.fetch_ticker(symbol).last; } catch (...) { price = 0.0; }
                }
            }
            const double cap = crypto_read_cap(QStringLiteral("cli.risk.max_order_value"), 25000.0);
            const auto rv = trading::crypto_risk_verdict(quantity, price, cap);
            if (!rv.ok) {
                crypto_exec_audit("crypto_submit_order", venue, "denied", rv.reason, intent);
                return ToolResult::fail("Risk floor: " + rv.reason);
            }
            if (!daily_loss_ok(rv.order_value)) {
                crypto_exec_audit("crypto_submit_order", venue, "denied", "daily loss cap", intent);
                return ToolResult::fail("Risk floor: daily loss cap would be exceeded");
            }

            try {
                const double limit_px = (otype == "limit") ? price : 0.0;
                const QJsonObject res = svc.place_exchange_order(symbol, side, otype, quantity, limit_px);
                if (res.contains("error") || (res.contains("success") && !res.value("success").toBool())) {
                    const QString msg = res.value("error").toString(res.value("message").toString("exchange error"));
                    crypto_exec_audit("crypto_submit_order", venue, "rejected", msg, intent);
                    return ToolResult::fail("Exchange rejected order: " + msg);
                }
                const QString status = res.value("status").toString(res.value("data").toObject().value("status").toString("submitted"));
                crypto_exec_audit("crypto_submit_order", venue, status.isEmpty() ? "submitted" : status, "", intent);
                return ToolResult::ok_data(res);
            } catch (const std::exception& e) {
                crypto_exec_audit("crypto_submit_order", venue, "rejected", e.what(), intent);
                return ToolResult::fail(e.what());
            }
        };
        tools.push_back(std::move(t));
    }

    // ── crypto_cancel_order (GATED) ────────────────────────────────────
    {
        ToolDef t;
        t.name = "crypto_cancel_order";
        t.description = "Cancel an open order on the configured crypto exchange. Gated: requires the live arms.";
        t.category = "crypto-trading";
        t.is_destructive = true;
        t.auth_required = AuthLevel::Authenticated;
        t.input_schema.properties = QJsonObject{
            {"order_id", QJsonObject{{"type", "string"}, {"description", "Exchange order id"}}},
            {"symbol", QJsonObject{{"type", "string"}, {"description", "Pair the order is on (e.g. BTC/USD)"}}}};
        t.input_schema.required = {"order_id", "symbol"};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            const QString order_id = args.value("order_id").toString().trimmed();
            const QString symbol = args.value("symbol").toString().trimmed();
            auto& svc = trading::ExchangeService::instance();
            const QString venue = svc.get_exchange();
            QJsonObject intent{{"order_id", order_id}, {"symbol", symbol}};
            if (mcp::cli_kill_switch_engaged()) {
                crypto_exec_audit("crypto_cancel_order", venue, "denied", "kill switch engaged", intent);
                return ToolResult::fail("Refused: kill switch engaged");
            }
            if (!(mcp::cli_trading_allowed() && mcp::cli_live_armed() && mcp::cli_fast_live_armed())) {
                crypto_exec_audit("crypto_cancel_order", venue, "denied", "not armed", intent);
                return ToolResult::fail("Refused: live trading not armed");
            }
            if (venue.isEmpty() || !mcp::cli_venue_allowed(venue)) {
                crypto_exec_audit("crypto_cancel_order", venue, "denied", "venue not allowed", intent);
                return ToolResult::fail("Refused: venue not allowed");
            }
            if (order_id.isEmpty() || symbol.isEmpty())
                return ToolResult::fail("order_id and symbol are required");
            try {
                const QJsonObject res = svc.cancel_exchange_order(order_id, symbol);
                if (res.contains("error")) {
                    crypto_exec_audit("crypto_cancel_order", venue, "rejected", res.value("error").toString(), intent);
                    return ToolResult::fail("Cancel failed: " + res.value("error").toString());
                }
                crypto_exec_audit("crypto_cancel_order", venue, "cancelled", "", intent);
                return ToolResult::ok_data(res);
            } catch (const std::exception& e) {
                crypto_exec_audit("crypto_cancel_order", venue, "rejected", e.what(), intent);
                return ToolResult::fail(e.what());
            }
        };
        tools.push_back(std::move(t));
    }

    return tools;
}

} // namespace openmarketterminal::mcp::tools
