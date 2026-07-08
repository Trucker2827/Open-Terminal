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

    // Fetches the raw {"trades":[{id, order, symbol, side, price, amount,
    // timestamp, datetime, ...}]} JSON for one (exchange_id, ccxt-pair
    // symbol) — the same shape ExchangeSession::fetch_my_trades /
    // exchange_daemon.py's _handle_fetch_my_trades returns. Tests inject a
    // fake callback instead of going through the real
    // ExchangeSessionManager/ExchangeDaemonPool singletons.
    using TradesFetcher = std::function<QJsonObject(const QString& exchange_id, const QString& symbol)>;

    explicit CryptoAccountSource(BalanceFetcher fetcher = default_fetcher(),
                                 TradesFetcher trades_fetcher = default_trades_fetcher());

    QVector<AccountRef> list_accounts() override;
    portfolio::FetchResult fetch(const AccountRef& ref) override;
    /// For every non-cash holding's broker_symbol (the ccxt pair, e.g.
    /// "BTC/USD"; "$CASH:*" holdings are skipped), fetches that symbol's
    /// trade history and maps each fill to a SyncedTransaction. Bounded by
    /// the number of held coins — one call per holding, not a full-account
    /// trade dump.
    QVector<portfolio::SyncedTransaction> fetch_transactions(const AccountRef& ref,
                                                              const QVector<portfolio::SyncedHolding>& holdings) override;

  private:
    // The real fetch path: ExchangeSessionManager::instance().session(id)
    // (lazily creates/returns the session for `id` directly — no need to
    // flip ExchangeService's single "active exchange" — then
    // ExchangeSession::fetch_balance() on it.
    static BalanceFetcher default_fetcher();
    // The real trades path: ExchangeSessionManager::instance().session(id)
    // then ExchangeSession::fetch_my_trades(symbol) on it — same session
    // resolution as default_fetcher().
    static TradesFetcher default_trades_fetcher();

    BalanceFetcher fetch_balance_;
    TradesFetcher fetch_trades_;
};

} // namespace openmarketterminal::services
