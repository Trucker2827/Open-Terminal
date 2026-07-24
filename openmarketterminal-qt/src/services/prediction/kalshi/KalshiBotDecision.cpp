#include "services/prediction/kalshi/KalshiBotDecision.h"

#include "services/prediction/kalshi/KalshiEvidenceEngine.h"

#include <QDateTime>
#include <QSet>
#include <QTimeZone>
#include <QString>

#include <cmath>

namespace openmarketterminal::services::prediction::kalshi_ns {

namespace {

constexpr auto kDecisionEvent = "kalshi_bot_decision";
constexpr auto kSettlementEvent = "kalshi_bot_paper_settlement";

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
QJsonObject track_record(const QJsonObject& report) {
    const QJsonValue brier_full = report.value(QStringLiteral("brier_full"));
    const QJsonValue brier_market = report.value(QStringLiteral("brier_market_baseline"));
    const bool brier_available = brier_full.isDouble() && brier_market.isDouble();
    QJsonObject record{
        {QStringLiteral("resolved_contracts"), report.value(QStringLiteral("resolved_contracts")).toInt()},
        {QStringLiteral("training_samples"), report.value(QStringLiteral("training_samples")).toInt()},
        {QStringLiteral("adds_value_over_market"),
         report.value(QStringLiteral("adds_value_over_market")).toBool()},
        {QStringLiteral("brier_available"), brier_available},
        {QStringLiteral("generated_at_ms"), report.value(QStringLiteral("generated_at_ms"))}};
    if (brier_available) {
        record.insert(QStringLiteral("brier_full"), brier_full.toDouble());
        record.insert(QStringLiteral("brier_market_baseline"), brier_market.toDouble());
    }
    return record;
}

} // namespace

QJsonArray KalshiBotDecision::decide(const QJsonObject& report,
                                     const QJsonArray& open_positions,
                                     const QJsonArray& settled_positions,
                                     qint64 now_ms,
                                     const Config& config) {
    return decide(report, open_positions, settled_positions, now_ms, config, Exposure{});
}

QJsonArray KalshiBotDecision::decide(const QJsonObject& report,
                                     const QJsonArray& open_positions,
                                     const QJsonArray& settled_positions,
                                     qint64 now_ms,
                                     const Config& config,
                                     const Exposure& exposure) {
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
    // calibrator sets adds_value_over_market only once its Brier beats the
    // market baseline across its own >=100-sample gate.
    const bool trusted = report.value(QStringLiteral("adds_value_over_market")).toBool();
    const QJsonObject record = track_record(report);
    const QJsonObject predictions = report.value(QStringLiteral("predictions")).toObject();

    const auto finish = [&](QJsonObject row) {
        row.insert(QStringLiteral("signal_trusted"), trusted);
        row.insert(QStringLiteral("track_record"), record);
        return row;
    };

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

        // Paper pricing: calibrator.json carries only the mid, so the bid is a
        // limit at the mid, floored to the cent (never pay above the mid) on
        // whichever side the edge points. Live rungs price off the real book.
        const bool yes_side = edge > 0.0;
        const double raw_price = yes_side ? market_mid : 1.0 - market_mid;
        const double price = std::floor(raw_price * 100.0 + 1e-9) / 100.0;
        if (!(price > 0.0 && price < 1.0)) {
            row.insert(QStringLiteral("reason_code"), QString::fromLatin1(kMalformedPrediction));
            rows.append(finish(row));
            continue;
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
        if (session_opened_usd + all_in > config.session_budget_usd + 1e-9) {
            refuse_on_cap(kSessionBudgetBlocksBid, session_opened_usd, config.session_budget_usd);
            continue;
        }
        // PR #44's shape exactly: the book contributes STAKE (its exposure
        // rule), the new order contributes its all-in (its worst case).
        at_risk_usd = round_cents(at_risk_usd + stake);
        session_opened_usd = round_cents(session_opened_usd + all_in);

        row.insert(QStringLiteral("action"), QStringLiteral("bid"));
        // An untrusted signal does not stop the paper bid — it labels it, on
        // every single bid, so no paper record can be read as a validated one.
        // A contract whose own quote this tick's TTL pass just pulled is the
        // replace half of cancel/replace, and says so — except when the signal
        // is untrusted, which outranks everything: rung 1's rule is that EVERY
        // bid made on an unvalidated signal is labelled as one. A requote made
        // under that label still carries `requote` and `replaces_position_id`,
        // so the replace is never invisible either way.
        const QString replaces = exposure.requoted.value(ticker).toString();
        row.insert(QStringLiteral("reason_code"),
                   QString::fromLatin1(!trusted            ? kSignalUntrusted
                                       : !replaces.isEmpty() ? kRequoted
                                                             : kEdgeClearsThreshold));
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

QJsonArray KalshiBotDecision::settle_paper(const QJsonArray& open_positions,
                                           const QJsonArray& settlements,
                                           qint64 now_ms) {
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
        // No real settlement record → no row. The position stays open; there
        // is no simulated outcome anywhere in this path.
        if (found == by_ticker.constEnd()) continue;

        const QString side = position.value(QStringLiteral("side")).toString();
        const QString result = found->value(QStringLiteral("market_result")).toString();
        const int contracts = position.value(QStringLiteral("contracts")).toInt();
        const double stake = position.value(QStringLiteral("stake_usd")).toDouble();
        const double fee = position.value(QStringLiteral("fee_usd")).toDouble();
        const bool won = result == side;
        const double payout = won ? contracts * 1.0 : 0.0;
        out.append(QJsonObject{
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
            {QStringLiteral("settled_time"), found->value(QStringLiteral("settled_time"))}});
    }
    return out;
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
