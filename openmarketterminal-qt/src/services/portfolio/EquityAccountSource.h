// src/services/portfolio/EquityAccountSource.h
//
// EquityAccountSource — IAccountSource backed by a connected broker account
// (trading::AccountManager + trading::BrokerRegistry). Read-only: only ever
// calls IBroker::get_holdings / get_funds, never place_order/cancel or any
// other mutating broker method.
#pragma once
#include "services/portfolio/IAccountSource.h"
#include "trading/TradingTypes.h"

#include <QString>
#include <QVector>

#include <functional>

namespace openmarketterminal::trading {
class IBroker;
}

namespace openmarketterminal::services {

class EquityAccountSource : public IAccountSource {
  public:
    // Resolves an account_id ("broker:<account_id>" with the prefix stripped)
    // to the IBroker* that serves it, filling `creds` with the credentials to
    // call it with. Returns nullptr when the account/broker/credentials can't
    // be resolved. Tests inject a fake broker via this callback instead of
    // going through the real AccountManager/BrokerRegistry singletons.
    using BrokerResolver =
        std::function<trading::IBroker*(const QString& account_id, trading::BrokerCredentials& creds)>;

    explicit EquityAccountSource(BrokerResolver resolver = default_resolver());

    QVector<AccountRef> list_accounts() override;
    portfolio::FetchResult fetch(const AccountRef& ref) override;
    /// Maps every FILLED order (filled_qty > 0) from IBroker::get_orders to a
    /// SyncedTransaction. Ignores `holdings` — get_orders already returns
    /// every order for the account, so there's no need to enumerate held
    /// symbols first. On a get_orders error, returns {} (never throws,
    /// mirrors fetch()'s read-only contract).
    QVector<portfolio::SyncedTransaction> fetch_transactions(const AccountRef& ref,
                                                              const QVector<portfolio::SyncedHolding>& holdings) override;

  private:
    // The real resolution path: trading::AccountManager::get_account() +
    // trading::BrokerRegistry::get() + trading::AccountManager::load_credentials(),
    // mirroring PortfolioService::try_broker_quotes's broker+creds resolution.
    static BrokerResolver default_resolver();

    BrokerResolver resolve_broker_;
};

} // namespace openmarketterminal::services
