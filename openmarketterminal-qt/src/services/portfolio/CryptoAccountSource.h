// src/services/portfolio/CryptoAccountSource.h
//
// CryptoAccountSource — IAccountSource backed by a connected crypto exchange
// session (trading::ExchangeSessionManager + trading::ExchangeSession).
// Read-only: only ever calls ExchangeSession::fetch_balance, never
// place_exchange_order/cancel_exchange_order or any other mutating call.
#pragma once
#include "services/portfolio/IAccountSource.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <functional>

namespace openmarketterminal::services {

class CryptoAccountSource : public IAccountSource {
  public:
    // Fetches the raw {"balances": {CCY: {"total": n, ...}}} JSON for one
    // exchange id (an ExchangeSessionManager::supported_exchange_ids()
    // entry), or {"error": ...} / an empty object on failure. Tests inject a
    // fake callback instead of going through the real
    // ExchangeSessionManager/ExchangeDaemonPool singletons.
    using BalanceFetcher = std::function<QJsonObject(const QString& exchange_id)>;

    explicit CryptoAccountSource(BalanceFetcher fetcher = default_fetcher());

    QVector<AccountRef> list_accounts() override;
    portfolio::FetchResult fetch(const AccountRef& ref) override;

  private:
    // The real fetch path: ExchangeSessionManager::instance().session(id)
    // (lazily creates/returns the session for `id` directly — no need to
    // flip ExchangeService's single "active exchange" — then
    // ExchangeSession::fetch_balance() on it.
    static BalanceFetcher default_fetcher();

    BalanceFetcher fetch_balance_;
};

} // namespace openmarketterminal::services
