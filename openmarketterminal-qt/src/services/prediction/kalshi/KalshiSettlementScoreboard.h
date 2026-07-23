#pragma once

#include <QJsonArray>
#include <QString>

namespace openmarketterminal::services::prediction::kalshi_ns {

/// Difficulty-cohorted scoreboard math for the live-account closed-bets pane.
///
/// Consumes the `settlements_ready` payload (the same record shape mirrored to
/// kalshi-account-settlements.jsonl: realized_pnl, accounting_status, stake,
/// yes_count, no_count, settled_time, side, market_result). Only
/// accounting_status == "exact_one_sided" rows are scored: their average entry
/// price (stake ÷ contracts) is an honest implied-probability-paid proxy, and
/// their realized P/L is exact. Netted mixed-turnover rows and rows missing
/// the required fields are never approximated into cohorts — they are counted
/// as excluded so the reader always sees what the scoreboard omits.
class KalshiSettlementScoreboard {
  public:
    /// One entry-price cohort. `pushes` are settlements whose exact P/L is
    /// zero: they count toward the sample but neither wins nor losses.
    struct Cohort {
        int wins = 0;
        int losses = 0;
        int pushes = 0;
        double pnl = 0.0;
        int sample() const { return wins + losses + pushes; }
        int decided() const { return wins + losses; }
        /// Wins over decided settlements; -1.0 when nothing is decided yet.
        double hit_rate() const {
            return decided() > 0 ? static_cast<double>(wins) / decided() : -1.0;
        }
    };

    struct Scoreboard {
        Cohort longshot;   ///< avg entry ≤ 35c
        Cohort coinflip;   ///< 35c < avg entry < 65c
        Cohort favorite;   ///< avg entry ≥ 65c
        /// accounting_status == "exact_netted_mixed": both YES and NO traded,
        /// so no single entry price exists — excluded, never cohorted.
        int excluded_mixed = 0;
        /// Rows without exact accounting fields (legacy/raw records, missing
        /// realized_pnl, or zero contracts) — excluded, never guessed.
        int excluded_incomplete = 0;
        /// Signed trailing run over decided rows ordered by settled_time:
        /// +n = n consecutive wins ending at the latest settlement, -n = losses,
        /// 0 = no decided settlements.
        int current_streak = 0;
        int best_win_streak = 0;
        int best_loss_streak = 0;
        int scored() const {
            return longshot.sample() + coinflip.sample() + favorite.sample();
        }
        int excluded() const { return excluded_mixed + excluded_incomplete; }
    };

    /// Which cohort an average entry price (dollars per contract) lands in:
    /// "longshot" (≤0.35), "coinflip" (<0.65), or "favorite" (≥0.65).
    static QString cohort_for_price(double average_entry_price);

    static Scoreboard compute(const QJsonArray& settlements);

    /// Multi-line scoreboard block for the closed-bets pane. States every
    /// cohort's sample count, labels exclusions, and reads "missing" (never a
    /// fabricated 0%) when nothing has settled with exact accounting.
    static QString format(const Scoreboard& board);
};

} // namespace openmarketterminal::services::prediction::kalshi_ns
