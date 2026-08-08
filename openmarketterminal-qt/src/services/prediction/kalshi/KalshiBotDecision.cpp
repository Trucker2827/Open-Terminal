#include "services/prediction/kalshi/KalshiBotDecision.h"

#include "services/edge_radar/KalshiAutoEngine.h"
#include "services/prediction/kalshi/KalshiEvidenceEngine.h"
// For the fill model's NAME only (kFillModel), so a bid row can state the model
// that will decide whether it ever becomes a position instead of restating it
// in a second literal that could drift. A .cpp-only include:
// KalshiBotOrders.h includes this header, and the decision math still knows
// nothing about the order lifecycle.
#include "services/prediction/kalshi/KalshiBotOrders.h"

#include <QDateTime>
#include <QSet>
#include <QTimeZone>
#include <QString>

#include <algorithm>
#include <cmath>

namespace openmarketterminal::services::prediction::kalshi_ns {

namespace {

constexpr auto kDecisionEvent = "kalshi_bot_decision";
constexpr auto kSettlementEvent = "kalshi_bot_paper_settlement";
constexpr auto kVoidEvent = "kalshi_bot_paper_void";
// 48h > any KXBTCD (~1h) / KXBTC15M (~15m) contract lifetime + the ~1-day
// settlement-record retention, so only a genuine orphan reaches the void.
constexpr qint64 kOrphanVoidAgeMs = 48LL * 60 * 60 * 1000;

/// Money is written to the ledger in whole cents. Kalshi quotes in cents, so
/// a rounded value here is exact rather than a convenience.
double round_cents(double dollars) { return std::round(dollars * 100.0) / 100.0; }

QString iso(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

/// The skeleton every ledger row shares. Numbers are added only once measured.
QJsonObject base_row(qint64 now_ms, const QString& ticker, const QString& action,
                     const QString& reason_code) {
    return QJsonObject{{QStringLiteral("event"), QString::fromLatin1(kDecisionEvent)},
                       {QStringLiteral("ts_ms"), static_cast<double>(now_ms)},
                       {QStringLiteral("ts"), iso(now_ms)},
                       {QStringLiteral("mode"), QStringLiteral("paper")},
                       {QStringLiteral("live_eligible"), false},
                       {QStringLiteral("ticker"), ticker},
                       {QStringLiteral("action"), action},
                       {QStringLiteral("reason_code"), reason_code}};
}

/// The calibrator's own track record, copied verbatim onto every row so the
/// ledger can be audited without the report that produced it. A Brier the
/// calibrator has not computed yet is reported as unavailable, never as 0.0.
///
/// Schema 2 (issue #171) separates three counts that schema 1 conflated, and
/// all three are carried: `scored_contracts` is the Brier's denominator,
/// `resolved_contracts` is the lifetime settled count, `training_observations`
/// is how many rows the weights were fitted on. The market is scored twice —
/// `brier_market_mid_raw` (the mid, untrained: the price this bot would
/// otherwise take) and `brier_market_trained_logit` (a fitted one-feature
/// baseline, kept for contrast). Only the raw mid backs the value claim.
QJsonObject track_record(const QJsonObject& report) {
    const QJsonValue brier_full = report.value(QStringLiteral("brier_full"));
    const QJsonValue brier_mid_raw = report.value(QStringLiteral("brier_market_mid_raw"));
    const QJsonValue brier_logit = report.value(QStringLiteral("brier_market_trained_logit"));
    const bool brier_available = brier_full.isDouble() && brier_mid_raw.isDouble();
    QJsonObject record{
        {QStringLiteral("resolved_contracts"), report.value(QStringLiteral("resolved_contracts")).toInt()},
        {QStringLiteral("scored_contracts"), report.value(QStringLiteral("scored_contracts")).toInt()},
        {QStringLiteral("training_observations"),
         report.value(QStringLiteral("training_observations")).toInt()},
        {QStringLiteral("adds_value_over_market"),
         report.value(QStringLiteral("adds_value_over_market")).toBool()},
        {QStringLiteral("brier_available"), brier_available},
        {QStringLiteral("generated_at_ms"), report.value(QStringLiteral("generated_at_ms"))}};
    if (brier_available) {
        record.insert(QStringLiteral("brier_full"), brier_full.toDouble());
        record.insert(QStringLiteral("brier_market_mid_raw"), brier_mid_raw.toDouble());
    }
    if (brier_logit.isDouble())
        record.insert(QStringLiteral("brier_market_trained_logit"), brier_logit.toDouble());
    return record;
}

} // namespace

bool KalshiBotDecision::signal_trusted(const QJsonObject& report) {
    // The claim, and the measurement it is a claim about. `track_record()`
    // already calls a report without both Briers unavailable; a report that
    // asserts value over a track record it does not carry is contradicting
    // itself, and this fails closed on it rather than believing the flag.
    return report.value(QStringLiteral("adds_value_over_market")).toBool() &&
           report.value(QStringLiteral("brier_full")).isDouble() &&
           report.value(QStringLiteral("brier_market_mid_raw")).isDouble();
}

QJsonObject KalshiBotDecision::merge_family_reports(const QJsonObject& threshold_report,
                                                    const QJsonObject& kxbtc15m_report,
                                                    qint64 now_ms,
                                                    const Config& config,
                                                    const QJsonObject& commodities_15m_report) {
    QJsonObject merged_predictions;
    qint64 newest_ms = 0;
    const auto take_fresh_filtered = [&](const QJsonObject& filtered_report) {
        const qint64 generated_ms =
            static_cast<qint64>(filtered_report.value(QStringLiteral("generated_at_ms")).toDouble());
        if (generated_ms <= 0) return;
        const qint64 age_ms = now_ms - generated_ms;
        if (age_ms >= config.max_report_age_ms || age_ms < 0) return;
        const QJsonObject filtered =
            filtered_report.value(QStringLiteral("predictions")).toObject();
        for (auto it = filtered.constBegin(); it != filtered.constEnd(); ++it)
            merged_predictions.insert(it.key(), it.value());
        newest_ms = std::max(newest_ms, generated_ms);
    };
    take_fresh_filtered(filter_predictions_for_family(threshold_report, /*keep_kxbtc15m=*/false));
    take_fresh_filtered(filter_predictions_for_family(kxbtc15m_report, /*keep_kxbtc15m=*/true));
    take_fresh_filtered(filter_commodity_15m_predictions(commodities_15m_report));
    if (merged_predictions.isEmpty() || newest_ms <= 0) return {};
    return QJsonObject{{QStringLiteral("generated_at_ms"), static_cast<double>(newest_ms)},
                       {QStringLiteral("predictions"), merged_predictions},
                       {QStringLiteral("event"), QStringLiteral("merged_family_calibrators")}};
}

QJsonArray KalshiBotDecision::decide(const QJsonObject& report,
                                     const QJsonArray& open_positions,
                                     const QJsonArray& settled_positions,
                                     qint64 now_ms,
                                     const Config& config,
                                     const KalshiBotStopFile& stop) {
    return decide(report, open_positions, settled_positions, now_ms, config, stop, Exposure{});
}

QJsonArray KalshiBotDecision::decide(const QJsonObject& report,
                                     const QJsonArray& open_positions,
                                     const QJsonArray& settled_positions,
                                     qint64 now_ms,
                                     const Config& config,
                                     const KalshiBotStopFile& stop,
                                     const Exposure& exposure) {
    // --- the kill switch, before anything else -----------------------------
    // This is the only path in this class that can produce a bid, so checking
    // here is what makes "checked every tick before any bid" structural. It
    // sits ahead of the rung 6 exposure/requote math for the same reason: no
    // lifecycle bookkeeping may run before the switch has been honoured. The
    // refusal is journaled rather than silent: a stop that left no row would
    // look exactly like a crashed loop.
    if (stop.engaged) {
        QJsonObject row = base_row(now_ms, QString(), QStringLiteral("pass"),
                                   QString::fromLatin1(kBotStopped));
        if (stop.ts_ms > 0) row.insert(QStringLiteral("stop_ts_ms"), static_cast<double>(stop.ts_ms));
        if (!stop.source.isEmpty()) row.insert(QStringLiteral("stop_source"), stop.source);
        if (!stop.reason.isEmpty()) row.insert(QStringLiteral("stop_reason"), stop.reason);
        return QJsonArray{row};
    }


    // --- the report itself must be trustworthy before any contract is read.
    const qint64 generated_ms =
        static_cast<qint64>(report.value(QStringLiteral("generated_at_ms")).toDouble());
    if (generated_ms <= 0)
        return QJsonArray{base_row(now_ms, QString(), QStringLiteral("pass"), QString::fromLatin1(kReportMissing))};

    const qint64 age_ms = now_ms - generated_ms;
    if (age_ms >= config.max_report_age_ms || age_ms < 0) {
        QJsonObject row =
            base_row(now_ms, QString(), QStringLiteral("pass"), QString::fromLatin1(kReportStale));
        // The file's age is an observation about the file, not a withheld
        // measurement — every probability and price stays absent.
        row.insert(QStringLiteral("report_age_ms"), static_cast<double>(age_ms));
        row.insert(QStringLiteral("max_report_age_ms"), static_cast<double>(config.max_report_age_ms));
        return QJsonArray{row};
    }

    // Signal trust is re-read from the live report on every call: the
    // calibrator sets adds_value_over_market only once its Brier beats the raw
    // market mid across its own >=100-CONTRACT gate (issue #171).
    const bool trusted = signal_trusted(report);
    const QJsonObject record = track_record(report);
    const QJsonObject predictions = report.value(QStringLiteral("predictions")).toObject();

    const auto finish = [&](QJsonObject row) {
        row.insert(QStringLiteral("signal_trusted"), trusted);
        row.insert(QStringLiteral("track_record"), record);
        return row;
    };

    // --- an untrusted signal does not bid (issue #165) ----------------------
    // Betting a self-reportedly edgeless signal into adverse selection minus
    // fees loses by construction, so the untrusted-bid path is gone: the tick
    // is one journaled PASS carrying the track record that refused it. It sits
    // beside the report's own refusals because trust is a property of the
    // REPORT, not of any one contract — the same reason REPORT_STALE is one
    // row and EDGE_BELOW_THRESHOLD is one per contract. Nothing downstream can
    // reinstate the bid: this returns before a single contract is priced.
    if (!trusted)
        return QJsonArray{finish(base_row(now_ms, QString(), QStringLiteral("pass"),
                                          QString::fromLatin1(kSignalUntrusted)))};

    if (predictions.isEmpty())
        return QJsonArray{finish(base_row(now_ms, QString(), QStringLiteral("pass"),
                                          QString::fromLatin1(kNoPredictions)))};

    QSet<QString> held;
    for (const auto& value : open_positions)
        held.insert(value.toObject().value(QStringLiteral("ticker")).toString());
    QSet<QString> settled;
    for (const auto& value : settled_positions)
        settled.insert(value.toObject().value(QStringLiteral("ticker")).toString());
    QSet<QString> resting;
    for (const auto& value : exposure.resting)
        resting.insert(value.toObject().value(QStringLiteral("ticker")).toString());

    // Running totals: the caps bind across the tick, not just against the book
    // this tick opened with. Five contracts at $2 each is $10 of new exposure
    // whether it is bid in five ticks or one.
    double at_risk_usd = exposure.at_risk_usd;
    double session_opened_usd = exposure.session_opened_usd;

    QJsonArray rows;
    for (auto it = predictions.constBegin(); it != predictions.constEnd(); ++it) {
        const QString ticker = it.key();
        const QJsonObject prediction = it.value().toObject();
        const double calibrated_p = prediction.value(QStringLiteral("p_yes_full")).toDouble();
        const double market_mid = prediction.value(QStringLiteral("market_yes_mid")).toDouble();
        const QJsonObject features = prediction.value(QStringLiteral("features")).toObject();
        const double sqrt_minutes_left = features.value(QStringLiteral("sqrt_minutes_left")).toDouble();

        if (!(calibrated_p > 0.0 && calibrated_p < 1.0) || !(market_mid > 0.0 && market_mid < 1.0) ||
            !(sqrt_minutes_left > 0.0)) {
            rows.append(finish(base_row(now_ms, ticker, QStringLiteral("pass"),
                                        QString::fromLatin1(kMalformedPrediction))));
            continue;
        }

        QJsonObject row = base_row(now_ms, ticker, QStringLiteral("pass"), QString());
        row.insert(QStringLiteral("calibrated_p"), calibrated_p);
        row.insert(QStringLiteral("market_mid"), market_mid);

        // A resolved contract is never bid again, whatever runway the report
        // still claims for it: the report's runway comes from the daemon
        // snapshot, which can be stale under a freshly generated report.
        if (settled.contains(ticker)) {
            row.insert(QStringLiteral("reason_code"), QString::fromLatin1(kContractSettled));
            rows.append(finish(row));
            continue;
        }

        // The runway feature was measured when the report was generated;
        // what matters is how much of it is left NOW.
        const double runway_seconds =
            sqrt_minutes_left * sqrt_minutes_left * 60.0 - static_cast<double>(age_ms) / 1000.0;
        row.insert(QStringLiteral("runway_seconds"), runway_seconds);
        if (runway_seconds < static_cast<double>(config.min_runway_seconds)) {
            row.insert(QStringLiteral("reason_code"), QString::fromLatin1(kNoRunway));
            rows.append(finish(row));
            continue;
        }

        const double edge = calibrated_p - market_mid;
        row.insert(QStringLiteral("edge"), edge);
        row.insert(QStringLiteral("edge_threshold"), config.edge_threshold);
        if (std::fabs(edge) < config.edge_threshold) {
            row.insert(QStringLiteral("reason_code"), QString::fromLatin1(kEdgeBelowThreshold));
            rows.append(finish(row));
            continue;
        }

        if (held.contains(ticker)) {
            row.insert(QStringLiteral("reason_code"), QString::fromLatin1(kAlreadyHeld));
            rows.append(finish(row));
            continue;
        }

        // A quote this tick's lifecycle pass chose to leave working still owns
        // this contract: the bot replaces quotes, it never stacks them.
        if (resting.contains(ticker)) {
            row.insert(QStringLiteral("reason_code"), QString::fromLatin1(kQuoteResting));
            rows.append(finish(row));
            continue;
        }

        // --- two-tier pricing (issue #158) ----------------------------------
        // The PASSIVE tier is rung 1's, unchanged: a limit at the mid, floored
        // to the cent (never pay above the mid) on whichever side the edge
        // points. It fills only when the market comes down through it, which
        // is why 147 of rung 6's first 151 quotes were canceled instead.
        const bool yes_side = edge > 0.0;
        const double side_mid = yes_side ? market_mid : 1.0 - market_mid;
        const double side_p = yes_side ? calibrated_p : 1.0 - calibrated_p;
        const double side_edge = side_p - side_mid;   // == |edge|, by construction
        const double rest_price = std::floor(side_mid * 100.0 + 1e-9) / 100.0;

        // Postmortem lesson: ban NO fades of already-high YES near close.
        // Worst paper losses were cheap NO crosses while YES mid ≥ ~0.85 and
        // the market still resolved YES. Lift only when venue lead confirms
        // down or BRTI avg60 is below open; missing confirm → ban stands.
        if (!yes_side && config.ban_no_fade_yes_mid > 0.0 &&
            market_mid + 1e-9 >= config.ban_no_fade_yes_mid &&
            runway_seconds <= static_cast<double>(config.ban_no_fade_max_runway_sec) + 1e-9) {
            const QString lift_reason = outside_info_no_fade_lift_reason(prediction);
            if (!lift_reason.isEmpty()) {
                row.insert(QStringLiteral("fade_ban_lifted"), true);
                row.insert(QStringLiteral("fade_ban_lift_reason"), lift_reason);
                // Fall through — allow the NO bid path.
            } else {
                row.insert(QStringLiteral("reason_code"), QString::fromLatin1(kFadeYesNearClose));
                row.insert(QStringLiteral("side"), QStringLiteral("NO"));
                row.insert(QStringLiteral("side_edge"), side_edge);
                row.insert(QStringLiteral("ban_no_fade_yes_mid"), config.ban_no_fade_yes_mid);
                row.insert(QStringLiteral("ban_no_fade_max_runway_sec"),
                           config.ban_no_fade_max_runway_sec);
                rows.append(finish(row));
                continue;
            }
        }

        // The CROSSING tier prices off the side's own REAL ask, as observed by
        // the daemon and passed through by the calibrator. Kalshi's NO book is
        // a book: the NO ask is read directly, never inferred from the YES bid.
        const QJsonValue ask_value = prediction.value(
            yes_side ? QStringLiteral("market_yes_ask") : QStringLiteral("market_no_ask"));
        const QJsonValue bid_value = prediction.value(
            yes_side ? QStringLiteral("market_yes_bid") : QStringLiteral("market_no_bid"));

        const char* style_reason = nullptr;
        double cross_price = 0.0;
        const double side_ask = ask_value.toDouble(0.0);
        if (!ask_value.isDouble()) {
            // No ask for this side. An unknown spread is not a free one.
            style_reason = kRestNoBook;
        } else if (!(side_ask > 0.0 && side_ask < 1.0) || side_ask < side_mid - 1e-9) {
            // An ask below the mid it is supposed to be half of is data
            // contradicting itself, and it would yield a NEGATIVE spread cost
            // — crossing would look cheaper than resting. Inconsistent is a
            // species of unavailable, and both rest.
            style_reason = kRestBookInconsistent;
        } else {
            // Rounded UP to the cent: paying is a cost, and rounding a cost
            // down would flatter the hurdle it has to clear.
            cross_price = std::ceil(side_ask * 100.0 - 1e-9) / 100.0;
            if (!(cross_price > 0.0 && cross_price < 1.0)) style_reason = kRestBookInconsistent;
        }

        const bool book_priced = style_reason == nullptr;
        double spread_cost = 0.0;
        double cross_fee = 0.0;
        double net_ev = 0.0;
        double effective_cross_margin = config.cross_margin_usd;
        bool cross = false;
        if (book_priced) {
            spread_cost = cross_price - side_mid;
            // The hurdle is judged per contract, at n=1: the fee schedule
            // ceils to the whole cent for the whole order, so the size the
            // caps end up allowing cannot be known before the tier is. A
            // one-contract ceil overstates the hurdle, which is the safe
            // direction. The order's ACTUAL total fee is `fee_usd`, sized
            // below at the price this choice picks.
            cross_fee = KalshiEvidenceEngine::conservative_taker_fee(cross_price, 1.0);
            net_ev = side_p - cross_price - cross_fee;
            // Postmortem lesson: favourite asymmetry — avg win $0.89 vs avg
            // loss $1.79. Crossing an expensive ask needs more surviving EV
            // before we pay for the fill; else rest (or pass on rest premium).
            if (config.favourite_cross_extra_margin_usd > 0.0 &&
                cross_price + 1e-9 >= config.favourite_cross_price)
                effective_cross_margin =
                    config.cross_margin_usd + config.favourite_cross_extra_margin_usd;
            cross = net_ev > effective_cross_margin + 1e-9;
            style_reason = cross ? kCrossEdgeClearsCost : kRestEdgeBelowCost;
        }

        const double price = cross ? cross_price : rest_price;
        if (!(price > 0.0 && price < 1.0)) {
            row.insert(QStringLiteral("reason_code"), QString::fromLatin1(kMalformedPrediction));
            rows.append(finish(row));
            continue;
        }

        // Attached before the caps run, so a refusal row says which tier it
        // was pricing too — a bid refused for size at the ask is a different
        // fact from one refused at the mid.
        row.insert(QStringLiteral("quote_style"),
                   QString::fromLatin1(cross ? kQuoteCross : kQuoteRest));
        row.insert(QStringLiteral("quote_style_reason"), QString::fromLatin1(style_reason));
        row.insert(QStringLiteral("rest_price"), rest_price);
        row.insert(QStringLiteral("cross_margin_usd"), config.cross_margin_usd);
        // |edge| by construction, and the quantity BOTH hurdles are judged
        // against — so it is on every priced row, not only the book-priced
        // ones, now that the resting tier has a hurdle of its own.
        row.insert(QStringLiteral("side_edge"), side_edge);
        if (book_priced) {
            // The arithmetic that chose the tier, on the rows of BOTH tiers:
            // a rest that was priced against a real book has to show the sum
            // it failed, or "the edge did not clear the cost" is an assertion.
            row.insert(QStringLiteral("side_ask"), side_ask);
            if (bid_value.isDouble()) row.insert(QStringLiteral("side_bid"), bid_value.toDouble());
            row.insert(QStringLiteral("cross_price"), cross_price);
            row.insert(QStringLiteral("spread_cost_usd"), spread_cost);
            row.insert(QStringLiteral("cross_fee_usd"), cross_fee);
            row.insert(QStringLiteral("cross_cost_usd"), spread_cost + cross_fee);
            row.insert(QStringLiteral("net_ev_usd"), net_ev);
            row.insert(QStringLiteral("effective_cross_margin_usd"), effective_cross_margin);
            if (effective_cross_margin > config.cross_margin_usd + 1e-9) {
                row.insert(QStringLiteral("favourite_cross_price"), config.favourite_cross_price);
                row.insert(QStringLiteral("favourite_cross_extra_margin_usd"),
                           config.favourite_cross_extra_margin_usd);
            }
        }

        // --- the resting tier's adverse-selection premium (issue #165) ------
        // A resting fill arrives when the market came to the quote, which is
        // disproportionately when the market moved AGAINST it; the crossing
        // tier's fill is bought outright. So the same edge is worth less
        // resting, and a rest demands more of it. The hurdle is journaled in
        // full — threshold, premium, the sum, and the edge judged against it —
        // so a refusal is checkable from the ledger alone rather than asserted.
        //
        // Deliberately asymmetric: the crossing hurdle above is untouched, so a
        // contract can clear the cross arithmetic and fail this one, and it
        // crosses. Nothing here is a second chance for a cross to rest.
        if (!cross) {
            const double rest_threshold = config.edge_threshold + config.rest_premium_usd;
            row.insert(QStringLiteral("rest_premium_usd"), config.rest_premium_usd);
            row.insert(QStringLiteral("rest_threshold"), rest_threshold);
            if (side_edge < rest_threshold - 1e-9) {
                row.insert(QStringLiteral("reason_code"),
                           QString::fromLatin1(kRestEdgeBelowPremium));
                rows.append(finish(row));
                continue;
            }
        }

        // Size down until BOTH ceilings hold: stake alone, and stake plus the
        // conservative taker fee. A size that clears neither is not bid.
        int contracts = static_cast<int>(std::floor(config.max_stake_usd / price + 1e-9));
        double stake = 0.0;
        double fee = 0.0;
        while (contracts >= 1) {
            stake = round_cents(contracts * price);
            fee = KalshiEvidenceEngine::conservative_taker_fee(price, contracts);
            if (stake <= config.max_stake_usd && round_cents(stake + fee) <= config.max_all_in_usd) break;
            --contracts;
        }
        if (contracts < 1) {
            row.insert(QStringLiteral("reason_code"), QString::fromLatin1(kSizeCapBlocksBid));
            row.insert(QStringLiteral("price"), price);
            row.insert(QStringLiteral("max_stake_usd"), config.max_stake_usd);
            row.insert(QStringLiteral("max_all_in_usd"), config.max_all_in_usd);
            rows.append(finish(row));
            continue;
        }

        // Exposure gates, in the PR #44 currency: a resting remainder is money
        // at risk, so a bid is REFUSED (never quietly sized down) when the
        // book plus this bid would breach a ceiling.
        const double all_in = round_cents(stake + fee);
        const auto refuse_on_cap = [&](const char* reason_code, double used, double cap) {
            row.insert(QStringLiteral("reason_code"), QString::fromLatin1(reason_code));
            row.insert(QStringLiteral("price"), price);
            row.insert(QStringLiteral("contracts"), contracts);
            row.insert(QStringLiteral("all_in_usd"), all_in);
            row.insert(QStringLiteral("exposure_used_usd"), used);
            row.insert(QStringLiteral("exposure_cap_usd"), cap);
            rows.append(finish(row));
        };
        if (at_risk_usd + all_in > config.max_open_exposure_usd + 1e-9) {
            refuse_on_cap(kExposureCapBlocksBid, at_risk_usd, config.max_open_exposure_usd);
            continue;
        }
        if (config.enforce_session_budget &&
            session_opened_usd + all_in > config.session_budget_usd + 1e-9) {
            refuse_on_cap(kSessionBudgetBlocksBid, session_opened_usd, config.session_budget_usd);
            continue;
        }
        // PR #44's shape exactly: the book contributes STAKE (its exposure
        // rule), the new order contributes its all-in (its worst case).
        at_risk_usd = round_cents(at_risk_usd + stake);
        session_opened_usd = round_cents(session_opened_usd + all_in);

        row.insert(QStringLiteral("action"), QStringLiteral("bid"));
        // Every bid that reaches here was made on a TRUSTED signal — the
        // untrusted tick returned before any contract was priced (#165), so
        // SIGNAL_UNTRUSTED is a pass code now and cannot label a bid. A
        // contract whose own quote this tick's TTL pass just pulled is the
        // replace half of cancel/replace, and says so.
        const QString replaces = exposure.requoted.value(ticker).toString();
        row.insert(QStringLiteral("reason_code"),
                   QString::fromLatin1(!replaces.isEmpty() ? kRequoted : kEdgeClearsThreshold));
        if (!replaces.isEmpty()) {
            row.insert(QStringLiteral("requote"), true);
            row.insert(QStringLiteral("replaces_position_id"), replaces);
        }
        row.insert(QStringLiteral("side"), yes_side ? QStringLiteral("YES") : QStringLiteral("NO"));
        row.insert(QStringLiteral("price"), price);
        row.insert(QStringLiteral("contracts"), contracts);
        row.insert(QStringLiteral("stake_usd"), stake);
        row.insert(QStringLiteral("fee_usd"), fee);
        row.insert(QStringLiteral("all_in_usd"), all_in);
        row.insert(QStringLiteral("max_stake_usd"), config.max_stake_usd);
        row.insert(QStringLiteral("max_all_in_usd"), config.max_all_in_usd);
        row.insert(QStringLiteral("position_id"), ticker + QStringLiteral("@") + QString::number(now_ms));
        // Ladder rung 6: this is an ORDER, and it starts life RESTING. It
        // becomes a position only when something observable fills it, and it
        // is pulled when its TTL runs out or its edge goes (KalshiBotOrders).
        // Until then its remainder counts as exposure at the limit price.
        row.insert(QStringLiteral("order_state"), QStringLiteral("resting"));
        row.insert(QStringLiteral("limit_price"), price);
        // The disclosure, promoted from a comment to the row (#158): whether
        // this order ever becomes a position is decided by a stated MODEL, not
        // by a venue. A crossing bid satisfies that model's condition the
        // moment it is placed — which is the whole point of paying for it, and
        // exactly why the row has to say the fill was modelled.
        //
        // The model's NAME only. `KalshiBotOrders::kFillRule`, the prose beside
        // it, describes the passive tier in terms this rung has falsified for a
        // crossing bid ("calibrator.json carries no book"; "its market mid is
        // the ask proxy"; "it selects on the market having moved to the quote"
        // — a cross moves the quote to the market). Stamping that sentence onto
        // a crossing bid would be a false disclosure on the rows this change is
        // meant to make the majority. What is true of THIS row is already on it,
        // in `quote_style`, `quote_style_reason` and the cost arithmetic.
        row.insert(QStringLiteral("fill_model"), QString::fromLatin1(KalshiBotOrders::kFillModel));
        row.insert(QStringLiteral("filled_count"), 0);
        row.insert(QStringLiteral("remaining_count"), contracts);
        row.insert(QStringLiteral("ttl_ms"), static_cast<double>(config.quote_ttl_seconds) * 1000.0);
        row.insert(QStringLiteral("expires_at_ms"),
                   static_cast<double>(now_ms + qint64{config.quote_ttl_seconds} * 1000));
        // Named for what it is: the book INCLUDING this order. A refusal row's
        // `exposure_used_usd` is the book WITHOUT the bid it refused, and one
        // number wearing both meanings would be unreadable.
        row.insert(QStringLiteral("exposure_after_usd"), at_risk_usd);
        row.insert(QStringLiteral("exposure_cap_usd"), config.max_open_exposure_usd);
        rows.append(finish(row));
    }
    return rows;
}

QJsonArray KalshiBotDecision::normalize_settlements(const QJsonArray& account_settlements,
                                                    const QJsonArray& settlement_labels) {
    QJsonArray out;
    const auto take = [&out](const QJsonArray& rows, const QString& ticker_key,
                             const QString& result_key, const QString& time_key,
                             const QString& source) {
        for (const auto& value : rows) {
            const QJsonObject row = value.toObject();
            const QString ticker = row.value(ticker_key).toString().trimmed();
            const QString result = row.value(result_key).toString().trimmed().toUpper();
            // An unresolved market stays unresolved: anything that is not a
            // posted YES/NO is dropped rather than interpreted.
            if (ticker.isEmpty() ||
                (result != QStringLiteral("YES") && result != QStringLiteral("NO")))
                continue;
            out.append(QJsonObject{{QStringLiteral("ticker"), ticker},
                                   {QStringLiteral("market_result"), result},
                                   {QStringLiteral("settled_time"), row.value(time_key).toString()},
                                   {QStringLiteral("source"), source}});
        }
    };
    take(account_settlements, QStringLiteral("market_id"), QStringLiteral("market_result"),
         QStringLiteral("settled_time"), QStringLiteral("kalshi-account-settlements.jsonl"));
    take(settlement_labels, QStringLiteral("kalshi_market_id"), QStringLiteral("result"),
         QStringLiteral("settlement_ts"), QStringLiteral("kalshi-settlements.jsonl"));
    return out;
}

namespace {

/// Decision fields already on the filled position (from the bid row). Copied
/// onto the settlement so a postmortem can classify without re-joining the
/// ledger. Absent on the position → absent on the settlement (never zeroed).
void copy_bid_snapshot_into(QJsonObject& settlement, const QJsonObject& position) {
    static const char* const kKeys[] = {
        "calibrated_p", "market_mid",     "edge",           "side_edge",
        "runway_seconds", "quote_style",  "quote_style_reason", "net_ev_usd",
        "cross_cost_usd", "fill_model",   "ttl_ms",         "signal_trusted",
        "reason_code"};
    for (const char* key : kKeys) {
        const QString k = QString::fromLatin1(key);
        if (position.contains(k)) settlement.insert(k, position.value(k));
    }
}

void maybe_insert_mid_at_settle(QJsonObject& settlement, const QString& ticker,
                                const QHash<QString, double>& market_mid_by_ticker) {
    const auto it = market_mid_by_ticker.constFind(ticker);
    if (it == market_mid_by_ticker.constEnd()) return;
    const double mid = it.value();
    if (!(mid > 0.0 && mid < 1.0)) return;
    settlement.insert(QStringLiteral("market_mid_at_settle"), mid);
}

} // namespace

QJsonArray KalshiBotDecision::settle_paper(const QJsonArray& open_positions,
                                           const QJsonArray& settlements,
                                           qint64 now_ms,
                                           const QHash<QString, double>& market_mid_by_ticker) {
    QHash<QString, QJsonObject> by_ticker;
    for (const auto& value : settlements) {
        const QJsonObject row = value.toObject();
        const QString ticker = row.value(QStringLiteral("ticker")).toString();
        if (!ticker.isEmpty() && !by_ticker.contains(ticker)) by_ticker.insert(ticker, row);
    }

    QJsonArray out;
    for (const auto& value : open_positions) {
        const QJsonObject position = value.toObject();
        if (position.value(QStringLiteral("action")).toString() != QStringLiteral("bid")) continue;
        const QString ticker = position.value(QStringLiteral("ticker")).toString();
        const auto found = by_ticker.constFind(ticker);
        if (found == by_ticker.constEnd()) {
            // No settlement record. Young: a real settlement may still arrive
            // (matched first, above) — leave it open. Old past the horizon: a
            // genuine orphan whose market resolved long ago in a retained
            // window that rotated out. Retire it UNRESOLVED (never a fabricated
            // win/loss) so its exposure is released. Age is placement age
            // (TZ-free), not a ticker-parsed close.
            const qint64 placed_ms =
                static_cast<qint64>(position.value(QStringLiteral("ts_ms")).toDouble());
            if (placed_ms <= 0 || now_ms - placed_ms < kOrphanVoidAgeMs) continue;
            out.append(QJsonObject{
                {QStringLiteral("event"), QString::fromLatin1(kVoidEvent)},
                {QStringLiteral("ts_ms"), static_cast<double>(now_ms)},
                {QStringLiteral("ts"), iso(now_ms)},
                {QStringLiteral("mode"), QStringLiteral("paper")},
                {QStringLiteral("live_eligible"), false},
                {QStringLiteral("position_id"), position.value(QStringLiteral("position_id"))},
                {QStringLiteral("ticker"), ticker},
                {QStringLiteral("side"), position.value(QStringLiteral("side"))},
                {QStringLiteral("contracts"), position.value(QStringLiteral("contracts"))},
                {QStringLiteral("stake_usd"), position.value(QStringLiteral("stake_usd"))},
                {QStringLiteral("fee_usd"), position.value(QStringLiteral("fee_usd"))},
                {QStringLiteral("resolution"), QStringLiteral("unresolved_expired")},
                {QStringLiteral("reason"),
                 QStringLiteral("position aged out with no settlement record in the retained window")},
                {QStringLiteral("placed_ms"), static_cast<double>(placed_ms)},
                {QStringLiteral("age_ms"), static_cast<double>(now_ms - placed_ms)},
                {QStringLiteral("won"), QJsonValue::Null},
                {QStringLiteral("realized_pnl"), 0.0}});
            continue;
        }

        const QString side = position.value(QStringLiteral("side")).toString();
        const QString result = found->value(QStringLiteral("market_result")).toString();
        const int contracts = position.value(QStringLiteral("contracts")).toInt();
        const double stake = position.value(QStringLiteral("stake_usd")).toDouble();
        const double fee = position.value(QStringLiteral("fee_usd")).toDouble();
        const bool won = result == side;
        const double payout = won ? contracts * 1.0 : 0.0;
        QJsonObject row{
            {QStringLiteral("event"), QString::fromLatin1(kSettlementEvent)},
            {QStringLiteral("ts_ms"), static_cast<double>(now_ms)},
            {QStringLiteral("ts"), iso(now_ms)},
            {QStringLiteral("mode"), QStringLiteral("paper")},
            {QStringLiteral("live_eligible"), false},
            {QStringLiteral("position_id"), position.value(QStringLiteral("position_id"))},
            {QStringLiteral("ticker"), ticker},
            {QStringLiteral("side"), side},
            {QStringLiteral("contracts"), contracts},
            {QStringLiteral("price"), position.value(QStringLiteral("price"))},
            {QStringLiteral("stake_usd"), stake},
            {QStringLiteral("fee_usd"), fee},
            {QStringLiteral("market_result"), result},
            {QStringLiteral("won"), won},
            {QStringLiteral("payout_usd"), payout},
            {QStringLiteral("realized_pnl"), round_cents(payout - stake - fee)},
            {QStringLiteral("settlement_source"), found->value(QStringLiteral("source"))},
            {QStringLiteral("settled_time"), found->value(QStringLiteral("settled_time"))}};
        copy_bid_snapshot_into(row, position);
        maybe_insert_mid_at_settle(row, ticker, market_mid_by_ticker);
        out.append(row);
    }
    return out;
}

namespace {

bool venue_lead_confirms_no_fade(const QJsonObject& features) {
    // Fail closed: without explicit lead fields there is no confirm to lift on.
    if (!features.contains(QStringLiteral("lead_confirms_direction")) ||
        !features.contains(QStringLiteral("lead_conflicts")) ||
        !features.value(QStringLiteral("venue_lead_bps_30s")).isDouble())
        return false;
    if (features.value(QStringLiteral("lead_conflicts")).toBool()) return false;
    if (!features.value(QStringLiteral("lead_confirms_direction")).toBool()) return false;
    // Downward venue lead agrees with fading YES (NO thesis).
    return features.value(QStringLiteral("venue_lead_bps_30s")).toDouble() < 0.0;
}

bool brti_avg60_confirms_no_fade(const QJsonObject& features) {
    // KXBTC15M pays on CF BRTI 60s average vs open. Avg60 below open confirms
    // the NO thesis when fading a high YES. Fail closed without both prices.
    if (!features.value(QStringLiteral("brti_avg_60s")).isDouble() ||
        !features.value(QStringLiteral("open_price")).isDouble())
        return false;
    const double avg60 = features.value(QStringLiteral("brti_avg_60s")).toDouble();
    const double open = features.value(QStringLiteral("open_price")).toDouble();
    if (!(avg60 > 0.0 && open > 0.0) || !(avg60 < open)) return false;
    // When physics-on-BRTI is present it must agree NO (< 0.5).
    if (features.value(QStringLiteral("p_brti_avg60")).isDouble() &&
        features.value(QStringLiteral("p_brti_avg60")).toDouble() >= 0.5)
        return false;
    return true;
}

} // namespace

QString KalshiBotDecision::outside_info_no_fade_lift_reason(const QJsonObject& prediction) {
    const QJsonObject features = prediction.value(QStringLiteral("features")).toObject();
    if (venue_lead_confirms_no_fade(features))
        return QStringLiteral("lead_confirms_down");
    if (brti_avg60_confirms_no_fade(features))
        return QStringLiteral("brti_avg60_below_open");
    return {};
}

bool KalshiBotDecision::outside_info_confirms_no_fade(const QJsonObject& prediction) {
    return !outside_info_no_fade_lift_reason(prediction).isEmpty();
}

void KalshiBotDecision::sample_mid_path(const QJsonArray& open_positions,
                                        const QJsonObject& predictions,
                                        qint64 now_ms,
                                        MidPathStore* store,
                                        int max_samples) {
    if (!store || max_samples < 1) return;
    for (const auto& value : open_positions) {
        const QJsonObject position = value.toObject();
        const QString pid = position.value(QStringLiteral("position_id")).toString();
        const QString ticker = position.value(QStringLiteral("ticker")).toString();
        if (pid.isEmpty() || ticker.isEmpty()) continue;
        const QJsonObject prediction = predictions.value(ticker).toObject();
        const QJsonValue mid_v = prediction.value(QStringLiteral("market_yes_mid"));
        if (!mid_v.isDouble()) continue;
        const double mid = mid_v.toDouble();
        if (!(mid > 0.0 && mid < 1.0)) continue;
        QJsonObject sample{{QStringLiteral("ts_ms"), static_cast<double>(now_ms)},
                           {QStringLiteral("market_yes_mid"), mid}};
        const QString side = position.value(QStringLiteral("side")).toString().toUpper();
        const QJsonValue bid_v = prediction.value(
            side == QLatin1String("NO") ? QStringLiteral("market_no_bid")
                                        : QStringLiteral("market_yes_bid"));
        if (bid_v.isDouble()) sample.insert(QStringLiteral("held_side_bid"), bid_v.toDouble());
        QJsonArray path = store->value(pid);
        if (!path.isEmpty()) {
            const qint64 last =
                static_cast<qint64>(path.last().toObject().value(QStringLiteral("ts_ms")).toDouble());
            if (last == now_ms) continue;  // one sample per tick
        }
        path.append(sample);
        while (path.size() > max_samples) path.removeFirst();
        store->insert(pid, path);
    }
}

void KalshiBotDecision::attach_mid_path(QJsonObject& settlement_or_close, MidPathStore* store) {
    if (!store) return;
    const QString pid = settlement_or_close.value(QStringLiteral("position_id")).toString();
    if (pid.isEmpty() || !store->contains(pid)) return;
    settlement_or_close.insert(QStringLiteral("mid_path"), store->take(pid));
}

QJsonArray KalshiBotDecision::paper_cashout(const QJsonArray& open_positions,
                                            const QJsonObject& report,
                                            qint64 now_ms,
                                            const Config& config,
                                            CashoutStreak* cut_streak) {
    using openmarketterminal::services::edge_radar::KalshiAutoEngine;
    using openmarketterminal::services::edge_radar::KalshiExitConstraints;
    using openmarketterminal::services::edge_radar::KalshiPositionExitInput;

    QJsonArray out;
    if (!config.enable_paper_cashout || open_positions.isEmpty()) return out;

    const qint64 generated_ms =
        static_cast<qint64>(report.value(QStringLiteral("generated_at_ms")).toDouble());
    const qint64 age_ms =
        (generated_ms > 0 && now_ms >= generated_ms) ? (now_ms - generated_ms) : -1;
    if (report.isEmpty() || generated_ms <= 0 || age_ms < 0 ||
        age_ms > config.max_report_age_ms) {
        // One refusal row for the tick — not per position — so a stale report
        // does not spam the ledger while still documenting the fail-closed hold.
        if (!open_positions.isEmpty()) {
            out.append(base_row(now_ms, QString(), QStringLiteral("pass"),
                                QString::fromLatin1(kCashoutStaleReport)));
        }
        return out;
    }

    const QJsonObject predictions = report.value(QStringLiteral("predictions")).toObject();
    KalshiExitConstraints constraints;
    constraints.max_signal_age_seconds =
        static_cast<int>(std::min<qint64>(constraints.max_signal_age_seconds,
                                          config.max_report_age_ms / 1000));

    for (const auto& value : open_positions) {
        const QJsonObject position = value.toObject();
        if (position.value(QStringLiteral("action")).toString() != QStringLiteral("bid")) continue;
        const QString ticker = position.value(QStringLiteral("ticker")).toString();
        const QString side = position.value(QStringLiteral("side")).toString().trimmed().toUpper();
        const int contracts = position.value(QStringLiteral("contracts")).toInt();
        const double entry_price = position.value(QStringLiteral("price")).toDouble();
        const double entry_fee = position.value(QStringLiteral("fee_usd")).toDouble();
        const QString position_id = position.value(QStringLiteral("position_id")).toString();
        if (ticker.isEmpty() || position_id.isEmpty() || contracts < 1 ||
            (side != QStringLiteral("YES") && side != QStringLiteral("NO")) ||
            !(entry_price > 0.0 && entry_price < 1.0))
            continue;

        const QJsonObject prediction = predictions.value(ticker).toObject();
        if (prediction.isEmpty()) {
            QJsonObject row = base_row(now_ms, ticker, QStringLiteral("pass"),
                                       QString::fromLatin1(kCashoutNoBid));
            row.insert(QStringLiteral("position_id"), position_id);
            row.insert(QStringLiteral("side"), side);
            row.insert(QStringLiteral("detail"),
                       QStringLiteral("no prediction for held ticker — cannot price cashout"));
            out.append(row);
            if (cut_streak) cut_streak->remove(ticker);
            continue;
        }

        const bool yes_side = side == QStringLiteral("YES");
        const QJsonValue bid_value = prediction.value(
            yes_side ? QStringLiteral("market_yes_bid") : QStringLiteral("market_no_bid"));
        // Fail closed: mid must never proxy a sell. Missing/invalid bid → HOLD.
        if (!bid_value.isDouble()) {
            QJsonObject row = base_row(now_ms, ticker, QStringLiteral("pass"),
                                       QString::fromLatin1(kCashoutNoBid));
            row.insert(QStringLiteral("position_id"), position_id);
            row.insert(QStringLiteral("side"), side);
            row.insert(QStringLiteral("detail"),
                       QStringLiteral("held-side bid absent — refuse cashout (no mid proxy)"));
            if (prediction.value(QStringLiteral("market_yes_mid")).isDouble())
                row.insert(QStringLiteral("market_mid"),
                           prediction.value(QStringLiteral("market_yes_mid")).toDouble());
            out.append(row);
            if (cut_streak) cut_streak->remove(ticker);
            continue;
        }
        const double raw_bid = bid_value.toDouble();
        if (!(raw_bid > 0.0 && raw_bid <= 1.0)) {
            QJsonObject row = base_row(now_ms, ticker, QStringLiteral("pass"),
                                       QString::fromLatin1(kCashoutNoBid));
            row.insert(QStringLiteral("position_id"), position_id);
            row.insert(QStringLiteral("side"), side);
            out.append(row);
            if (cut_streak) cut_streak->remove(ticker);
            continue;
        }
        // Seller never assumes better than the book: floor to the cent.
        const double exit_price = std::floor(raw_bid * 100.0 + 1e-9) / 100.0;
        if (!(exit_price > 0.0 && exit_price <= 1.0)) {
            if (cut_streak) cut_streak->remove(ticker);
            continue;
        }

        const double calibrated_p = prediction.value(QStringLiteral("p_yes_full")).toDouble();
        if (!(calibrated_p > 0.0 && calibrated_p < 1.0)) {
            if (cut_streak) cut_streak->remove(ticker);
            continue;
        }
        const double held_fair = yes_side ? calibrated_p : (1.0 - calibrated_p);

        const QJsonObject features = prediction.value(QStringLiteral("features")).toObject();
        const double sqrt_minutes_left =
            features.value(QStringLiteral("sqrt_minutes_left")).toDouble();
        int seconds_left = -1;
        if (sqrt_minutes_left > 0.0) {
            const double runway =
                sqrt_minutes_left * sqrt_minutes_left * 60.0 - static_cast<double>(age_ms) / 1000.0;
            seconds_left = static_cast<int>(std::floor(runway));
        }

        const double exit_fee_per =
            KalshiEvidenceEngine::conservative_taker_fee(exit_price, 1.0);
        const double cash_out = exit_price - exit_fee_per;
        // Streak tracks the economic-cut CONDITION across ticks (AutoEngine
        // only sells once streak >= min_trigger_streak).
        int streak = 0;
        if (cut_streak) {
            if (cash_out >= held_fair + constraints.economic_margin)
                streak = cut_streak->value(ticker, 0) + 1;
            else
                streak = 0;
            cut_streak->insert(ticker, streak);
        }

        KalshiPositionExitInput input;
        input.held_side = side.toLower();
        input.entry_price = entry_price;
        input.contracts = contracts;
        input.held_side_fair = held_fair;
        input.held_side_bid = exit_price;
        input.exit_fee_per_contract = exit_fee_per;
        input.seconds_left = seconds_left;
        input.trigger_streak = streak;
        input.signal_age_seconds = static_cast<int>(age_ms / 1000);

        const auto verdict = KalshiAutoEngine::evaluate_position_exit(input, constraints);
        if (!verdict.sell) {
            // Quiet HOLD — journaling every intact hold would drown the ledger.
            continue;
        }

        const double exit_fee =
            KalshiEvidenceEngine::conservative_taker_fee(exit_price, contracts);
        const double stake = round_cents(contracts * entry_price);
        const double proceeds = round_cents(contracts * exit_price);
        const double realized =
            round_cents(proceeds - stake - entry_fee - exit_fee);

        QJsonObject sell = base_row(now_ms, ticker, QStringLiteral("sell"), verdict.reason);
        sell.insert(QStringLiteral("position_id"), position_id);
        sell.insert(QStringLiteral("side"), side);
        sell.insert(QStringLiteral("contracts"), contracts);
        sell.insert(QStringLiteral("entry_price"), entry_price);
        sell.insert(QStringLiteral("exit_price"), exit_price);
        sell.insert(QStringLiteral("held_side_fair"), held_fair);
        sell.insert(QStringLiteral("held_side_bid"), exit_price);
        sell.insert(QStringLiteral("exit_fee_usd"), exit_fee);
        sell.insert(QStringLiteral("entry_fee_usd"), entry_fee);
        sell.insert(QStringLiteral("runway_seconds"), seconds_left);
        sell.insert(QStringLiteral("trigger_streak"), streak);
        sell.insert(QStringLiteral("quote_style"), QStringLiteral("cross"));
        sell.insert(QStringLiteral("fill_model"),
                    QString::fromLatin1(KalshiBotOrders::kCashoutFillModel));
        sell.insert(QStringLiteral("fill_rule"),
                    QString::fromLatin1(KalshiBotOrders::kCashoutFillRule));
        sell.insert(QStringLiteral("signal_trusted"), signal_trusted(report));
        out.append(sell);

        QJsonObject close{
            {QStringLiteral("event"), QString::fromLatin1(kSettlementEvent)},
            {QStringLiteral("ts_ms"), static_cast<double>(now_ms)},
            {QStringLiteral("ts"), iso(now_ms)},
            {QStringLiteral("mode"), QStringLiteral("paper")},
            {QStringLiteral("live_eligible"), false},
            {QStringLiteral("position_id"), position_id},
            {QStringLiteral("ticker"), ticker},
            {QStringLiteral("side"), side},
            {QStringLiteral("contracts"), contracts},
            {QStringLiteral("price"), entry_price},
            {QStringLiteral("exit_price"), exit_price},
            {QStringLiteral("stake_usd"), stake},
            {QStringLiteral("fee_usd"), round_cents(entry_fee + exit_fee)},
            {QStringLiteral("entry_fee_usd"), entry_fee},
            {QStringLiteral("exit_fee_usd"), exit_fee},
            {QStringLiteral("resolution"), QStringLiteral("early_exit")},
            {QStringLiteral("close_reason"), verdict.reason},
            {QStringLiteral("won"), QJsonValue::Null},
            {QStringLiteral("payout_usd"), proceeds},
            {QStringLiteral("realized_pnl"), realized},
            {QStringLiteral("settlement_source"), QStringLiteral("paper_cashout")},
            {QStringLiteral("held_side_fair"), held_fair},
            {QStringLiteral("runway_seconds"), seconds_left},
            {QStringLiteral("fill_model"),
             QString::fromLatin1(KalshiBotOrders::kCashoutFillModel)}};
        copy_bid_snapshot_into(close, position);
        if (prediction.value(QStringLiteral("market_yes_mid")).isDouble()) {
            const double mid_now = prediction.value(QStringLiteral("market_yes_mid")).toDouble();
            if (mid_now > 0.0 && mid_now < 1.0)
                close.insert(QStringLiteral("market_mid_at_settle"), mid_now);
        }
        out.append(close);

        if (cut_streak) cut_streak->remove(ticker);
    }
    return out;
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
