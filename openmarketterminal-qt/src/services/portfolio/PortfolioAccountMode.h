// src/services/portfolio/PortfolioAccountMode.h
#pragma once
// Pure helper deciding whether a synced portfolio's source account is a PAPER
// (fake-money) account, so the "All Accounts" aggregate can exclude paper money
// from the real-money total and the selector can badge it "(paper)".
//
// The broker trading-mode lookup is injected (a callback) so this stays free of
// the AccountManager singleton and is unit-testable. Crypto exchanges have no
// paper mode — they are always live/real.

#include <QString>

#include <functional>

namespace openmarketterminal::portfolio {

/// @param sync_source  e.g. "broker:<account_id>" or "crypto:<exchange>".
/// @param broker_mode  maps a broker account_id -> its trading_mode ("live" /
///                     "paper"); only called for "broker:" sources.
/// Returns true when the source is a paper (fake-money) broker account.
inline bool sync_source_is_paper(const QString& sync_source,
                                 const std::function<QString(const QString&)>& broker_mode) {
    static const QLatin1String kBroker("broker:");
    if (sync_source.startsWith(kBroker)) {
        const QString account_id = sync_source.mid(kBroker.size());
        return broker_mode(account_id).compare(QLatin1String("paper"), Qt::CaseInsensitive) == 0;
    }
    return false; // crypto (and anything else) is live/real
}

} // namespace openmarketterminal::portfolio
