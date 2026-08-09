#pragma once

#include "services/prediction/kalshi/KalshiBotRuntime.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>

#include <algorithm>
#include <cmath>

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
///   4. **The signal's trust is re-read live every call, and an untrusted
///      signal does not bid at all** (issue #165). `signal_trusted()` reads the
///      report's own `adds_value_over_market`, which the calibrator only sets
///      true once its per-contract Brier beats the RAW MARKET MID over its
///      ≥100-CONTRACT gate (`spot_calibrator.MIN_SCORED_CONTRACTS`, issue
///      #171), AND requires the track record that claim is
///      made of to actually be present, AND requires that same comparison to
///      hold on the BET-ELIGIBLE subset — the contracts whose model-vs-mid
///      edge reached `edge_threshold`, which is the population a bid is
///      actually drawn from. The full-population flag alone was measured
///      where the bot does not bet. When the signal fails that rule the
///      tick is a journaled PASS with `reason_code=SIGNAL_UNTRUSTED` — no
///      order, paper or otherwise. Rung 1 papered those bids and labelled them;
///      24 hours of rung 6 showed what that buys (42% of bids placed on a
///      self-reportedly edgeless signal, and 11 of 27 fills from them), so the
///      operator superseded that clause. Historical rows stand: the record is
///      the record, and discipline starts at the next tick.
///
/// Runway is computed as of *now*, not as of the report: the report's
/// `sqrt_minutes_left` feature was measured at `generated_at_ms`, so the
/// elapsed time since generation is subtracted before the runway gate runs.
///
/// **Two-tier quoting (issue #158).** A bid that only ever rests at the mid is
/// an opinion nobody paid for: rung 6's first 24 hours rested 151 quotes,
/// canceled 147 and filled 4, because a resting bid at floor(mid) fills only
/// when the market comes DOWN through it. So when the edge is big enough to
/// PAY for a fill, the bot crosses: it quotes at the side's real ask instead,
/// which is marketable by construction. The hurdle is net of everything the
/// cross costs —
///
///     cross  ⇔  side_edge > spread_cost + taker_fee + cross_margin_usd
///           ⇔  side_p − cross_price − taker_fee > cross_margin_usd
///
/// — the same inequality written two ways, and both are journaled so the
/// arithmetic can be checked from the ledger alone. Anything below the hurdle
/// rests at floor(mid) exactly as before.
///
/// The tier decision **fails closed to resting** in both directions data can
/// fail. A report that carries no ask for the side being bid rests
/// (`REST_NO_BOOK`) — an unknown spread is not a free one. So does a report
/// whose ask contradicts the mid it is supposed to be half of, or that rounds
/// to a full dollar (`REST_BOOK_INCONSISTENT`): a negative spread cost would
/// make crossing look cheaper than resting, which is a fabricated free lunch
/// rather than a strategy. Until the calibrator that writes the report carries
/// book data at all, EVERY contract takes that path and the bot quotes exactly
/// as it did before.
///
/// **The resting tier pays an adverse-selection premium (issue #165).** The two
/// tiers do not face the same hurdle, because they do not face the same
/// counterparty. A crossing quote is filled by whoever is already offering; a
/// RESTING quote is filled only when the market comes to it, which is
/// disproportionately when the market has moved against it. So a rest must
/// demand MORE modelled edge than a cross, not the same:
///
///     rest   ⇔  |edge| ≥ edge_threshold + rest_premium_usd
///     cross  ⇔  side_edge > spread_cost + taker_fee + cross_margin_usd
///
/// — two independent hurdles, and the asymmetry is the point: a contract whose
/// edge clears the crossing arithmetic but not the resting premium CROSSES
/// rather than passing, and one that clears neither passes with
/// `REST_EDGE_BELOW_PREMIUM`. The crossing hurdle is untouched by this. Both
/// tiers journal the full arithmetic they were judged against.
///
/// The paper fill model is untouched by this (KalshiBotOrders): a crossing bid
/// is still filled only against an observed mid at or through its limit, and
/// still fills AT its limit, never at the mid — it pays what it offered to pay.
/// Bid rows state that model BY NAME (`fill_model`) so no row's fill can be
/// read as measured. Only the name: `KalshiBotOrders::kFillRule`, the prose
/// beside it, describes the passive tier in terms a crossing bid falsifies, and
/// a false disclosure is worse than none.
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
    /// Legacy journal code: lifetime gate FAIL used to pause paper. Paper now
    /// pauses only on *current-generation* drawdown and auto-rotates; live
    /// admission still requires a full-record PASS via KalshiBotLive::permit.
    static constexpr auto kGateFail = "GATE_FAIL";
    /// Paper-only: current generation max_drawdown_usd exceeds the sealed cap.
    static constexpr auto kDrawdownCap = "DRAWDOWN_CAP";
    /// Paper ledger live file archived to the next KeepAllGenerations slot so
    /// a new generation can bid without deleting the sealed history.
    static constexpr auto kPaperGenerationRotated = "PAPER_GENERATION_ROTATED";
    static constexpr auto kQuoteResting = "QUOTE_RESTING";
    static constexpr auto kExposureCapBlocksBid = "EXPOSURE_CAP_BLOCKS_BID";
    static constexpr auto kSessionBudgetBlocksBid = "SESSION_BUDGET_BLOCKS_BID";
    static constexpr auto kRequoted = "REQUOTED";
    /// Paper cashout (sell-to-close before settlement). Fail-closed reasons.
    static constexpr auto kCashoutNoBid = "CASHOUT_NO_BID";
    static constexpr auto kCashoutStaleReport = "CASHOUT_STALE_REPORT";
    static constexpr auto kLockWin = "LOCK_WIN";
    static constexpr auto kCutEdgeReversed = "CUT_EDGE_REVERSED";
    static constexpr auto kHoldEdgeIntact = "HOLD_EDGE_INTACT";
    /// Postmortem lesson: refuse NO fades of already-high YES near close.
    static constexpr auto kFadeYesNearClose = "FADE_YES_NEAR_CLOSE";

    /// Risk scored over one paper generation's settlement rows (the live
    /// jsonl only — not archived `.1`, `.2`, …). Same drawdown math as the
    /// sealed gate, so a rotated generation cannot silently change the ruler.
    struct PaperGenerationRisk {
        int settled_bids = 0;
        double net_pnl_usd = 0.0;
        double max_drawdown_usd = 0.0;
    };
    static PaperGenerationRisk score_paper_generation(const QJsonArray& settlement_rows) {
        PaperGenerationRisk out;
        QSet<QString> seen;
        double running = 0.0;
        double peak = 0.0;
        for (const auto& value : settlement_rows) {
            const QJsonObject row = value.toObject();
            if (row.value(QStringLiteral("event")).toString() !=
                QLatin1String("kalshi_bot_paper_settlement"))
                continue;
            const QString id = row.value(QStringLiteral("position_id")).toString();
            const QJsonValue pnl_v = row.value(QStringLiteral("realized_pnl"));
            if (id.isEmpty() || !pnl_v.isDouble() || seen.contains(id)) continue;
            seen.insert(id);
            const double pnl = pnl_v.toDouble();
            out.net_pnl_usd += pnl;
            running += pnl;
            peak = std::max(peak, running);
            out.max_drawdown_usd = std::max(out.max_drawdown_usd, peak - running);
            ++out.settled_bids;
        }
        out.net_pnl_usd = std::round(out.net_pnl_usd * 100.0) / 100.0;
        out.max_drawdown_usd = std::round(out.max_drawdown_usd * 100.0) / 100.0;
        return out;
    }

    /// Whether the paper loop must refuse new bids / rotate the live
    /// generation. Uses **current-generation** drawdown vs the sealed cap —
    /// never the lifetime gate FAIL — so a dead book can auto-rotate and keep
    /// learning while live promotion stays fail-closed on the full record.
    struct PaperBidPause {
        bool paused = false;
        bool should_rotate = false;
        QString reason_code;  ///< kDrawdownCap
        QString detail;
    };
    static PaperBidPause paper_bid_pause(const QJsonObject& gate,
                                         const PaperGenerationRisk& current_gen) {
        PaperBidPause out;
        const QJsonObject params = gate.value(QStringLiteral("params")).toObject();
        const QJsonValue cap = params.value(QStringLiteral("max_drawdown_usd"));
        if (!cap.isDouble()) return out;
        if (current_gen.max_drawdown_usd > cap.toDouble()) {
            out.paused = true;
            out.should_rotate = true;
            out.reason_code = QString::fromLatin1(kDrawdownCap);
            out.detail =
                QStringLiteral("current paper generation drawdown $%1 exceeds sealed cap $%2 "
                               "— rotating generation and continuing paper "
                               "(live still requires full-record PASS)")
                    .arg(current_gen.max_drawdown_usd, 0, 'f', 2)
                    .arg(cap.toDouble(), 0, 'f', 2);
        }
        return out;
    }

    /// Which tier priced a quote, journaled on every row that reached pricing.
    /// A separate field from `reason_code` on purpose: the reason a bid exists
    /// (its edge cleared the threshold) and the way it is quoted are different
    /// questions, and collapsing them would rewrite the reason codes the gate
    /// and the panel already read.
    static constexpr auto kQuoteCross = "cross";
    static constexpr auto kQuoteRest = "rest";

    /// Why that tier and not the other — the audit trail for `quote_style`.
    static constexpr auto kCrossEdgeClearsCost = "CROSS_EDGE_CLEARS_COST";
    static constexpr auto kRestEdgeBelowCost = "REST_EDGE_BELOW_COST";
    static constexpr auto kRestNoBook = "REST_NO_BOOK";
    static constexpr auto kRestBookInconsistent = "REST_BOOK_INCONSISTENT";

    /// A resting quote whose edge cleared the base threshold but not the
    /// adverse-selection premium the resting tier adds to it (#165). A
    /// `reason_code`, not a `quote_style_reason`: the tier was chosen and then
    /// the tier's own hurdle refused the bid, which is a different fact from
    /// EDGE_BELOW_THRESHOLD (that one never reached pricing at all).
    static constexpr auto kRestEdgeBelowPremium = "REST_EDGE_BELOW_PREMIUM";

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
        /// Net expected value per contract, in dollars, that must REMAIN after
        /// paying the spread AND the conservative taker fee before the bot will
        /// cross the spread instead of resting at the mid. Equivalently — and
        /// the row journals both forms — the edge must clear
        /// `spread_cost + fee + margin`. Conservative by default: two cents of
        /// modelled edge per contract have to survive the round trip before the
        /// bot pays for a fill, which is a wide hurdle against a signal whose
        /// Brier only just beats the market baseline.
        double cross_margin_usd = 0.02;
        /// Extra edge, in dollars per contract (probability units — a Kalshi
        /// contract pays $1), that a RESTING quote must demand on top of
        /// `edge_threshold` before it is placed. A resting fill is adversely
        /// selected by construction: it arrives when the market came to the
        /// quote, i.e. disproportionately when the market moved against it, so
        /// the same edge is worth less resting than crossing. Three cents
        /// against a ten-cent base threshold — the operator's conservative
        /// default (#165), chosen to sit above the one-cent tick the mid is
        /// floored to and below the crossing tier's own hurdle, so the rest
        /// tier tightens without the cross tier moving at all.
        double rest_premium_usd = 0.03;
        /// Ceiling on the all-in a single bot run may newly commit. A bounded
        /// run is the charter's first carve-out condition; this is the money
        /// half of that bound. Defaults to the same ceiling, so it constrains
        /// only once a session tightens it.
        double session_budget_usd = 120.00;
        /// The session budget (issue #125) is a LIVE bounded-run safety: an
        /// armed run may commit at most this all-in before a human re-arms,
        /// and stop/resume does not reset it. The perpetual paper loop has no
        /// arming boundary, so enforcing a lifetime cap there is a deadlock —
        /// it bricks accumulation once cumulative paper all-in reaches the cap,
        /// long before the 300-settled gate. The paper loop (run_tick) sets
        /// this false; live (run_live_tick) leaves it true.
        bool enforce_session_budget = true;
        /// Paper sell/cashout: when true, filled positions may sell-to-close at
        /// the observed held-side bid (LOCK_WIN / CUT_EDGE_REVERSED) instead of
        /// always riding to settlement. Hold remains the path when the exit
        /// evaluator says HOLD or when bid/fair is missing (fail closed).
        /// Live ignores this flag. Default ON for paper so LaunchAgent uses it;
        /// CLI `--no-paper-cashout` turns it off.
        bool enable_paper_cashout = true;

        /// Postmortem lesson — ban NO fades of high YES near close.
        /// When bidding NO (model below YES mid) and YES mid ≥ this and runway
        /// ≤ `ban_no_fade_max_runway_sec`, pass with FADE_YES_NEAR_CLOSE unless
        /// outside-info confirms the NO thesis: venue lead down, or BRTI
        /// avg60 below open (with p_brti_avg60 < 0.5 when present). Missing
        /// confirm fields → ban stands (fail closed). Mid 0 disables.
        double ban_no_fade_yes_mid = 0.85;
        int ban_no_fade_max_runway_sec = 600;  // 10 minutes

        /// Postmortem lesson — favourite asymmetry on crosses.
        /// When the cross ask is ≥ `favourite_cross_price`, require
        /// `cross_margin_usd + favourite_cross_extra_margin_usd` of net EV
        /// before crossing (else rest). Cheapens the path that paid −$1.79
        /// average losses at ~50% win rate. Extra 0 disables the surcharge.
        double favourite_cross_price = 0.65;
        double favourite_cross_extra_margin_usd = 0.03;
    };

    /// Per-ticker streak for CUT_EDGE_REVERSED hysteresis across ticks.
    using CashoutStreak = QHash<QString, int>;
    /// Per-position YES-mid samples from open fill → close (gamma honesty).
    using MidPathStore = QHash<QString, QJsonArray>;

    /// True when kxbtc15m-style features confirm a NO fade (venue lead down,
    /// or BRTI avg60 below open). Absent keys → false (no lift).
    static bool outside_info_confirms_no_fade(const QJsonObject& prediction);
    /// Journal reason for a lift, or empty when the ban stands.
    static QString outside_info_no_fade_lift_reason(const QJsonObject& prediction);

    /// Append one mid sample per open position (capped). Pure aside from store.
    static void sample_mid_path(const QJsonArray& open_positions,
                                const QJsonObject& predictions,
                                qint64 now_ms,
                                MidPathStore* store,
                                int max_samples = 32);

    /// Move stored path onto a settlement/cashout row and clear the slot.
    static void attach_mid_path(QJsonObject& settlement_or_close, MidPathStore* store);

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

    /// The one definition of "the signal may be traded", so `decide()` and
    /// every readout that reports trust cannot drift apart: a tick that passed
    /// every contract SIGNAL_UNTRUSTED must never be printed as TRUSTED.
    ///
    /// True only when the calibrator claims to add value over the market AND
    /// the track record that claim is made of is actually in the report. The
    /// claim's own sample floor is the calibrator's (`build_report` requires
    /// `scored_contracts >= MIN_SCORED_CONTRACTS` and
    /// `brier_full < brier_market_mid_raw`), and is deliberately not restated
    /// here — one floor, in the process that measures it. The presence check
    /// names the raw mid rather than the trained one-feature logit on purpose:
    /// the logit is a handicapped baseline, so a report carrying only it is
    /// not carrying the record its claim is made of (issue #171).
    /// What IS restated is presence: this reads a file another
    /// process rewrites every cycle, and a report asserting value while
    /// carrying no Brier at all is contradicting itself. Unmeasured is not
    /// trusted, the same way an unknown spread is not a free one.
    ///
    /// `adds_value_over_market` is measured over EVERY resolved contract, a
    /// population dominated by far-from-strike contracts where both the model
    /// and the market are nearly always right. The bot does not bet that
    /// population -- it bets where its edge over the mid clears
    /// `edge_threshold`, and there the market has been winning. So trust
    /// additionally requires the model to beat the mid on the BET-ELIGIBLE
    /// subset. A report predating that field cannot confer trust.
    ///
    /// Inline for the same reason `is_kxbtc15m_ticker` is: one scorer, many
    /// readers (KalshiBotCommands.cpp), so the BOT cockpit presenter and the
    /// scoreboard CLI can share the exact same promotion rule as `kalshi bot`
    /// without linking this .cpp into header-only tests.
    static bool signal_trusted(const QJsonObject& report) {
        return report.value(QStringLiteral("adds_value_over_market")).toBool() &&
               report.value(QStringLiteral("brier_full")).isDouble() &&
               report.value(QStringLiteral("brier_market_mid_raw")).isDouble() &&
               report.value(QStringLiteral("adds_value_on_bet_eligible")).toBool() &&
               report.value(QStringLiteral("brier_eligible_full")).isDouble() &&
               report.value(QStringLiteral("brier_eligible_market_mid_raw")).isDouble();
    }

    /// KXBTC15M family — ticker prefix before the first '-'. The directional
    /// 15-minute race uses its own calibrator report; threshold books do not.
    /// Inline so the BOT cockpit presenter (header-only tests) shares the
    /// exact same family split as `kalshi bot` without linking this .cpp.
    static bool is_kxbtc15m_ticker(const QString& ticker) {
        const int dash = ticker.indexOf(QLatin1Char('-'));
        const QString family = dash < 0 ? ticker : ticker.left(dash);
        return family == QLatin1String("KXBTC15M");
    }

    /// Commodities 15m directional races (own calibrator report).
    static bool is_commodity_15m_ticker(const QString& ticker) {
        const int dash = ticker.indexOf(QLatin1Char('-'));
        const QString family = dash < 0 ? ticker : ticker.left(dash);
        return family == QLatin1String("KXGOLD15M") || family == QLatin1String("KXSILVER15M") ||
               family == QLatin1String("KXWTI15M");
    }

    /// Copy of `report` keeping only predictions that match (or exclude) the
    /// KXBTC15M family. Commodity-15m tickers are never kept on either side of
    /// this bool — they have their own `filter_commodity_15m_predictions`.
    /// Track-record / trust fields are left intact so `signal_trusted()` still
    /// answers for that source report.
    static QJsonObject filter_predictions_for_family(const QJsonObject& report,
                                                     bool keep_kxbtc15m) {
        QJsonObject out = report;
        const QJsonObject predictions = report.value(QStringLiteral("predictions")).toObject();
        QJsonObject filtered;
        for (auto it = predictions.constBegin(); it != predictions.constEnd(); ++it) {
            if (is_commodity_15m_ticker(it.key())) continue;
            if (is_kxbtc15m_ticker(it.key()) == keep_kxbtc15m)
                filtered.insert(it.key(), it.value());
        }
        out.insert(QStringLiteral("predictions"), filtered);
        return out;
    }

    /// Keep only KXGOLD15M / KXSILVER15M / KXWTI15M predictions.
    static QJsonObject filter_commodity_15m_predictions(const QJsonObject& report) {
        QJsonObject out = report;
        const QJsonObject predictions = report.value(QStringLiteral("predictions")).toObject();
        QJsonObject filtered;
        for (auto it = predictions.constBegin(); it != predictions.constEnd(); ++it) {
            if (is_commodity_15m_ticker(it.key())) filtered.insert(it.key(), it.value());
        }
        out.insert(QStringLiteral("predictions"), filtered);
        return out;
    }

    /// Predictions from the threshold report (non-directional) plus the BTC
    /// and commodities 15m reports. Each source contributes only when fresh
    /// under `config.max_report_age_ms`. Trust never crosses family boundaries.
    /// `generated_at_ms` is the newest contributing source; empty when none.
    static QJsonObject merge_family_reports(const QJsonObject& threshold_report,
                                            const QJsonObject& kxbtc15m_report,
                                            qint64 now_ms,
                                            const Config& config,
                                            const QJsonObject& commodities_15m_report = {});

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
    ///
    /// Each settlement copies the originating bid's decision fields (edge,
    /// mid, runway, quote style, …) onto the row so postmortems do not depend
    /// on a separate ledger join. When `market_mid_by_ticker` carries the
    /// YES mid observed at settle time for that ticker, it is persisted as
    /// `market_mid_at_settle` — absent when unknown (never invented).
    static QJsonArray settle_paper(const QJsonArray& open_positions,
                                   const QJsonArray& settlements,
                                   qint64 now_ms,
                                   const QHash<QString, double>& market_mid_by_ticker = {});

    /// Paper sell/cashout pass: for each filled unsettled position, evaluate
    /// LOCK_WIN / CUT_EDGE_REVERSED via KalshiAutoEngine::evaluate_position_exit
    /// and, when selling, journal a `sell` decision row plus an early-exit
    /// `kalshi_bot_paper_settlement` close. Cross-only at floor(held-side bid);
    /// mid is never used as a sell proxy. Updates `cut_streak` for CUT hysteresis.
    /// Returns journal-ready rows only (empty when feature off / nothing to do).
    static QJsonArray paper_cashout(const QJsonArray& open_positions,
                                    const QJsonObject& report,
                                    qint64 now_ms,
                                    const Config& config,
                                    CashoutStreak* cut_streak);
};

} // namespace openmarketterminal::services::prediction::kalshi_ns
