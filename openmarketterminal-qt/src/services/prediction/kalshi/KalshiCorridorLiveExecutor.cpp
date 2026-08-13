#include "services/prediction/kalshi/KalshiCorridorLiveExecutor.h"

#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace openmarketterminal::services::prediction::kalshi_ns {

namespace {

using Executor = KalshiCorridorLiveExecutor;

bool valid_leg(const Executor::Leg& leg, const QString& outcome) {
    return !leg.ticker.isEmpty() && leg.outcome == outcome && leg.ask_price > 0.0 &&
           leg.ask_price < 1.0 && leg.unwind_bid > 0.0 && leg.unwind_bid < 1.0 &&
           leg.unwind_bid <= leg.ask_price && leg.depth_at_ask > 0;
}

QJsonObject leg_json(const Executor::Leg& leg) {
    return {{QStringLiteral("ticker"), leg.ticker},
            {QStringLiteral("outcome"), leg.outcome},
            {QStringLiteral("ask_price"), leg.ask_price},
            {QStringLiteral("unwind_bid"), leg.unwind_bid},
            {QStringLiteral("depth_at_ask"), leg.depth_at_ask}};
}

Executor::Leg leg_from_json(const QJsonObject& object) {
    return {object.value(QStringLiteral("ticker")).toString(),
            object.value(QStringLiteral("outcome")).toString(),
            object.value(QStringLiteral("ask_price")).toDouble(),
            object.value(QStringLiteral("unwind_bid")).toDouble(),
            object.value(QStringLiteral("depth_at_ask")).toInt()};
}

} // namespace

KalshiCorridorLiveExecutor KalshiCorridorLiveExecutor::create(const Intent& intent,
                                                               QString* error) {
    KalshiCorridorLiveExecutor out;
    const QRegularExpression id_re(QStringLiteral("^[A-Za-z0-9_-]{8,64}$"));
    if (!id_re.match(intent.bundle_id).hasMatch()) {
        out.fail(QStringLiteral("bundle_id must be 8-64 ASCII letters, digits, '_' or '-'"), error);
        return out;
    }
    if (!valid_leg(intent.lower_yes, QStringLiteral("YES")) ||
        !valid_leg(intent.higher_no, QStringLiteral("NO")) ||
        intent.lower_yes.ticker == intent.higher_no.ticker) {
        out.fail(QStringLiteral("corridor legs are invalid or not distinct lower-YES/higher-NO legs"),
                 error);
        return out;
    }
    if (intent.bundles < 1 || intent.bundles > intent.lower_yes.depth_at_ask ||
        intent.bundles > intent.higher_no.depth_at_ask) {
        out.fail(QStringLiteral("bundle quantity exceeds certified ask depth"), error);
        return out;
    }
    const double acquisition = intent.bundles *
                               (intent.lower_yes.ask_price + intent.higher_no.ask_price);
    if (intent.tier == ExecutionTier::ProductionLive) {
        out.fail(QStringLiteral("production-live corridor authority ($20-$100 per leg) is unavailable"),
                 error);
        return out;
    }
    const double numbers[]{intent.lower_yes_fee_usd, intent.higher_no_fee_usd,
                           intent.lower_yes_buffer_usd, intent.higher_no_buffer_usd};
    for (const double number : numbers) {
        if (!std::isfinite(number) || number < 0.0) {
            out.fail(QStringLiteral("micro-live fees and buffers must be finite and non-negative"),
                     error);
            return out;
        }
    }
    if (!std::isfinite(intent.max_all_in_per_leg_usd) ||
        intent.max_all_in_per_leg_usd <= 0.0 ||
        intent.max_all_in_per_leg_usd > kMicroLiveMaxAllInPerLegUsd + 1e-9) {
        out.fail(QStringLiteral("micro-live corridor seal exceeds the hard $2 per-leg ceiling"), error);
        return out;
    }
    const double lower_all_in = intent.bundles * intent.lower_yes.ask_price +
                                intent.lower_yes_fee_usd + intent.lower_yes_buffer_usd;
    const double higher_all_in = intent.bundles * intent.higher_no.ask_price +
                                 intent.higher_no_fee_usd + intent.higher_no_buffer_usd;
    if (!std::isfinite(acquisition) ||
        lower_all_in > intent.max_all_in_per_leg_usd + 1e-9 ||
        higher_all_in > intent.max_all_in_per_leg_usd + 1e-9 ||
        lower_all_in + higher_all_in > kMicroLiveMaxPairCostUsd + 1e-9) {
        out.fail(QStringLiteral("one or both legs exceed the authorized $2 all-in per-leg cost"),
                 error);
        return out;
    }

    out.intent_ = intent;
    // Execute the thinner displayed ask first. If it cannot be obtained, the
    // deeper leg is never touched. A tie is deterministic for idempotency.
    const bool lower_first = intent.lower_yes.depth_at_ask <= intent.higher_no.depth_at_ask;
    out.first_ = lower_first ? intent.lower_yes : intent.higher_no;
    out.second_ = lower_first ? intent.higher_no : intent.lower_yes;
    out.phase_ = Phase::NeedFirstSubmit;
    if (error) error->clear();
    return out;
}

