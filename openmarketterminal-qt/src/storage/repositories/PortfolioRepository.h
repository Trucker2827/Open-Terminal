// src/storage/repositories/PortfolioRepository.h
#pragma once
#include "screens/portfolio/PortfolioTypes.h"
#include "storage/repositories/BaseRepository.h"

namespace openmarketterminal {

class PortfolioRepository : public BaseRepository<portfolio::Portfolio> {
  public:
    static PortfolioRepository& instance();

    // ── Portfolios CRUD ──────────────────────────────────────────────────────
    Result<QVector<portfolio::Portfolio>> list_portfolios();
    Result<portfolio::Portfolio> get_portfolio(const QString& id);
    /// `broker_account_id` ties this portfolio to a broker account so live
    /// quotes can be sourced from the broker instead of yfinance. Empty for
    /// manually-created / JSON-imported portfolios. See migration v022.
    Result<QString> create_portfolio(const QString& name, const QString& owner, const QString& currency,
                                     const QString& description = {},
                                     const QString& broker_account_id = {});
    Result<void> update_portfolio(const QString& id, const QString& name, const QString& owner, const QString& currency,
                                  const QString& description = {});
    Result<void> delete_portfolio(const QString& id);

    // ── Account sync (v060) ──────────────────────────────────────────────────
    /// Stamps a portfolio's sync bookkeeping columns. Called by
    /// AccountSyncService::ensure_portfolio (sync_source only, on first
    /// creation) and after every sync_account attempt (synced_at + sync_error
    /// on success, sync_error only — synced_at unchanged — on failure).
    Result<void> set_sync_meta(const QString& portfolio_id, const QString& sync_source, const QString& synced_at,
                               const QString& sync_error);
    /// Finds the portfolio mirroring a given connected account, if any.
    /// sync_source == '' never matches (manual portfolios are never synced).
    std::optional<portfolio::Portfolio> find_by_sync_source(const QString& sync_source);
    /// All portfolios with a non-empty sync_source, for Task 8 (sync-all UI /
    /// scheduler) to enumerate.
    QVector<portfolio::Portfolio> list_synced();

    // ── Assets CRUD ──────────────────────────────────────────────────────────
    Result<QVector<portfolio::PortfolioAsset>> get_assets(const QString& portfolio_id);
    /// `broker_symbol` + `exchange` are the broker-native pair (e.g.
    /// symbol + exchange); both empty for manual / JSON imports. The
    /// canonical `symbol` arg stays in yfinance-format ("AAPL")
    /// regardless — every downstream consumer treats it as such.
    Result<qint64> add_asset(const QString& portfolio_id, const QString& symbol, double qty, double price,
                             const QString& date = {}, const QString& sector = {},
                             const QString& broker_symbol = {}, const QString& exchange = {},
                             bool has_cost_basis = true);
    /// `has_cost_basis` defaults to true (manual-edit call sites keep the
    /// asset's existing value by reading it and passing it back explicitly —
    /// see CommandDispatch/PortfolioService callers). Added so
    /// AccountSyncService's mirror-write can converge a has_cost_basis-only
    /// diff (reconcile_mirror's update predicate includes it).
    Result<void> update_asset(const QString& portfolio_id, const QString& symbol, double qty, double avg_price,
                              bool has_cost_basis = true);
    Result<void> set_asset_sector(const QString& portfolio_id, const QString& symbol, const QString& sector);
    Result<void> remove_asset(const QString& portfolio_id, const QString& symbol);

    // ── Transactions ─────────────────────────────────────────────────────────
    Result<QVector<portfolio::Transaction>> get_transactions(const QString& portfolio_id, int limit = 50);
    Result<QVector<portfolio::Transaction>> get_symbol_transactions(const QString& portfolio_id, const QString& symbol);
    Result<QString> add_transaction(const QString& portfolio_id, const QString& symbol, const QString& type, double qty,
                                    double price, const QString& date, const QString& notes = {});
    /// Dedup insert for trade-history sync (v061): INSERT OR IGNORE keyed on
    /// idx_ptx_external (portfolio_id, external_id) — a repeat external_id
    /// (re-syncing the same account) is a silent no-op, so re-sync never
    /// duplicates. `external_id` MUST be non-empty (e.g. "broker:<order_id>",
    /// "<exchange_id>:<trade_id>") — an empty external_id falls outside the
    /// partial unique index and would insert unconstrained, like
    /// add_transaction. Returns the generated id even when the row was
    /// ignored (the caller only needs know-it-succeeded, not which case).
    Result<QString> import_transaction(const QString& portfolio_id, const QString& symbol, const QString& type,
                                       double qty, double price, const QString& date, const QString& external_id,
                                       const QString& notes = {});
    Result<void> update_transaction(const QString& id, double qty, double price, const QString& date,
                                    const QString& notes = {});
    Result<void> delete_transaction(const QString& id);

    // ── Snapshots ────────────────────────────────────────────────────────────
    Result<void> save_snapshot(const QString& portfolio_id, double value, double cost_basis, double pnl, double pnl_pct,
                               const QString& date);
    Result<QVector<portfolio::PortfolioSnapshot>> get_snapshots(const QString& portfolio_id, int days = 365);

  private:
    PortfolioRepository() = default;

    static portfolio::Portfolio map_portfolio(QSqlQuery& q);
    static portfolio::PortfolioAsset map_asset(QSqlQuery& q);
    static portfolio::Transaction map_transaction(QSqlQuery& q);
    static portfolio::PortfolioSnapshot map_snapshot(QSqlQuery& q);
};

} // namespace openmarketterminal
