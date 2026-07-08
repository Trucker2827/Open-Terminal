// src/services/portfolio/AccountSyncService.cpp
#include "services/portfolio/AccountSyncService.h"

#include "core/logging/Logger.h"
#include "storage/repositories/PortfolioRepository.h"

#include <QDateTime>

namespace openmarketterminal::services {

AccountSyncService& AccountSyncService::instance() {
    static AccountSyncService s;
    return s;
}

AccountSyncService::AccountSyncService() : QObject(nullptr) {}

void AccountSyncService::register_source(IAccountSource* src) {
    if (src && !sources_.contains(src))
        sources_.append(src);
}

void AccountSyncService::sync_all() {
    // Re-entrancy guard: the live broker fetch path runs a nested
    // QEventLoop::exec() while waiting on the network, which keeps the UI
    // responsive — a click on the portfolio selector mid-sweep can re-enter
    // sync_all(). Without this guard the two sweeps interleave writes
    // against the same portfolios. A re-entrant call is a silent no-op.
    if (syncing_)
        return;
    syncing_ = true;

    emit sync_started();
    for (auto* src : sources_) {
        if (!src)
            continue;
        for (const auto& ref : src->list_accounts())
            sync_account(ref, src);
    }

    syncing_ = false;
    emit sync_finished();
}

QString AccountSyncService::ensure_portfolio(const AccountRef& ref) {
    auto& repo = PortfolioRepository::instance();

    auto existing = repo.find_by_sync_source(ref.sync_source);
    if (existing.has_value())
        return existing->id;

    // broker_account_id intentionally left empty here — routing live quotes
    // through the broker for synced portfolios is a later concern, not part
    // of this mirror-write path.
    // owner must be a non-null (empty is fine) QString — portfolios.owner is
    // NOT NULL and QSqlQuery binds a default-constructed QString() as SQL
    // NULL, not ''.
    auto created = repo.create_portfolio(ref.display_name, QStringLiteral(""), ref.base_currency,
                                         QStringLiteral("Synced from ") + ref.display_name, QString());
    if (created.is_err()) {
        LOG_ERROR("AccountSyncService",
                  QString("Failed to create synced portfolio for '%1': %2")
                      .arg(ref.sync_source, QString::fromStdString(created.error())));
        return QString();
    }

    const QString pid = created.value();
    repo.set_sync_meta(pid, ref.sync_source, QString(), QString());
    return pid;
}

void AccountSyncService::sync_account(const AccountRef& ref, IAccountSource* src) {
    if (!src) {
        emit account_synced(ref.sync_source, false, QStringLiteral("no source"));
        return;
    }

    const QString pid = ensure_portfolio(ref);
    if (pid.isEmpty()) {
        emit account_synced(ref.sync_source, false, QStringLiteral("failed to create/find synced portfolio"));
        return;
    }

    auto& repo = PortfolioRepository::instance();
    const auto res = src->fetch(ref);

    if (!res.ok) {
        // Never-wipe-on-failure: record the error, leave portfolio_assets
        // and synced_at untouched.
        const auto current = repo.find_by_sync_source(ref.sync_source);
        const QString synced_at = current.has_value() ? current->synced_at : QString();
        repo.set_sync_meta(pid, ref.sync_source, synced_at, res.error);
        emit account_synced(ref.sync_source, false, res.error);
        return;
    }

    const auto current_assets_r = repo.get_assets(pid);
    const auto current_assets =
        current_assets_r.is_ok() ? current_assets_r.value() : QVector<portfolio::PortfolioAsset>{};
    const auto plan = portfolio::reconcile_mirror(current_assets, res.holdings);

    for (const auto& h : plan.to_add)
        repo.add_asset(pid, h.canonical_symbol, h.quantity, h.avg_cost, QString(), QString(), h.broker_symbol,
                       h.exchange, h.has_cost_basis);
    for (const auto& h : plan.to_update)
        repo.update_asset(pid, h.canonical_symbol, h.quantity, h.avg_cost, h.has_cost_basis);
    for (const auto& symbol : plan.to_remove)
        repo.remove_asset(pid, symbol);

    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    repo.set_sync_meta(pid, ref.sync_source, now, QString());
    emit account_synced(ref.sync_source, true, QString());
}

} // namespace openmarketterminal::services
