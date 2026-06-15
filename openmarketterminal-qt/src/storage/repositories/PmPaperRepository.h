#pragma once
#include "storage/repositories/BaseRepository.h"

#include <optional>

namespace openmarketterminal {

/// One opened prediction-market paper position (v050). "One OPEN long per
/// (venue,asset_id)" is a repo invariant (get_open filters status='open');
/// closed rows persist as history.
struct PmPosition {
    qint64 id = 0;
    QString venue;
    QString market_id;
    QString asset_id;
    QString outcome;
    QString category;
    double contracts = 0;
    double avg_price = 0;
    double cost_basis = 0;
    QString opened_at;
    QString status;
};

/// Data-access layer for the PM paper book (pm_paper_account + pm_paper_positions).
class PmPaperRepository : public BaseRepository<PmPosition> {
  public:
    static PmPaperRepository& instance();

    // ── Account (single id=1 cash ledger row) ────────────────────────────────
    /// Cash balance. Seeds the id=1 row with the default 100000 on first read.
    Result<double> cash();
    /// Debit/credit cash by delta (ensures the row exists first).
    Result<void> adjust_cash(double delta);

    // ── Positions ────────────────────────────────────────────────────────────
    /// The single OPEN position for (venue,asset_id), or nullopt if none open.
    Result<std::optional<PmPosition>> get_open(const QString& venue, const QString& asset_id);
    /// Insert a new position row; returns its autoincrement id.
    Result<qint64> insert_open(const PmPosition& p);
    /// Update contracts/cost_basis/status for the row with the given id.
    Result<void> set_contracts(qint64 id, double contracts, double cost_basis, const QString& status);
    /// All currently-open positions.
    Result<QVector<PmPosition>> list_open();
    /// Sum of cost_basis across all OPEN positions in a category.
    Result<double> open_stake_in_category(const QString& category);

  private:
    PmPaperRepository() = default;
    static PmPosition map_position(QSqlQuery& q);
};

} // namespace openmarketterminal
