#pragma once

#include "services/prediction/kalshi/KalshiBotDecision.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

namespace openmarketterminal::services::prediction::kalshi_ns {

/// Order lifecycle for the Kalshi bot (ladder rung 6): a bid is an ORDER, not
/// a position. It rests until something observable happens to it.
///
/// Rung 1 booked every bid as an instantly filled position. That is the thing
/// this rung retires: a quote left resting at a price the market has walked
/// away from is money at risk that nothing was counting. Here a bid enters the
/// book `resting` with a TTL, and each tick either fills it, cancels it
/// (TTL expired / the edge that justified it is gone / its market settled), or
/// leaves it resting — and every one of those transitions is journaled.
///
/// Three rules are structural:
///
///   1. **Resting is risk.** `exposure_usd()` is the PR #44 rule verbatim —
///      filled quantity at its fill price plus the unfilled remainder at the
///      LIMIT price, over every order the venue has not finished with. A
///      canceled order releases only its remainder; a partial fill stays at
///      risk. Fees are not exposure under that rule (the per-bid all-in
///      ceiling is what bounds them), so they are reported separately.
///
///   2. **A cancel is not done until it is confirmed.** `reconcile()` asks the
///      injected canceller to make the cancel effective and only writes a
///      terminal CANCELED_* row when it confirms. An unconfirmed cancel is
///      journaled UNCONFIRMED_CANCEL, leaves the order resting, keeps it in
///      the exposure sum, and is retried on the next tick.
///
///   3. **No fill is invented.** Paper has no order book: calibrator.json
///      carries a mid and nothing else. A resting paper order is therefore
///      filled only once an OBSERVED mid is at or through its limit, and then
///      at its own limit price, never better; fills are all-or-nothing because
///      nothing in the report could evidence a partial. That inference is
///      stated on every row it produces (`fill_model`, `fill_rule`) exactly as
///      rung 1 stated its own — it is a model, not a measurement. The
///      remainder arithmetic is nevertheless kept general so a live rung's
///      real partial fills flow through the same code.
///
/// Nothing here reaches an exchange. Cancellation is an injected effect; this
/// class only decides what should happen and what gets written down.
class KalshiBotOrders {
  public:
    /// Reason codes for lifecycle rows. Stable strings: the ledger is an audit
    /// record that later rungs and the BOT panel read back.
    static constexpr auto kFilledAtLimit = "FILLED_AT_LIMIT";
    static constexpr auto kCanceledTtl = "CANCELED_TTL";
    static constexpr auto kCanceledEdgeGone = "CANCELED_EDGE_GONE";
    static constexpr auto kCanceledSettled = "CANCELED_MARKET_SETTLED";
    static constexpr auto kUnconfirmedCancel = "UNCONFIRMED_CANCEL";

    /// Order states, spelled exactly as the live execution ledger spells them
    /// (kalshi_live_orders.state, written by `normalized_kalshi_order_state`
    /// in OrderFlowTools.cpp — note its "cancelled" carries two Ls, unlike the
    /// CANCELED_* reason codes above), so the same exposure rule reads paper
    /// and live books without a translation layer. `rejected` and `settled`
    /// are the two other terminal states that SQL excludes; paper never writes
    /// them, but the exposure rule honours them so a live book sums correctly.
    static constexpr auto kResting = "resting";
    static constexpr auto kPartiallyFilled = "partially_filled";
    static constexpr auto kFilled = "filled";
    static constexpr auto kCanceled = "cancelled";
    static constexpr auto kRejected = "rejected";
    static constexpr auto kSettled = "settled";

    /// Named on every order this rung opens and on every fill it infers, so a
    /// reader can tell a rung-1 assumed fill from a rung-6 observed-mid fill
    /// without reconstructing which build wrote the row. Rung 1's rows carry
    /// no `order_state`; those are replayed as `rung1_assumed` positions.
    static constexpr auto kFillModel = "rung6_conditional_mid";
    static constexpr auto kLegacyFillModel = "rung1_assumed";
    static constexpr auto kFillRule =
        "paper: calibrator.json carries no book, so its market mid is the ask proxy — a resting "
        "limit fills only once an observed mid is at or through the limit, and then at the limit, "
        "never better; all-or-nothing, because nothing in the report could evidence a partial. A "
        "stated model, not a measured fill: it selects on the market having moved to the quote.";

    /// The bot's order book, replayed from its own ledger.
    struct Book {
        /// Orders with an unfilled remainder the venue is still working.
        QJsonArray resting;
        /// Filled quantity not yet settled, in `settle_paper()`'s row shape
        /// (contracts/stake/fee/price are the FILLED numbers, not the ordered
        /// ones), so a partial fill settles for what it actually got.
        QJsonArray positions;
        /// Every contract the bot knows has resolved: `{ticker}` rows in the
        /// shape `KalshiBotDecision::decide()` reads for its CONTRACT_SETTLED
        /// gate. It is NOT just the settlement events — an order that only
        /// ever rested settles into no position and produces none, so the
        /// CANCELED_MARKET_SETTLED rows count too. Without them a resolved
        /// contract drops out of the book entirely and the bot quotes it
        /// again on the next tick.
        QJsonArray settled;
        /// The PR #44 exposure rule over both, in dollars.
        double exposure_usd = 0.0;
        /// The resting remainders' share of it, at limit price.
        double resting_usd = 0.0;
        /// Fees already committed by filled quantity. Not exposure under the
        /// #44 rule; reported so the number is never silently dropped.
        double fees_usd = 0.0;
    };

    /// Makes a cancel effective at the venue and reports whether the venue
    /// CONFIRMED it. Paper's canceller confirms unconditionally — the paper
    /// book is the ledger itself, so the cancel is effective exactly when its
    /// row lands, and a row that fails to land leaves the order resting next
    /// tick. A live rung injects one that talks to the exchange.
    using Canceller = std::function<bool(const QJsonObject& order, const QString& reason)>;

    /// Replays `ledger_rows` (kalshi-bot-decisions.jsonl, decision AND
    /// settlement events, in file order) into the current book.
    static Book replay(const QJsonArray& ledger_rows);

    /// The PR #44 rule for one replayed order row: filled quantity at its fill
    /// price plus the remainder at the limit price.
    static double order_exposure_usd(const QJsonObject& order);

    /// One tick of lifecycle management for everything resting, in book order.
    /// Returns the journal-ready rows (fills, confirmed cancels, unconfirmed
    /// cancels) for the caller to append; nothing is written here.
    ///
    /// `report` is the freshly re-read calibrator report — the edge that
    /// justified a quote is re-derived from it, never remembered. A report
    /// that is missing, stale, or silent about a ticker yields NO edge
    /// judgement for that order: it is left resting for the TTL to catch,
    /// because "I cannot see the edge" is not "the edge is gone".
    static QJsonArray reconcile(const Book& book,
                                const QJsonObject& report,
                                const QJsonArray& settlements,
                                qint64 now_ms,
                                const KalshiBotDecision::Config& config,
                                const Canceller& cancel);

    /// The tickers `reconcile()` freed for a fresh quote this tick, mapped to
    /// the order id that was canceled: `{ticker: position_id}`. Only TTL
    /// expiry re-quotes — an order canceled because its edge is gone or its
    /// market settled must not be replaced.
    static QJsonObject requotable(const QJsonArray& lifecycle_rows);
};

} // namespace openmarketterminal::services::prediction::kalshi_ns
