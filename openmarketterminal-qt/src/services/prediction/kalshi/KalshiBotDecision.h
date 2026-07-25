#pragma once

#include "services/prediction/kalshi/KalshiBotRuntime.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace openmarketterminal::services::prediction::kalshi_ns {

/// Pure decision math for the PAPER Kalshi bot (ladder rung 1).
///
/// Turns the spot calibrator's report (calibrator.json: per-contract
/// calibrated P(YES) plus the calibrator's own Brier track record) into bids.
/// Nothing here reaches an exchange: no method prepares, submits, amends, or
/// cancels an order. Ladder rung 5 can hand a bid row from here to the
/// terminal's existing submit path, but the sizing and pricing below are the
/// paper math either way — this class never learns what mode it is in, and
/// gains no authority from one.
///
/// Three honesty rules are structural, not stylistic:
///   1. **Every decision is journaled, including passes.** `decide()` returns
///      one row per contract the report offers, so a tick that bids nothing
///      still says why it bid nothing. A refused report (missing or stale)
///      returns one refusal row rather than an empty array.
///   2. **Withheld numbers are absent, never zero.** A missing or stale report
///      carries no probability, edge, or price — the caller cannot mistake a
///      default-constructed 0.0 for a measurement.
///   3. **The kill switch is checked before anything else** (rung 4). There is
///      exactly one path from this class to a bid, and an engaged stop file
///      short-circuits it at the top of `decide()`, so "checked every tick
///      before any bid" is a structural property rather than a caller's
///      promise. The refused tick still journals one `BOT_STOPPED` row: a
///      silent stop would be indistinguishable from a dead loop.
///   4. **The signal's trust is re-read live every call.** `signal_trusted`
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
    static constexpr auto kBotStopped = "BOT_STOPPED";
    static constexpr auto kQuoteResting = "QUOTE_RESTING";
    static constexpr auto kExposureCapBlocksBid = "EXPOSURE_CAP_BLOCKS_BID";
    static constexpr auto kSessionBudgetBlocksBid = "SESSION_BUDGET_BLOCKS_BID";
    static constexpr auto kRequoted = "REQUOTED";

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
        /// How long a quote may rest before it is pulled (ladder rung 6).
        /// Three default ticks, and well inside the fifteen-minute contracts
        /// the calibrator quotes: a quote older than this is priced off a
        /// market that has moved on.
        int quote_ttl_seconds = 180;
        /// Ceiling on total exposure outstanding — resting remainders at limit
        /// price plus filled-but-unsettled positions (the PR #44 rule). It is
        /// the ceiling `kalshi auto live arm` refuses to exceed for the live
        /// experiment (CommandDispatch.cpp's `tightened(..., 120.0)`), reused
        /// rather than invented so paper is measured under the live fence.
        double max_open_exposure_usd = 120.00;
        /// Ceiling on the all-in a single bot run may newly commit. A bounded
        /// run is the charter's first carve-out condition; this is the money
        /// half of that bound. Defaults to the same ceiling, so it constrains
        /// only once a session tightens it.
        double session_budget_usd = 120.00;
    };

    /// What the bot already has at risk when `decide()` is called, and what
    /// this tick's lifecycle pass freed up. Supplied by the caller from
    /// `KalshiBotOrders::replay()` so the decision math stays pure.
    struct Exposure {
        /// The PR #44 rule over the whole book, in dollars.
        double at_risk_usd = 0.0;
        /// All-in this run has already committed, against `session_budget_usd`.
        double session_opened_usd = 0.0;
        /// Orders still working. A ticker with one is not quoted again: the
        /// bot replaces quotes, it never stacks them.
        QJsonArray resting;
        /// `{ticker: position_id}` freed by a TTL cancel on THIS tick, from
        /// `KalshiBotOrders::requotable()`. A bid on one of these is the
        /// replace half of cancel/replace and is journaled REQUOTED.
        QJsonObject requoted;
    };

    /// One journal-ready row per decision, in the report's contract order.
    ///
    /// `open_positions` are the bot's filled, unsettled positions (from
    /// `KalshiBotOrders::replay`); a contract already held passes with
    /// `ALREADY_HELD` so a `run` loop never re-bids the same contract every
    /// tick, and a contract with a quote still working passes with
    /// `QUOTE_RESTING`. `settled_positions` are its closed ones: a contract the exchange
    /// has already resolved passes with `CONTRACT_SETTLED` even if the report
    /// still advertises runway for it, which happens whenever the daemon
    /// snapshot the calibrator reads goes stale under a fresh report.
    /// Rows carry `event`, `ts_ms`, `mode:"paper"`, `live_eligible:false`,
    /// `ticker`, `action` ("bid"|"pass"), `reason_code`, `signal_trusted`, and
    /// a `track_record` snapshot of the report that produced them. Bid rows
    /// additionally carry side, price, contracts, stake, fee, and — since
    /// ladder rung 6 — the order's opening lifecycle state: `order_state`
    /// ("resting"), its TTL, and the fill model that will decide whether it
    /// ever becomes a position. A bid is an ORDER, not a position; see
    /// KalshiBotOrders.
    ///
    /// `stop` is the kill switch as read from disk this tick. When it is
    /// engaged the report is not even read: the call returns a single
    /// `BOT_STOPPED` refusal row carrying who threw the switch and when. It is
    /// checked before the exposure math, before the report, before anything —
    /// this is the single path from this class to a bid.
    ///
    /// `exposure` is the book as it stands after this tick's lifecycle pass.
    /// A bid is refused outright — never sized down — when it would push
    /// outstanding exposure past `max_open_exposure_usd` or this run's
    /// committed all-in past `session_budget_usd`, and the running totals
    /// include the bids this same call has already made.
    static QJsonArray decide(const QJsonObject& report,
                             const QJsonArray& open_positions,
                             const QJsonArray& settled_positions,
                             qint64 now_ms,
                             const Config& config,
                             const KalshiBotStopFile& stop,
                             const Exposure& exposure);

    /// The same decision against an empty book: nothing resting, nothing at
    /// risk, no requote. (A defaulted argument cannot be used for `exposure` —
    /// the nested Exposure's member initializers are not yet parsed at this
    /// point in the class.) There is deliberately NO overload that takes an
    /// Exposure without a stop: the kill switch must not be bypassable by
    /// argument shape.
    static QJsonArray decide(const QJsonObject& report,
                             const QJsonArray& open_positions,
                             const QJsonArray& settled_positions,
                             qint64 now_ms,
                             const Config& config,
                             const KalshiBotStopFile& stop = {});

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
    ///
    /// `open_positions` are `KalshiBotOrders::replay()`'s position rows, whose
    /// contracts/stake/fee are the FILLED quantities — an order that only ever
    /// rested is not among them and settles into nothing, which is the honest
    /// outcome of a quote the market never took.
    static QJsonArray settle_paper(const QJsonArray& open_positions,
                                   const QJsonArray& settlements,
                                   qint64 now_ms);
};

} // namespace openmarketterminal::services::prediction::kalshi_ns
