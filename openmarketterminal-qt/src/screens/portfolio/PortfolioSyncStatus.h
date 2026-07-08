// src/screens/portfolio/PortfolioSyncStatus.h
#pragma once
// Pure text builder for the Portfolio toolbar's account-sync status label,
// split out of PortfolioScreen so the wording (and the "no accounts" empty
// state) can be unit-tested without standing up the screen.
//
// Deliberately says "accounts" (not "portfolio") so it never implies the
// currently-viewed manual portfolio was synced — the label reflects the state
// of the user's CONNECTED ACCOUNTS, globally.

#include <QDateTime>
#include <QString>

namespace openmarketterminal::portfolio {

/// @param account_count   number of synced (account-mirrored) portfolios.
/// @param newest_synced   most recent successful sync time (invalid if none yet).
/// @param now             current time (UTC), injected for testability.
/// Returns:
///   - "No accounts connected"        when account_count <= 0
///   - "Accounts not yet synced"      when there are accounts but no sync time
///   - "N account(s) synced <when>"   otherwise (<when> = just now / Nm / Nh / Nd ago)
inline QString sync_status_text(int account_count, const QDateTime& newest_synced, const QDateTime& now) {
    if (account_count <= 0)
        return QStringLiteral("No accounts connected");
    if (!newest_synced.isValid())
        return QStringLiteral("Accounts not yet synced");

    const qint64 secs = qMax<qint64>(0, newest_synced.secsTo(now));
    QString when;
    if (secs < 60)
        when = QStringLiteral("just now");
    else if (secs < 3600)
        when = QStringLiteral("%1m ago").arg(secs / 60);
    else if (secs < 86400)
        when = QStringLiteral("%1h ago").arg(secs / 3600);
    else
        when = QStringLiteral("%1d ago").arg(secs / 86400);

    const QString noun = account_count == 1 ? QStringLiteral("account") : QStringLiteral("accounts");
    return QStringLiteral("%1 %2 synced %3").arg(account_count).arg(noun, when);
}

} // namespace openmarketterminal::portfolio
