// src/services/portfolio/AccountSyncService.h
//
// AccountSyncService — orchestrates Portfolio Account Sync: iterates
// registered IAccountSource implementations, mirror-writes each connected
// account's holdings into the portfolio it's synced to (via
// PortfolioRepository + AccountSyncTypes::reconcile_mirror). READ-ONLY
// feature: only ever calls IAccountSource::list_accounts/fetch (never a
// broker/exchange mutating method) and writes ONLY to the local
// portfolios/portfolio_assets tables. Manual portfolios (sync_source == '')
// are never touched — this service only operates on the portfolio it
// finds/creates for a given sync_source.
//
// Never-wipe-on-failure invariant: a failed fetch (FetchResult::ok == false)
// records sync_error and returns WITHOUT touching portfolio_assets — prior
// holdings survive a rate-limited/network-down source.
#pragma once
#include "services/portfolio/IAccountSource.h"

#include <QObject>
#include <QString>
#include <QVector>

namespace openmarketterminal::services {

class AccountSyncService : public QObject {
    Q_OBJECT
  public:
    static AccountSyncService& instance();

    /// Registers a source to be swept by sync_all(). The service does not
    /// take ownership — sources are expected to be process-lifetime
    /// singletons/statics, mirroring PredictionExchangeRegistry.
    void register_source(IAccountSource* src);

    /// Enumerates every registered source's connected accounts and syncs each.
    void sync_all();

    /// Syncs one account: ensures/finds its mirrored portfolio, fetches
    /// current holdings, and either mirror-writes them (on success) or
    /// records the error without touching assets (on failure).
    void sync_account(const AccountRef& ref, IAccountSource* src);

  signals:
    void sync_started();
    void account_synced(QString sync_source, bool ok, QString error);
    void sync_finished();

  private:
    AccountSyncService();

    /// Finds the portfolio mirroring ref.sync_source, creating it (and
    /// stamping its sync_source) if absent. Returns an empty id on failure.
    QString ensure_portfolio(const AccountRef& ref);

    QVector<IAccountSource*> sources_;
};

} // namespace openmarketterminal::services