QString KalshiCorridorLiveExecutor::production_refusal(ExecutionTier tier) {
    return tier == ExecutionTier::MicroLive
               ? QStringLiteral("BTC corridor micro-live dispatch requires its runtime gate (maximum $2 all-in per leg)")
               : QStringLiteral("BTC corridor production-live dispatch ($20-$100 per leg) is not armed in this release");
}

QString KalshiCorridorLiveExecutor::first_client_id() const {
    static const QUuid ns(QStringLiteral("{af759935-fb67-4b6d-b5a2-dc19638f7131}"));
    return QUuid::createUuidV5(ns, (intent_.bundle_id + QStringLiteral("|first")).toUtf8())
        .toString(QUuid::WithoutBraces);
}

QString KalshiCorridorLiveExecutor::second_client_id() const {
    static const QUuid ns(QStringLiteral("{af759935-fb67-4b6d-b5a2-dc19638f7131}"));
    return QUuid::createUuidV5(ns, (intent_.bundle_id + QStringLiteral("|second")).toUtf8())
        .toString(QUuid::WithoutBraces);
}

QString KalshiCorridorLiveExecutor::unwind_client_id() const {
    static const QUuid ns(QStringLiteral("{af759935-fb67-4b6d-b5a2-dc19638f7131}"));
    return QUuid::createUuidV5(ns, (intent_.bundle_id + QStringLiteral("|unwind")).toUtf8())
        .toString(QUuid::WithoutBraces);
}

KalshiCorridorLiveExecutor::Action KalshiCorridorLiveExecutor::next_action() {
    Action action;
    switch (phase_) {
    case Phase::NeedFirstSubmit:
        action = {ActionKind::SubmitFak, first_client_id(), {}, first_.ticker, first_.outcome,
                  QStringLiteral("BUY"), first_.ask_price, intent_.bundles, false};
        phase_ = Phase::AwaitFirst;
        break;
    case Phase::NeedFirstCancel:
        action = {ActionKind::Cancel, {}, first_order_id_, {}, {}, {}, 0.0, 0, false};
        phase_ = Phase::AwaitFirstCancel;
        break;
    case Phase::NeedSecondSubmit:
        action = {ActionKind::SubmitFak, second_client_id(), {}, second_.ticker, second_.outcome,
                  QStringLiteral("BUY"), second_.ask_price, first_filled_, false};
        phase_ = Phase::AwaitSecond;
        break;
    case Phase::NeedSecondCancel:
        action = {ActionKind::Cancel, {}, second_order_id_, {}, {}, {}, 0.0, 0, false};
        phase_ = Phase::AwaitSecondCancel;
        break;
    case Phase::NeedUnwindSubmit:
        action = {ActionKind::SubmitFak, unwind_client_id(), {}, first_.ticker, first_.outcome,
                  QStringLiteral("SELL"), first_.unwind_bid,
                  first_filled_ - second_filled_, true};
        phase_ = Phase::AwaitUnwind;
        break;
    case Phase::NeedUnwindCancel:
        action = {ActionKind::Cancel, {}, unwind_order_id_, {}, {}, {}, 0.0, 0, false};
        phase_ = Phase::AwaitUnwindCancel;
        break;
    default:
        break;
    }
    return action;
}

