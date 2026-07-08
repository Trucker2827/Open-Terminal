// src/services/portfolio/EquityAccountSource.cpp
#include "services/portfolio/EquityAccountSource.h"

#include "trading/AccountManager.h"
#include "trading/BrokerInterface.h"
#include "trading/BrokerRegistry.h"

namespace openmarketterminal::services {

namespace {
const QString kSourcePrefix = QStringLiteral("broker:");
}

EquityAccountSource::BrokerResolver EquityAccountSource::default_resolver() {
    return [](const QString& account_id, trading::BrokerCredentials& creds) -> trading::IBroker* {
        auto& acct_mgr = trading::AccountManager::instance();
        const auto account = acct_mgr.get_account(account_id);
        if (account.account_id.isEmpty() || account.broker_id.isEmpty())
            return nullptr;

        auto* broker = trading::BrokerRegistry::instance().get(account.broker_id);
        if (!broker)
            return nullptr;

        creds = acct_mgr.load_credentials(account_id);
        return broker;
    };
}

EquityAccountSource::EquityAccountSource(BrokerResolver resolver)
    : resolve_broker_(resolver ? std::move(resolver) : default_resolver()) {}

QVector<AccountRef> EquityAccountSource::list_accounts() {
    QVector<AccountRef> out;
    const auto accounts = trading::AccountManager::instance().list_accounts();
    out.reserve(accounts.size());
    for (const auto& account : accounts) {
        AccountRef ref;
        ref.sync_source = kSourcePrefix + account.account_id;
        ref.display_name = account.display_name;

        // BrokerAccount itself carries no currency — the broker's registered
        // profile is the source of truth for its settlement currency.
        QString ccy;
        if (auto* broker = trading::BrokerRegistry::instance().get(account.broker_id))
            ccy = broker->profile().currency;
        ref.base_currency = ccy.isEmpty() ? QStringLiteral("USD") : ccy;

        out.append(ref);
    }
    return out;
}

portfolio::FetchResult EquityAccountSource::fetch(const AccountRef& ref) {
    portfolio::FetchResult result;

    const QString account_id =
        ref.sync_source.startsWith(kSourcePrefix) ? ref.sync_source.mid(kSourcePrefix.size()) : ref.sync_source;

    trading::BrokerCredentials creds;
    trading::IBroker* broker = resolve_broker_ ? resolve_broker_(account_id, creds) : nullptr;
    if (!broker) {
        result.ok = false;
        result.error = QStringLiteral("could not resolve broker for account '%1'").arg(account_id);
        return result;
    }

    const QString base_ccy = ref.base_currency.isEmpty() ? QStringLiteral("USD") : ref.base_currency;

    const auto holdings_resp = broker->get_holdings(creds);
    if (!holdings_resp.success) {
        result.ok = false;
        result.error = holdings_resp.error.isEmpty() ? QStringLiteral("get_holdings failed") : holdings_resp.error;
        return result;
    }

    QVector<portfolio::SyncedHolding> holdings;
    for (const auto& h : holdings_resp.data.value_or(QVector<trading::BrokerHolding>{})) {
        portfolio::SyncedHolding sh;
        sh.canonical_symbol = h.symbol;
        sh.quantity = h.quantity;
        sh.avg_cost = h.avg_price;
        sh.has_cost_basis = true;
        sh.native_currency = base_ccy;
        sh.broker_symbol = h.symbol;
        sh.exchange = h.exchange;
        holdings.append(sh);
    }

    const auto funds_resp = broker->get_funds(creds);
    if (!funds_resp.success) {
        result.ok = false;
        result.error = funds_resp.error.isEmpty() ? QStringLiteral("get_funds failed") : funds_resp.error;
        return result;
    }

    portfolio::SyncedHolding cash;
    cash.canonical_symbol = QStringLiteral("$CASH:") + base_ccy;
    cash.quantity = funds_resp.data.value_or(trading::BrokerFunds{}).available_balance;
    cash.avg_cost = 1.0;
    cash.has_cost_basis = false;
    cash.native_currency = base_ccy;
    holdings.append(cash);

    result.ok = true;
    result.holdings = holdings;
    return result;
}

} // namespace openmarketterminal::services
