// src/services/portfolio/IAccountSource.h
//
// IAccountSource — an abstract, read-only source of "accounts of one kind"
// (broker/equity, exchange/crypto, ...) that can be mirrored into a portfolio.
// list_accounts() enumerates the connected accounts of this source; fetch()
// pulls one account's current holdings + cash, normalized to SyncedHolding
// (see AccountSyncTypes.h). Implementations MUST be read-only — no order
// placement, no mutation of the underlying account.
#pragma once
#include "services/portfolio/AccountSyncTypes.h"

#include <QString>
#include <QVector>

namespace openmarketterminal::services {

/// One connected account this source can fetch. sync_source is the stable key
/// stored on a portfolio to remember which account it mirrors (e.g.
/// "broker:<account_id>"); display_name is for the UI; base_currency is the
/// account's settlement currency, used to tag the synced cash line
/// ("$CASH:<base_currency>") and native_currency on holdings.
struct AccountRef {
    QString sync_source;
    QString display_name;
    QString base_currency;
};

class IAccountSource {
  public:
    virtual ~IAccountSource() = default;

    /// Connected accounts of this kind, ready to be fetched/mirrored.
    virtual QVector<AccountRef> list_accounts() = 0;

    /// Read-only fetch of one account's holdings + cash. On any underlying
    /// error, returns FetchResult{ok=false, error, holdings={}} — callers must
    /// NOT mirror a failed fetch (see FetchResult doc in AccountSyncTypes.h).
    virtual portfolio::FetchResult fetch(const AccountRef& ref) = 0;
};

} // namespace openmarketterminal::services