bool KalshiCorridorLiveExecutor::fail(const QString& why, QString* error) {
    phase_ = Phase::HaltedUnsafe;
    reason_ = why;
    if (error) *error = why;
    return false;
}

void KalshiCorridorLiveExecutor::after_first_terminal() {
    phase_ = first_filled_ == 0 ? Phase::Complete : Phase::NeedSecondSubmit;
    reason_ = first_filled_ == 0 ? QStringLiteral("first leg did not fill; second leg was not sent")
                                 : QString();
}

void KalshiCorridorLiveExecutor::after_second_terminal() {
    if (second_filled_ == first_filled_) {
        phase_ = Phase::Complete;
        reason_ = QStringLiteral("both corridor legs matched exactly");
    } else {
        phase_ = Phase::NeedUnwindSubmit;
        reason_ = QStringLiteral("second leg underfilled; unwind unmatched first-leg contracts");
    }
}

void KalshiCorridorLiveExecutor::after_unwind_terminal() {
    const int excess = first_filled_ - second_filled_;
    if (unwind_filled_ == excess) {
        phase_ = Phase::Complete;
        reason_ = QStringLiteral("unmatched first-leg exposure was fully unwound");
    } else {
        phase_ = Phase::HaltedUnsafe;
        reason_ = QStringLiteral("automatic unwind was incomplete; operator intervention required");
    }
}

bool KalshiCorridorLiveExecutor::apply_order_report(const OrderReport& report, QString* error) {
    QString expected;
    int requested = 0;
    enum class Which { First, Second, Unwind } which;
    if (phase_ == Phase::AwaitFirst) {
        expected = first_client_id();
        requested = intent_.bundles;
        which = Which::First;
    } else if (phase_ == Phase::AwaitSecond) {
        expected = second_client_id();
        requested = first_filled_;
        which = Which::Second;
    } else if (phase_ == Phase::AwaitUnwind) {
        expected = unwind_client_id();
        requested = first_filled_ - second_filled_;
        which = Which::Unwind;
    } else {
        return fail(QStringLiteral("order report arrived in a phase that is not awaiting an order"), error);
    }
    if (report.indeterminate)
        return fail(QStringLiteral(
            "submission outcome is unknown; reconcile the client order id before any further corridor action"),
            error);
    if (report.client_order_id != expected || report.cumulative_filled < 0 ||
        report.cumulative_filled > requested ||
        (report.cumulative_filled > 0 && !report.accepted))
        return fail(QStringLiteral("venue order report is inconsistent with the pending action"), error);

    if (which == Which::First) {
        first_order_id_ = report.order_id;
        first_filled_ = report.cumulative_filled;
    } else if (which == Which::Second) {
        second_order_id_ = report.order_id;
        second_filled_ = report.cumulative_filled;
    } else {
        unwind_order_id_ = report.order_id;
        unwind_filled_ = report.cumulative_filled;
    }

    if (!report.accepted) {
        if (which == Which::First) after_first_terminal();
        else if (which == Which::Second) after_second_terminal();
        else after_unwind_terminal();
        if (error) error->clear();
        return true;
    }
    if (!report.terminal) {
        if (report.order_id.isEmpty())
            return fail(QStringLiteral("working order has no venue order id to cancel"), error);
        phase_ = which == Which::First ? Phase::NeedFirstCancel
                                      : which == Which::Second ? Phase::NeedSecondCancel
                                                               : Phase::NeedUnwindCancel;
    } else if (which == Which::First) {
        after_first_terminal();
    } else if (which == Which::Second) {
        after_second_terminal();
    } else {
        after_unwind_terminal();
    }
    if (error) error->clear();
    return true;
}

