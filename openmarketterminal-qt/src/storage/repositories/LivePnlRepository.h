#pragma once
#include "storage/repositories/BaseRepository.h"

#include <optional>

namespace openmarketterminal {

/// One opened LIVE position (v051). "One OPEN per (account,venue,instrument)" is a
/// repo invariant (get_open filters status='open'); closed rows persist as
/// history so a later close→re-open cannot collide. P&L-ONLY (NO cash side) —
/// distinct from the PM paper book's cash account. `instrument` = equity symbol
/// or PM asset_id.
struct LivePosition {
    qint64 id = 0;
    QString account;
    QString venue;
    QString instrument;
    double qty = 0;
    double avg_cost = 0;
    double cost_basis = 0;
    QString opened_at;
    QString status;
};

/// Data-access layer for the live realized-P&L ledger (live_positions + daily_pnl).
class LivePnlRepository : public BaseRepository<LivePosition> {
  public:
    static LivePnlRepository& instance();

    // ── Positions (live_positions) ───────────────────────────────────────────
    /// The single OPEN position for (account,venue,instrument), or nullopt.
    Result<std::optional<LivePosition>> get_open(const QString& account, const QString& venue,
                                                 const QString& instrument);
    /// Insert a new position row; returns its autoincrement id.
    Result<qint64> insert_open(const LivePosition& p);
    /// Update qty/avg_cost/cost_basis/status for the row with the given id.
    Result<void> set_position(qint64 id, double qty, double avg_cost, double cost_basis,
                              const QString& status);

    // ── Daily realized-P&L tally (daily_pnl) ─────────────────────────────────
    /// Realized P&L recorded for a UTC day; 0 if no row yet.
    Result<double> realized_today(const QString& utc_day);
    /// Add delta to a UTC day's realized P&L (upsert: existing += delta, else INSERT).
    Result<void> add_realized(const QString& utc_day, double delta);

  private:
    LivePnlRepository() = default;
    static LivePosition map_position(QSqlQuery& q);
};

} // namespace openmarketterminal
