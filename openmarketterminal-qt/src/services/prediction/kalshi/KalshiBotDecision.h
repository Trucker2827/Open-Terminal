#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace openmarketterminal::services::prediction::kalshi_ns {

/// Pure decision math for the PAPER Kalshi bot (ladder rung 1).
///
/// Turns the spot calibrator's report (calibrator.json: per-contract
/// calibrated P(YES) plus the calibrator's own Brier track record) into paper
/// bids. Nothing here reaches an exchange: no method prepares, submits,
/// amends, or cancels an order, and this rung has no live mode at all.
///
/// Three honesty rules are structural, not stylistic:
///   1. **Every decision is journaled, including passes.** `decide()` returns
///      one row per contract the report offers, so a tick that bids nothing
///      still says why it bid nothing. A refused report (missing or stale)
///      returns one refusal row rather than an empty array.
///   2. **Withheld numbers are absent, never zero.** A missing or stale report
///      carries no probability, edge, or price — the caller cannot mistake a
///      default-constructed 0.0 for a measurement.
///   3. **The signal's trust is re-read live every call.** `signal_trusted`
///      comes from the report's own `adds_value_over_market`, which the
///      calibrator only sets true once its Brier beats the market baseline
///      over its ≥100-sample gate. When it is false the bot still papers, but
///      every bid is journaled `reason_code=SIGNAL_UNTRUSTED`.
///
/// Runway is computed as of *now*, not as of the report: the report's
/// `sqrt_minutes_left` feature was measured at `generated_at_ms`, so the
/// elapsed time since generation is subtracted before the runway gate runs.
class KalshiBotDecision {
  public:
    /// Reason codes written to every ledger row. Stable strings — the ledger
    /// is an audit record, so these are matched by tests and downstream rungs.
    static constexpr auto kReportMissing = "REPORT_MISSING";
    static constexpr auto kReportStale = "REPORT_STALE";
    static constexpr auto kNoPredictions = "NO_PREDICTIONS";
    static constexpr auto kMalformedPrediction = "MALFORMED_PREDICTION";
    static constexpr auto kNoRunway = "NO_RUNWAY";
    static constexpr auto kEdgeBelowThreshold = "EDGE_BELOW_THRESHOLD";
    static constexpr auto kAlreadyHeld = "ALREADY_HELD";
    static constexpr auto kContractSettled = "CONTRACT_SETTLED";
    static constexpr auto kSizeCapBlocksBid = "SIZE_CAP_BLOCKS_BID";
    static constexpr auto kEdgeClearsThreshold = "EDGE_CLEARS_THRESHOLD";
    static constexpr auto kSignalUntrusted = "SIGNAL_UNTRUSTED";

    /// Paper sizing/pricing policy. Defaults are deliberately conservative and
    /// mirror the charter's live ceilings ($2 stake, $3 all-in) so rung 1's
    /// paper record is measured under the same size discipline a later live
    /// rung would have to obey. They are *paper* configuration here — this
    /// rung satisfies none of the live carve-out's other conditions.
    struct Config {
        /// Minimum |calibrated P(YES) − market mid| before a bid is placed.
        double edge_threshold = 0.10;
        /// Hard ceiling on contracts × limit price, in dollars.
        double max_stake_usd = 2.00;
        /// Hard ceiling on stake + conservative taker fee, in dollars.
        double max_all_in_usd = 3.00;
        /// A contract must still have at least this many seconds to run
        /// *right now* (report runway minus elapsed time since generation).
        int min_runway_seconds = 120;
        /// A report at least this old is refused outright. The bot reads a
        /// file another process rewrites every cycle; a stale read is a dead
        /// opinion, not a slightly old one.
        qint64 max_report_age_ms = 120'000;
    };

    /// One journal-ready row per decision, in the report's contract order.
    ///
    /// `open_positions` are the bot's unsettled paper bids (see
    /// `open_positions_from_ledger`); a contract already held passes with
    /// `ALREADY_HELD` so a `run` loop never re-bids the same contract every
    /// tick. `settled_positions` are its closed ones: a contract the exchange
    /// has already resolved passes with `CONTRACT_SETTLED` even if the report
    /// still advertises runway for it, which happens whenever the daemon
    /// snapshot the calibrator reads goes stale under a fresh report.
    /// Rows carry `event`, `ts_ms`, `mode:"paper"`, `live_eligible:false`,
    /// `ticker`, `action` ("bid"|"pass"), `reason_code`, `signal_trusted`, and
    /// a `track_record` snapshot of the report that produced them. Bid rows
    /// additionally carry side, price, contracts, stake, fee, and the paper
    /// fill assumption.
    static QJsonArray decide(const QJsonObject& report,
                             const QJsonArray& open_positions,
                             const QJsonArray& settled_positions,
                             qint64 now_ms,
                             const Config& config);

    /// The bot's unsettled paper bids, replayed from its own decision ledger:
    /// every `action=="bid"` row whose `position_id` has no settlement row.
    static QJsonArray open_positions_from_ledger(const QJsonArray& decision_rows,
                                                 const QJsonArray& settlement_rows);

    /// Flattens the terminal's two real settlement ledgers into
    /// `{ticker, market_result, settled_time, source}` rows. Rows without a
    /// ticker, or whose result is neither YES nor NO, are dropped — an
    /// unresolved market stays unresolved.
    ///
    /// `account_settlements` is kalshi-account-settlements.jsonl (market_id /
    /// market_result, exact account accounting, only markets the account
    /// traded); `settlement_labels` is kalshi-settlements.jsonl
    /// (kalshi_market_id / result, every market the terminal has watched
    /// settle). Both carry exchange truth; neither is simulated.
    static QJsonArray normalize_settlements(const QJsonArray& account_settlements,
                                            const QJsonArray& settlement_labels);

    /// Settles open paper positions against real settlement results. A
    /// position whose ticker has no settlement record produces NO row: it
    /// stays open until the exchange actually resolves it. There is no
    /// synthetic outcome, anywhere.
    static QJsonArray settle_paper(const QJsonArray& open_positions,
                                   const QJsonArray& settlements,
                                   qint64 now_ms);
};

} // namespace openmarketterminal::services::prediction::kalshi_ns