bool KalshiCorridorLiveExecutor::apply_cancel_report(const CancelReport& report, QString* error) {
    int requested = 0;
    enum class Which { First, Second, Unwind } which;
    QString expected_order;
    if (phase_ == Phase::AwaitFirstCancel) {
        requested = intent_.bundles;
        which = Which::First;
        expected_order = first_order_id_;
    } else if (phase_ == Phase::AwaitSecondCancel) {
        requested = first_filled_;
        which = Which::Second;
        expected_order = second_order_id_;
    } else if (phase_ == Phase::AwaitUnwindCancel) {
        requested = first_filled_ - second_filled_;
        which = Which::Unwind;
        expected_order = unwind_order_id_;
    } else {
        return fail(QStringLiteral("cancel report arrived in a phase that is not awaiting cancellation"),
                    error);
    }
    if (report.indeterminate)
        return fail(QStringLiteral(
            "cancel/final-fill outcome is unknown; reconcile the venue order before any dependent corridor action"),
            error);
    if (report.order_id != expected_order || report.cumulative_filled < 0 ||
        report.cumulative_filled > requested)
        return fail(QStringLiteral("venue cancel report is inconsistent with the working order"), error);
    if (!report.confirmed) {
        phase_ = which == Which::First ? Phase::NeedFirstCancel
                                      : which == Which::Second ? Phase::NeedSecondCancel
                                                               : Phase::NeedUnwindCancel;
        reason_ = QStringLiteral("cancel unconfirmed; no dependent order may be sent");
        if (error) error->clear();
        return true;
    }

    if (which == Which::First) {
        first_filled_ = report.cumulative_filled;
        after_first_terminal();
    } else if (which == Which::Second) {
        second_filled_ = report.cumulative_filled;
        after_second_terminal();
    } else {
        unwind_filled_ = report.cumulative_filled;
        after_unwind_terminal();
    }
    if (error) error->clear();
    return true;
}

KalshiCorridorLiveExecutor::Snapshot KalshiCorridorLiveExecutor::snapshot() const {
    Snapshot out;
    out.phase = phase_;
    out.bundle_id = intent_.bundle_id;
    out.first_ticker = first_.ticker;
    out.second_ticker = second_.ticker;
    out.requested_bundles = intent_.bundles;
    out.first_filled = first_filled_;
    out.second_filled = second_filled_;
    out.unwind_filled = unwind_filled_;
    out.matched_bundles = std::min(first_filled_, second_filled_);
    out.unmatched_first_leg = std::max(0, first_filled_ - second_filled_ - unwind_filled_);
    out.reason = reason_;
    return out;
}

QJsonObject KalshiCorridorLiveExecutor::to_json() const {
    return {{QStringLiteral("schema"), 1},
            {QStringLiteral("intent"),
             QJsonObject{{QStringLiteral("bundle_id"), intent_.bundle_id},
                         {QStringLiteral("tier"),
                          intent_.tier == ExecutionTier::MicroLive ? QStringLiteral("micro_live")
                                                                   : QStringLiteral("production_live")},
                         {QStringLiteral("lower_yes"), leg_json(intent_.lower_yes)},
                         {QStringLiteral("higher_no"), leg_json(intent_.higher_no)},
                         {QStringLiteral("bundles"), intent_.bundles},
                         {QStringLiteral("lower_yes_fee_usd"), intent_.lower_yes_fee_usd},
                         {QStringLiteral("higher_no_fee_usd"), intent_.higher_no_fee_usd},
                         {QStringLiteral("lower_yes_buffer_usd"), intent_.lower_yes_buffer_usd},
                         {QStringLiteral("higher_no_buffer_usd"), intent_.higher_no_buffer_usd},
                         {QStringLiteral("max_all_in_per_leg_usd"),
                          intent_.max_all_in_per_leg_usd}}},
            {QStringLiteral("first"), leg_json(first_)},
            {QStringLiteral("second"), leg_json(second_)},
            {QStringLiteral("phase"), static_cast<int>(phase_)},
            {QStringLiteral("reason"), reason_},
            {QStringLiteral("first_order_id"), first_order_id_},
            {QStringLiteral("second_order_id"), second_order_id_},
            {QStringLiteral("unwind_order_id"), unwind_order_id_},
            {QStringLiteral("first_filled"), first_filled_},
            {QStringLiteral("second_filled"), second_filled_},
            {QStringLiteral("unwind_filled"), unwind_filled_}};
}

