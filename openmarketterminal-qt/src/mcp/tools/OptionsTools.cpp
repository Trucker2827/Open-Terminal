// OptionsTools.cpp — read-only option-contract discovery for the AI/CLI.
//
// Exposes ONE non-destructive tool, `get_option_contracts`, that lists tradable
// OCC option symbols for an underlying so the model can pick a concrete contract
// before drafting an order. Routing mirrors the live-trade tools: it resolves the
// single account a human has authorised in GUI Settings (cli.allowed_account),
// grabs that account's broker, and delegates to IBroker::get_option_contracts.
// Discovery itself is harmless (read-only), but reusing the allowed account keeps
// the symbols consistent with the venue the AI is actually permitted to trade.

#include "mcp/tools/OptionsTools.h"

#include "mcp/tools/SettingsGate.h"
#include "trading/AccountManager.h"
#include "trading/BrokerInterface.h"

#include <QJsonArray>
#include <QJsonObject>

namespace openmarketterminal::mcp::tools {

std::vector<ToolDef> get_options_tools() {
    ToolDef t;
    t.name = "get_option_contracts";
    t.description = "List tradable option contracts for an underlying (Alpaca). Returns OCC "
                    "symbols + strike/expiry/type/last price so you can pick a contract to trade.";
    t.category = "markets";
    t.is_destructive = false;
    t.input_schema.properties = QJsonObject{
        {"underlying", QJsonObject{{"type", "string"},
                                   {"description", "Underlying ticker symbol (e.g. AAPL, SPY)"}}},
        {"type", QJsonObject{{"type", "string"},
                             {"enum", QJsonArray{"call", "put"}},
                             {"description", "Filter by option type: 'call' or 'put' (optional)"}}},
        {"expiry_gte", QJsonObject{{"type", "string"},
                                   {"description", "Earliest expiry, YYYY-MM-DD (optional)"}}},
        {"expiry_lte", QJsonObject{{"type", "string"},
                                   {"description", "Latest expiry, YYYY-MM-DD (optional)"}}},
        {"strike_gte", QJsonObject{{"type", "number"},
                                   {"description", "Minimum strike price (optional)"}}},
        {"strike_lte", QJsonObject{{"type", "number"},
                                   {"description", "Maximum strike price (optional)"}}},
        {"limit", QJsonObject{{"type", "number"},
                              {"description", "Max contracts to return (optional)"}}}};
    t.input_schema.required = {"underlying"};
    t.handler = [](const QJsonObject& args) -> ToolResult {
        const QString underlying = args.value("underlying").toString().trimmed();
        if (underlying.isEmpty())
            return ToolResult::fail("Missing 'underlying'");
        const QString acct = mcp::cli_allowed_account();
        auto& am = trading::AccountManager::instance();
        if (acct.isEmpty() || !am.has_account(acct))
            return ToolResult::fail("no allowed account configured for option discovery");
        trading::IBroker* broker = am.broker_for(acct);
        if (!broker)
            return ToolResult::fail("broker unavailable for the allowed account");
        const auto creds = am.load_credentials(acct);
        const auto resp = broker->get_option_contracts(creds, args);
        if (!resp.success || !resp.data.has_value())
            return ToolResult::fail(resp.error.isEmpty() ? "option discovery failed" : resp.error);
        return ToolResult::ok_data(QJsonObject{{"contracts", resp.data.value()},
                                               {"count", resp.data.value().size()}});
    };
    return {t};
}

} // namespace openmarketterminal::mcp::tools
