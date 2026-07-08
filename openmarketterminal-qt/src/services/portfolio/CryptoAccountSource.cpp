// src/services/portfolio/CryptoAccountSource.cpp
#include "services/portfolio/CryptoAccountSource.h"

#include "storage/secure/SecureStorage.h"
#include "trading/ExchangeSession.h"
#include "trading/ExchangeSessionManager.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTimeZone>

namespace openmarketterminal::services {

namespace {
const QString kSourcePrefix = QStringLiteral("crypto:");

// Mirrors ExchangeSession's private `creds_key` convention
// ("crypto:<exchange>:<field>") — see trading/ExchangeSession.cpp.
QString creds_key(const QString& exchange_id, const QString& field) {
    return QStringLiteral("crypto:") + exchange_id + QLatin1Char(':') + field;
}

// Must agree with ExchangeSession::load_stored_credentials()'s definition of
// "credentials are stored" — ANY of these five fields counts, not just
// api_key. Wallet-authenticated exchanges (e.g. Hyperliquid) only ever set
// wallet_address/private_key and have no api_key, so checking api_key alone
// silently drops them from list_accounts().
bool has_stored_credentials(const QString& exchange_id) {
    auto& ss = SecureStorage::instance();
    static const QStringList fields = {"api_key", "secret", "password", "wallet_address", "private_key"};
    for (const auto& field : fields) {
        const auto r = ss.retrieve(creds_key(exchange_id, field));
        if (r.is_ok() && !r.value().isEmpty())
            return true;
    }
    return false;
}

QString title_case(const QString& id) {
    if (id.isEmpty())
        return id;
    return id.left(1).toUpper() + id.mid(1);
}

const QSet<QString>& fiat_currencies() {
    static const QSet<QString> fiat = {"USD", "USDT", "USDC", "EUR", "GBP", "CAD", "AUD", "JPY", "CHF"};
    return fiat;
}

} // namespace

CryptoAccountSource::BalanceFetcher CryptoAccountSource::default_fetcher() {
    return [](const QString& exchange_id) -> QJsonObject {
        // ExchangeSessionManager::session() lazily creates/returns the
        // session for `exchange_id` directly, so this does NOT go through
        // ExchangeService's single "active exchange" facade (no need to
        // flip and restore it around the call).
        auto* session = trading::ExchangeSessionManager::instance().session(exchange_id);
        if (!session)
            return QJsonObject{};
        return session->fetch_balance();
    };
}

CryptoAccountSource::TradesFetcher CryptoAccountSource::default_trades_fetcher() {
    return [](const QString& exchange_id, const QString& symbol) -> QJsonObject {
        auto* session = trading::ExchangeSessionManager::instance().session(exchange_id);
        if (!session)
            return QJsonObject{};
        return session->fetch_my_trades(symbol);
    };
}

CryptoAccountSource::CryptoAccountSource(BalanceFetcher fetcher, TradesFetcher trades_fetcher)
    : fetch_balance_(fetcher ? std::move(fetcher) : default_fetcher()),
      fetch_trades_(trades_fetcher ? std::move(trades_fetcher) : default_trades_fetcher()) {}

QVector<AccountRef> CryptoAccountSource::list_accounts() {
    QVector<AccountRef> out;
    for (const auto& id : trading::ExchangeSessionManager::supported_exchange_ids()) {
        if (!has_stored_credentials(id))
            continue;
        AccountRef ref;
        ref.sync_source = kSourcePrefix + id;
        ref.display_name = title_case(id);
        ref.base_currency = QStringLiteral("USD");
        out.append(ref);
    }
    return out;
}

portfolio::FetchResult CryptoAccountSource::fetch(const AccountRef& ref) {
    portfolio::FetchResult result;

    const QString exchange_id =
        ref.sync_source.startsWith(kSourcePrefix) ? ref.sync_source.mid(kSourcePrefix.size()) : ref.sync_source;

    const QJsonObject resp = fetch_balance_ ? fetch_balance_(exchange_id) : QJsonObject{};
    if (resp.contains("error") || !resp.contains("balances")) {
        result.ok = false;
        result.error = resp.contains("error")
                           ? resp.value("error").toString()
                           : QStringLiteral("fetch_balance failed for exchange '%1'").arg(exchange_id);
        return result;
    }

    const QJsonObject balances = resp.value("balances").toObject();
    QVector<portfolio::SyncedHolding> holdings;
    for (auto it = balances.constBegin(); it != balances.constEnd(); ++it) {
        const QString ccy = it.key();
        // PortfolioRepository::add_asset/remove_asset normalize the stored
        // symbol via .toUpper() — reconcile_mirror matches canonical_symbol
        // against that stored form case-sensitively, so canonical_symbol
        // must be uppercased at the source. fiat_currencies() is also an
        // uppercase set, so the classification check must compare against
        // the uppercased currency too, or a lowercase fiat key (e.g. "usd")
        // would be misclassified as a coin.
        const QString ccy_upper = ccy.toUpper();
        const double total = it.value().toObject().value("total").toDouble();
        if (total == 0.0)
            continue;

        if (fiat_currencies().contains(ccy_upper)) {
            portfolio::SyncedHolding cash;
            cash.canonical_symbol = QStringLiteral("$CASH:") + ccy_upper;
            cash.quantity = total;
            cash.avg_cost = 1.0;
            cash.has_cost_basis = false;
            cash.native_currency = ccy_upper;
            holdings.append(cash);
        } else {
            portfolio::SyncedHolding sh;
            sh.canonical_symbol = ccy_upper + QStringLiteral("-USD");
            sh.quantity = total;
            sh.avg_cost = 0.0;
            sh.has_cost_basis = false;
            sh.native_currency = QStringLiteral("USD");
            sh.broker_symbol = ccy + QStringLiteral("/USD");
            sh.exchange = exchange_id;
            holdings.append(sh);
        }
    }

    result.ok = true;
    result.holdings = holdings;
    return result;
}

QVector<portfolio::SyncedTransaction> CryptoAccountSource::fetch_transactions(
    const AccountRef& ref, const QVector<portfolio::SyncedHolding>& holdings) {
    QVector<portfolio::SyncedTransaction> out;

    const QString exchange_id =
        ref.sync_source.startsWith(kSourcePrefix) ? ref.sync_source.mid(kSourcePrefix.size()) : ref.sync_source;

    for (const auto& h : holdings) {
        // "$CASH:*" holdings have no ccxt pair to query — skip them. A coin
        // holding always carries a non-empty broker_symbol (see fetch()).
        if (h.canonical_symbol.startsWith(QStringLiteral("$CASH:")) || h.broker_symbol.isEmpty())
            continue;

        const QJsonObject resp = fetch_trades_ ? fetch_trades_(exchange_id, h.broker_symbol) : QJsonObject{};
        if (!resp.contains("trades"))
            continue;

        for (const auto& v : resp.value("trades").toArray()) {
            const QJsonObject t = v.toObject();
            const QString side = t.value("side").toString();
            const QString id = t.value("id").toVariant().toString();
            if (id.isEmpty())
                continue;

            portfolio::SyncedTransaction tx;
            tx.external_id = exchange_id + QLatin1Char(':') + id;
            tx.symbol = h.canonical_symbol;
            tx.type = side.compare(QStringLiteral("buy"), Qt::CaseInsensitive) == 0 ? QStringLiteral("BUY")
                                                                                     : QStringLiteral("SELL");
            tx.quantity = t.value("amount").toDouble();
            tx.price = t.value("price").toDouble();

            const QString datetime = t.value("datetime").toString();
            if (!datetime.isEmpty()) {
                tx.date = datetime;
            } else {
                const qint64 ms = static_cast<qint64>(t.value("timestamp").toDouble());
                tx.date = ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODate) : QString();
            }

            out.append(tx);
        }
    }

    return out;
}

} // namespace openmarketterminal::services