KalshiCorridorLiveExecutor KalshiCorridorLiveExecutor::from_json(
    const QJsonObject& object, QString* error) {
    KalshiCorridorLiveExecutor out;
    if (object.value(QStringLiteral("schema")).toInt() != 1)
        return out.fail(QStringLiteral("unsupported corridor executor state schema"), error), out;
    const QJsonObject raw = object.value(QStringLiteral("intent")).toObject();
    Intent intent;
    intent.bundle_id = raw.value(QStringLiteral("bundle_id")).toString();
    intent.tier = raw.value(QStringLiteral("tier")).toString() == QLatin1String("micro_live")
                      ? ExecutionTier::MicroLive
                      : ExecutionTier::ProductionLive;
    intent.lower_yes = leg_from_json(raw.value(QStringLiteral("lower_yes")).toObject());
    intent.higher_no = leg_from_json(raw.value(QStringLiteral("higher_no")).toObject());
    intent.bundles = raw.value(QStringLiteral("bundles")).toInt();
    intent.lower_yes_fee_usd = raw.value(QStringLiteral("lower_yes_fee_usd")).toDouble();
    intent.higher_no_fee_usd = raw.value(QStringLiteral("higher_no_fee_usd")).toDouble();
    intent.lower_yes_buffer_usd = raw.value(QStringLiteral("lower_yes_buffer_usd")).toDouble();
    intent.higher_no_buffer_usd = raw.value(QStringLiteral("higher_no_buffer_usd")).toDouble();
    intent.max_all_in_per_leg_usd =
        raw.value(QStringLiteral("max_all_in_per_leg_usd")).toDouble();
    QString create_error;
    out = create(intent, &create_error);
    if (!create_error.isEmpty()) return out.fail(create_error, error), out;
    const int phase = object.value(QStringLiteral("phase")).toInt(-1);
    if (phase < static_cast<int>(Phase::Invalid) || phase > static_cast<int>(Phase::HaltedUnsafe))
        return out.fail(QStringLiteral("corridor executor state has an invalid phase"), error), out;
    out.first_ = leg_from_json(object.value(QStringLiteral("first")).toObject());
    out.second_ = leg_from_json(object.value(QStringLiteral("second")).toObject());
    const bool restored_order_matches =
        (out.first_.ticker == intent.lower_yes.ticker &&
         out.first_.outcome == intent.lower_yes.outcome &&
         out.second_.ticker == intent.higher_no.ticker &&
         out.second_.outcome == intent.higher_no.outcome) ||
        (out.first_.ticker == intent.higher_no.ticker &&
         out.first_.outcome == intent.higher_no.outcome &&
         out.second_.ticker == intent.lower_yes.ticker &&
         out.second_.outcome == intent.lower_yes.outcome);
    if (!restored_order_matches ||
        out.first_.ask_price <= 0.0 || out.second_.ask_price <= 0.0 ||
        out.first_.unwind_bid <= 0.0 || out.second_.unwind_bid <= 0.0 ||
        out.first_.depth_at_ask <= 0 || out.second_.depth_at_ask <= 0)
        return out.fail(QStringLiteral("corridor executor state carries invalid legs"), error), out;
    out.phase_ = static_cast<Phase>(phase);
    out.reason_ = object.value(QStringLiteral("reason")).toString();
    out.first_order_id_ = object.value(QStringLiteral("first_order_id")).toString();
    out.second_order_id_ = object.value(QStringLiteral("second_order_id")).toString();
    out.unwind_order_id_ = object.value(QStringLiteral("unwind_order_id")).toString();
    out.first_filled_ = object.value(QStringLiteral("first_filled")).toInt(-1);
    out.second_filled_ = object.value(QStringLiteral("second_filled")).toInt(-1);
    out.unwind_filled_ = object.value(QStringLiteral("unwind_filled")).toInt(-1);
    if (out.first_filled_ < 0 || out.first_filled_ > intent.bundles ||
        out.second_filled_ < 0 || out.second_filled_ > out.first_filled_ ||
        out.unwind_filled_ < 0 ||
        out.unwind_filled_ > out.first_filled_ - out.second_filled_)
        return out.fail(QStringLiteral("corridor executor state carries impossible fill counts"), error), out;
    if (error) error->clear();
    return out;
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
