#pragma once

// Contract rollover for the Kalshi screen's market list.
//
// A 15-minute contract dies every quarter hour. Before this module the screen
// re-fetched its market list only when the operator touched a control, so at
// every rollover the selected contract went silent — maker numbers frozen,
// charts stalled — until someone toggled the cadence buttons by hand. Two
// decisions fix that, and both live here as pure functions so the honesty and
// no-storm rules are regression-testable without a widget:
//
//   decide_market_refresh — when the screen may re-fetch the market list. One
//     request at a time (a periodic timer must never stack on an in-flight
//     fetch), with a stall escape so a reply that never arrives cannot wedge
//     the timer forever.
//   choose_market_row — which row to select once a fetch lands. A background
//     refresh must not yank the operator's selection; an expired selection
//     rolls to the next contract in the same series; nothing else moves it.
//
// The roll is deliberately keyed on the selected contract's OWN close time,
// not on its absence from the new list. Absence has innocent causes (a search
// result set, a horizon filter change), and a roll is journaled as a real
// market event — it fires when a contract actually expired, or not at all.

#include "services/prediction/PredictionTypes.h"

#include <QDateTime>
#include <QString>
#include <QVector>

namespace openmarketterminal::screens::kalshi {

/// Background list refresh cadence. Fast enough that a newly opened 15-minute
/// contract shows up well inside its life, slow enough to stay a rounding
/// error against the 1s DOM and evidence timers already running.
inline constexpr qint64 kMarketRefreshIntervalMs = 30'000;

/// Once the selected contract has expired the screen wants its successor
/// immediately, but the successor may not be listed yet — so the expiry path
/// retries on its own floor rather than firing on every tick.
inline constexpr qint64 kMarketExpiredRetryMs = 5'000;

/// A list fetch that has been in flight this long is presumed lost (the reply
/// errored on a path the screen does not observe, or the network dropped it).
/// The guard releases so the timer can recover instead of freezing forever.
inline constexpr qint64 kMarketRefreshStallMs = 90'000;

/// A contract is treated as expired `kMarketCloseGraceMs` after its stated
/// close: Kalshi stamps the close to the second and the screen's clock is not
/// the exchange's, so a roll fires just past the boundary, never just before.
inline constexpr qint64 kMarketCloseGraceMs = 2'000;

struct MarketRefreshState {
    bool visible = false;            // the screen is actually on-screen
    bool list_fetch_in_flight = false;
    qint64 fetch_started_ms = 0;     // when that in-flight fetch was issued
    qint64 last_fetch_ms = 0;        // when the last fetch was issued
    bool has_selection = false;
    QString selected_end_date_iso;   // close time of the selected contract
    bool selected_settled = false;   // the exchange reports it closed/inactive
};

struct MarketRefreshDecision {
    bool refresh = false;
    QString reason;  // "hidden" | "in-flight" | "waiting" | "expired" | "periodic"
};

/// Selection outcome for one populate pass. `row` is the row to select; -1
/// means the list is empty and nothing can be selected.
struct MarketSelectionDecision {
    int row = -1;
    bool same_contract = false;  // `row` holds the ticker that was already selected
    bool rolled = false;         // an expired contract handed off to its successor
    QString reason;  // "empty" | "no-selection" | "preserved" | "rolled" |
                     // "expired-no-successor" | "vanished"
    QString from_ticker;
    QString to_ticker;
};

namespace market_roll_detail {

/// Close time in epoch ms, or 0 when the market states none. A contract with
/// no stated close is never rolled off — an unreadable close is unknown, not
/// expired.
inline qint64 close_ms(const QString& end_date_iso) {
    if (end_date_iso.isEmpty()) return 0;
    const QDateTime close = QDateTime::fromString(end_date_iso, Qt::ISODate);
    return close.isValid() ? close.toUTC().toMSecsSinceEpoch() : 0;
}

/// The series a contract belongs to. Kalshi puts it in `extras`; when the
/// adapter did not carry it, the ticker's own prefix (`KXBTCD-…`) is the same
/// grouping, so the successor search still has something honest to match on.
inline QString series_key(const services::prediction::PredictionMarket& market) {
    QString series = market.extras.value(QStringLiteral("series_ticker")).toString().trimmed();
    if (!series.isEmpty()) return series;
    return market.key.market_id.section(QLatin1Char('-'), 0, 0);
}

inline bool is_expired(const QString& end_date_iso, bool settled, qint64 now_ms) {
    if (settled) return true;
    const qint64 close = close_ms(end_date_iso);
    return close > 0 && now_ms >= close + kMarketCloseGraceMs;
}

} // namespace market_roll_detail

/// Whether row `row` of `markets` holds the contract that is already selected.
/// This is the predicate the screen's select_market uses to skip its teardown
/// (unsubscribe, chart clear, freshness-stamp reset) when a background refresh
/// hands it back the same contract, and the predicate choose_market_row reports
/// as `same_contract` — one definition, so the two can never disagree.
inline bool is_selected_contract(const QVector<services::prediction::PredictionMarket>& markets,
                                 int row, const QString& selected_ticker, bool has_selection) {
    if (!has_selection || selected_ticker.isEmpty()) return false;
    if (row < 0 || row >= markets.size()) return false;
    return markets.at(row).key.market_id == selected_ticker;
}

/// Whether the screen may issue a market-list fetch now. `interval_ms` is the
/// background cadence; the expiry path uses its own shorter floor.
inline MarketRefreshDecision decide_market_refresh(const MarketRefreshState& state, qint64 now_ms,
                                                   qint64 interval_ms = kMarketRefreshIntervalMs) {
    MarketRefreshDecision decision;
    if (!state.visible) {
        decision.reason = QStringLiteral("hidden");
        return decision;
    }
    // One in flight at a time — the whole point of the guard — unless it has
    // been in flight long enough to be presumed lost.
    if (state.list_fetch_in_flight && now_ms - state.fetch_started_ms < kMarketRefreshStallMs) {
        decision.reason = QStringLiteral("in-flight");
        return decision;
    }
    const bool expired =
        state.has_selection &&
        market_roll_detail::is_expired(state.selected_end_date_iso, state.selected_settled, now_ms);
    const qint64 since = now_ms - state.last_fetch_ms;
    if (expired && since >= kMarketExpiredRetryMs) {
        decision.refresh = true;
        decision.reason = QStringLiteral("expired");
        return decision;
    }
    if (since >= interval_ms) {
        decision.refresh = true;
        decision.reason = QStringLiteral("periodic");
        return decision;
    }
    decision.reason = QStringLiteral("waiting");
    return decision;
}

/// Which row to select after `markets` (already filtered and sorted as the
/// list displays them) replaced the list.
///
/// `selected_ticker` empty means "no selection to preserve" — that is how an
/// operator-driven refresh (family/asset/cadence switch, first show) asks for
/// the top of the new list. A background refresh passes the live selection and
/// gets it back untouched unless that contract expired.
inline MarketSelectionDecision choose_market_row(
    const QVector<services::prediction::PredictionMarket>& markets,
    const QString& selected_ticker, const QString& selected_end_date_iso, bool selected_settled,
    qint64 now_ms) {
    using namespace market_roll_detail;
    MarketSelectionDecision decision;
    decision.from_ticker = selected_ticker;

    if (markets.isEmpty()) {
        decision.reason = QStringLiteral("empty");
        return decision;
    }
    if (selected_ticker.isEmpty()) {
        decision.row = 0;
        decision.to_ticker = markets.at(0).key.market_id;
        decision.reason = QStringLiteral("no-selection");
        return decision;
    }

    int previous_row = -1;
    for (int row = 0; row < markets.size(); ++row) {
        if (markets.at(row).key.market_id == selected_ticker) {
            previous_row = row;
            break;
        }
    }

    // The selected contract's own close time decides, and the freshly fetched
    // copy of it is more current than what the screen last stored.
    const QString close_iso = previous_row >= 0 ? markets.at(previous_row).end_date_iso
                                                : selected_end_date_iso;
    // `closed` alone, never `!active`: the adapter derives both from the
    // exchange's status string, and a market whose status did not parse would
    // read inactive. Unknown status is unknown, not expired — the close time
    // decides that case.
    const bool settled = previous_row >= 0 ? markets.at(previous_row).closed : selected_settled;
    const bool expired = is_expired(close_iso, settled, now_ms);

    if (previous_row >= 0 && !expired) {
        decision.row = previous_row;
        decision.same_contract = is_selected_contract(markets, previous_row, selected_ticker, true);
        decision.to_ticker = selected_ticker;
        decision.reason = QStringLiteral("preserved");
        return decision;
    }

    // Successor hunt: same series, closes after now, soonest first. Ties (a
    // whole ladder shares one close time) keep list order, which is the
    // screen's own actionability ranking — the same rule that puts a fresh
    // list's row 0 where it is.
    const QString series = previous_row >= 0 ? series_key(markets.at(previous_row))
                                             : selected_ticker.section(QLatin1Char('-'), 0, 0);
    int best_row = -1;
    qint64 best_close = 0;
    for (int row = 0; row < markets.size(); ++row) {
        const auto& market = markets.at(row);
        if (market.key.market_id == selected_ticker) continue;
        if (series_key(market) != series) continue;
        const qint64 close = close_ms(market.end_date_iso);
        if (close <= now_ms) continue;
        if (best_row < 0 || close < best_close) {
            best_row = row;
            best_close = close;
        }
    }

    if (!expired) {
        // The contract did not expire, it just is not in this payload (a
        // search result, a narrowed horizon). Fall back to the top of the
        // list, but do NOT call that a roll — nothing rolled over.
        decision.row = 0;
        decision.to_ticker = markets.at(0).key.market_id;
        decision.reason = QStringLiteral("vanished");
        return decision;
    }

    if (best_row < 0) {
        // Expired with no successor listed yet. Take the top of the list so
        // the screen still shows something live, and say plainly that this was
        // not a series roll.
        decision.row = 0;
        decision.to_ticker = markets.at(0).key.market_id;
        decision.reason = QStringLiteral("expired-no-successor");
        return decision;
    }

    decision.row = best_row;
    decision.rolled = true;
    decision.to_ticker = markets.at(best_row).key.market_id;
    decision.reason = QStringLiteral("rolled");
    return decision;
}

} // namespace openmarketterminal::screens::kalshi
