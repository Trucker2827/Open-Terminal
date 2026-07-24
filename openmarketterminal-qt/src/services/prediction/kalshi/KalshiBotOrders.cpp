#include "services/prediction/kalshi/KalshiBotOrders.h"

#include "services/prediction/kalshi/KalshiEvidenceEngine.h"

#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QTimeZone>

#include <cmath>

namespace openmarketterminal::services::prediction::kalshi_ns {

namespace {

constexpr auto kDecisionEvent = "kalshi_bot_decision";
constexpr auto kSettlementEvent = "kalshi_bot_paper_settlement";

double round_cents(double dollars) { return std::round(dollars * 100.0) / 100.0; }

QString iso(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

QString str(const QJsonObject& row, const char* key) {
    return row.value(QLatin1String(key)).toString();
}

double num(const QJsonObject& row, const char* key) {
    return row.value(QLatin1String(key)).toDouble();
}

/// The replayed state of one order. `bid` is the decision row that opened it,
/// kept whole so every downstream row can carry the reasoning that produced it.
struct ReplayedOrder {
    QJsonObject bid;
    QString position_id;
    QString ticker;
    QString side;
    double limit_price = 0.0;
    double requested = 0.0;
    double filled = 0.0;
    double fill_price = 0.0;
    double fees_usd = 0.0;
    qint64 placed_ms = 0;
    qint64 ttl_ms = 0;
    QString state;
    bool settled = false;
    bool legacy = false;

    double remaining() const { return std::max(0.0, requested - filled); }
    bool canceled() const { return state == QLatin1String(KalshiBotOrders::kCanceled); }
};

/// Rows the calibrator report can be trusted for on this tick. A missing or
/// stale report yields an empty object — the caller then makes NO edge
/// judgement, rather than treating an unreadable edge as a vanished one.
QJsonObject fresh_predictions(const QJsonObject& report, qint64 now_ms,
                              const KalshiBotDecision::Config& config) {
    const auto generated_ms = static_cast<qint64>(num(report, "generated_at_ms"));
    if (generated_ms <= 0) return {};
    const qint64 age_ms = now_ms - generated_ms;
    if (age_ms >= config.max_report_age_ms || age_ms < 0) return {};
    return report.value(QStringLiteral("predictions")).toObject();
}

QJsonObject lifecycle_row(const ReplayedOrder& order, qint64 now_ms, const QString& action,
                          const char* reason_code) {
    return QJsonObject{
        {QStringLiteral("event"), QString::fromLatin1(kDecisionEvent)},
        {QStringLiteral("ts_ms"), static_cast<double>(now_ms)},
        {QStringLiteral("ts"), iso(now_ms)},
        {QStringLiteral("mode"), order.bid.value(QStringLiteral("mode"))},
        {QStringLiteral("live_eligible"), order.bid.value(QStringLiteral("live_eligible"))},
        {QStringLiteral("ticker"), order.ticker},
        {QStringLiteral("action"), action},
        {QStringLiteral("reason_code"), QString::fromLatin1(reason_code)},
        {QStringLiteral("position_id"), order.position_id},
        {QStringLiteral("side"), order.side},
        {QStringLiteral("limit_price"), order.limit_price},
        {QStringLiteral("resting_seconds"),
         order.placed_ms > 0 ? (now_ms - order.placed_ms) / 1000.0 : 0.0},
        {QStringLiteral("ttl_seconds"), static_cast<double>(order.ttl_ms) / 1000.0},
        {QStringLiteral("signal_trusted"), order.bid.value(QStringLiteral("signal_trusted"))}};
}

} // namespace

double KalshiBotOrders::order_exposure_usd(const QJsonObject& order) {
    // PR #44's rule, verbatim: filled quantity at what it actually cost, plus
    // the unfilled remainder at the price the venue could still fill it at —
    // and the remainder counts only in the states that SQL counts it in
    // (OrderFlowTools.cpp's micro-live reservation). Whitelisting rather than
    // blacklisting matters: an unknown state reads as released, not as risk,
    // only if it is spelled the same, so anything not on this list is treated
    // as still working and the sum errs high.
    const double filled = num(order, "filled_count");
    const double fill_price = num(order, "average_fill_price") > 0.0
                                  ? num(order, "average_fill_price")
                                  : num(order, "limit_price");
    const double remaining = num(order, "remaining_count");
    const QString state = str(order, "order_state");
    const bool remainder_at_risk = state != QLatin1String(kCanceled) &&
                                   state != QLatin1String(kRejected) &&
                                   state != QLatin1String(kSettled);
    const double at_risk_remainder =
        remainder_at_risk ? std::max(0.0, remaining) * num(order, "limit_price") : 0.0;
    return round_cents(std::max(0.0, filled) * fill_price + at_risk_remainder);
}

KalshiBotOrders::Book KalshiBotOrders::replay(const QJsonArray& ledger_rows) {
    QHash<QString, ReplayedOrder> orders;
    QStringList order_of_arrival;
    // Contracts known to have resolved. A settled position says so through its
    // settlement event; an order that only ever RESTED settles into nothing and
    // says so only through the cancel that retired it.
    QSet<QString> resolved;

    for (const auto& value : ledger_rows) {
        const QJsonObject row = value.toObject();
        const QString event = str(row, "event");
        const QString id = str(row, "position_id");
        if (id.isEmpty()) continue;

        if (event == QLatin1String(kSettlementEvent)) {
            auto it = orders.find(id);
            if (it != orders.end()) it->settled = true;
            resolved.insert(str(row, "ticker"));
            continue;
        }
        if (event != QLatin1String(kDecisionEvent)) continue;

        const QString action = str(row, "action");
        if (action == QStringLiteral("bid")) {
            ReplayedOrder order;
            order.bid = row;
            order.position_id = id;
            order.ticker = str(row, "ticker");
            order.side = str(row, "side");
            order.limit_price = num(row, "price");
            order.requested = num(row, "contracts");
            order.placed_ms = static_cast<qint64>(num(row, "ts_ms"));
            order.ttl_ms = static_cast<qint64>(num(row, "ttl_ms"));
            // Rung 1 wrote no order_state: those bids WERE the position under
            // its stated assumed-fill model. They are replayed as filled at
            // the price they claimed, labelled with the model that made them,
            // so a mixed ledger is readable rather than silently averaged.
            if (row.contains(QStringLiteral("order_state"))) {
                order.state = str(row, "order_state");
                if (order.state.isEmpty()) order.state = QString::fromLatin1(kResting);
            } else {
                order.legacy = true;
                order.state = QString::fromLatin1(kFilled);
                order.filled = order.requested;
                order.fill_price = order.limit_price;
                order.fees_usd = num(row, "fee_usd");
            }
            if (!orders.contains(id)) order_of_arrival.append(id);
            orders.insert(id, order);
            continue;
        }

        auto it = orders.find(id);
        if (it == orders.end()) continue;

        if (action == QStringLiteral("fill")) {
            it->filled = std::min(it->requested, it->filled + num(row, "contracts"));
            it->fill_price = num(row, "price");
            it->fees_usd += num(row, "fee_usd");
            it->state = it->remaining() > 0.0 ? QString::fromLatin1(kPartiallyFilled)
                                              : QString::fromLatin1(kFilled);
        } else if (action == QStringLiteral("cancel")) {
            // An unconfirmed cancel changes nothing: the order is still
            // working as far as anyone can prove, so it stays in the book and
            // in the exposure sum until a cancel actually confirms.
            const QString why = str(row, "reason_code");
            if (why != QLatin1String(kUnconfirmedCancel))
                it->state = QString::fromLatin1(kCanceled);
            if (why == QLatin1String(kCanceledSettled)) resolved.insert(it->ticker);
        }
    }

    Book book;
    for (const QString& id : order_of_arrival) {
        const ReplayedOrder& order = orders.value(id);
        if (order.settled) continue;

        const double remaining = order.canceled() ? 0.0 : order.remaining();
        if (order.filled > 0.0) {
            // settle_paper()'s row shape, carrying the FILLED numbers: a
            // partial fill settles for what it got, never for what it asked.
            QJsonObject position = order.bid;
            position.insert(QStringLiteral("contracts"), order.filled);
            position.insert(QStringLiteral("price"), order.fill_price);
            position.insert(QStringLiteral("stake_usd"), round_cents(order.filled * order.fill_price));
            position.insert(QStringLiteral("fee_usd"), order.fees_usd);
            position.insert(QStringLiteral("order_state"), order.state);
            position.insert(QStringLiteral("fill_model"),
                            QString::fromLatin1(order.legacy ? kLegacyFillModel : kFillModel));
            book.positions.append(position);
            book.fees_usd = round_cents(book.fees_usd + order.fees_usd);
        }
        if (remaining > 0.0) {
            QJsonObject resting = order.bid;
            resting.insert(QStringLiteral("order_state"), order.state);
            resting.insert(QStringLiteral("limit_price"), order.limit_price);
            resting.insert(QStringLiteral("filled_count"), order.filled);
            resting.insert(QStringLiteral("remaining_count"), remaining);
            resting.insert(QStringLiteral("placed_at_ms"), static_cast<double>(order.placed_ms));
            resting.insert(QStringLiteral("ttl_ms"), static_cast<double>(order.ttl_ms));
            book.resting.append(resting);
            book.resting_usd = round_cents(book.resting_usd + remaining * order.limit_price);
        }

        QJsonObject accounting{
            {QStringLiteral("filled_count"), order.filled},
            {QStringLiteral("average_fill_price"), order.fill_price},
            {QStringLiteral("remaining_count"), order.remaining()},
            {QStringLiteral("limit_price"), order.limit_price},
            {QStringLiteral("order_state"), order.state}};
        book.exposure_usd = round_cents(book.exposure_usd + order_exposure_usd(accounting));
    }
    for (const QString& ticker : resolved)
        if (!ticker.isEmpty()) book.settled.append(QJsonObject{{QStringLiteral("ticker"), ticker}});
    return book;
}

QJsonArray KalshiBotOrders::reconcile(const Book& book,
                                      const QJsonObject& report,
                                      const QJsonArray& settlements,
                                      qint64 now_ms,
                                      const KalshiBotDecision::Config& config,
                                      const Canceller& cancel) {
    const QJsonObject predictions = fresh_predictions(report, now_ms, config);
    QSet<QString> settled;
    for (const auto& value : settlements) settled.insert(str(value.toObject(), "ticker"));

    QJsonArray rows;
    for (const auto& value : book.resting) {
        const QJsonObject working = value.toObject();
        ReplayedOrder order;
        order.bid = working;
        order.position_id = str(working, "position_id");
        order.ticker = str(working, "ticker");
        order.side = str(working, "side");
        order.limit_price = num(working, "limit_price");
        order.requested = num(working, "contracts");
        order.filled = num(working, "filled_count");
        order.placed_ms = static_cast<qint64>(num(working, "placed_at_ms"));
        order.ttl_ms = static_cast<qint64>(num(working, "ttl_ms"));
        const double remaining = num(working, "remaining_count");
        if (remaining <= 0.0) continue;

        const QJsonObject prediction = predictions.value(order.ticker).toObject();
        const double market_mid = num(prediction, "market_yes_mid");
        const bool quoted = market_mid > 0.0 && market_mid < 1.0;
        const double side_price =
            order.side == QStringLiteral("YES") ? market_mid : 1.0 - market_mid;
        const bool market_settled = settled.contains(order.ticker);

        // --- did it fill? ---------------------------------------------------
        // Checked before the TTL on purpose: an order the market has traded
        // through is an order that filled, and a cancel cannot un-fill it.
        // Checked AFTER settlement for the opposite reason: the report's
        // snapshot goes stale under a fresh generation stamp, so a resolved
        // contract can still be quoted at a price that looks marketable — and
        // a resolved contract cannot fill. (Rung 1 closed the same trap with
        // CONTRACT_SETTLED.)
        if (!market_settled && quoted && side_price <= order.limit_price + 1e-9) {
            const double fee =
                KalshiEvidenceEngine::conservative_taker_fee(order.limit_price, remaining);
            QJsonObject row = lifecycle_row(order, now_ms, QStringLiteral("fill"), kFilledAtLimit);
            row.insert(QStringLiteral("contracts"), remaining);
            row.insert(QStringLiteral("price"), order.limit_price);
            row.insert(QStringLiteral("stake_usd"), round_cents(remaining * order.limit_price));
            row.insert(QStringLiteral("fee_usd"), fee);
            row.insert(QStringLiteral("observed_mid"), market_mid);
            row.insert(QStringLiteral("order_state"), QString::fromLatin1(kFilled));
            row.insert(QStringLiteral("fill_model"), QString::fromLatin1(kFillModel));
            row.insert(QStringLiteral("fill_rule"), QString::fromLatin1(kFillRule));
            rows.append(row);
            continue;
        }

        // --- otherwise: is there a reason to pull it? ------------------------
        const char* reason = nullptr;
        if (market_settled) {
            // The market resolved with the quote still working: it never
            // filled, so there is no position — only an order to retire.
            reason = kCanceledSettled;
        } else if (order.ttl_ms > 0 && order.placed_ms > 0 &&
                   now_ms - order.placed_ms >= order.ttl_ms) {
            reason = kCanceledTtl;
        } else if (quoted) {
            // The edge is RE-DERIVED from the fresh report, never remembered.
            // A flip (the market crossed the calibrated probability) and a
            // collapse (below the threshold that justified the quote) are the
            // same failure: the reason this order exists is gone.
            const double calibrated_p = num(prediction, "p_yes_full");
            const double edge = calibrated_p - market_mid;
            const double side_edge = order.side == QStringLiteral("YES") ? edge : -edge;
            if (calibrated_p > 0.0 && calibrated_p < 1.0 && side_edge < config.edge_threshold)
                reason = kCanceledEdgeGone;
        }
        if (reason == nullptr) continue;

        const bool confirmed = cancel && cancel(working, QString::fromLatin1(reason));
        QJsonObject row = lifecycle_row(order, now_ms, QStringLiteral("cancel"),
                                        confirmed ? reason : kUnconfirmedCancel);
        row.insert(QStringLiteral("contracts"), remaining);
        row.insert(QStringLiteral("released_usd"),
                   confirmed ? round_cents(remaining * order.limit_price) : 0.0);
        row.insert(QStringLiteral("order_state"),
                   QString::fromLatin1(confirmed ? kCanceled : kResting));
        if (quoted) row.insert(QStringLiteral("observed_mid"), market_mid);
        if (!confirmed) {
            // Unconfirmed means unknown, and unknown counts as risk: the order
            // stays in the book, keeps its exposure, and is retried next tick.
            row.insert(QStringLiteral("attempted_reason_code"), QString::fromLatin1(reason));
            row.insert(QStringLiteral("still_at_risk_usd"), round_cents(remaining * order.limit_price));
        }
        rows.append(row);
    }
    return rows;
}

QJsonObject KalshiBotOrders::requotable(const QJsonArray& lifecycle_rows) {
    QJsonObject out;
    for (const auto& value : lifecycle_rows) {
        const QJsonObject row = value.toObject();
        if (str(row, "action") != QStringLiteral("cancel")) continue;
        if (str(row, "reason_code") != QLatin1String(kCanceledTtl)) continue;
        out.insert(str(row, "ticker"), str(row, "position_id"));
    }
    return out;
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
