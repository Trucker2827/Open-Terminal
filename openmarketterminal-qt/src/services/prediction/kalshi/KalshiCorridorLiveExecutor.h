#pragma once

#include <QJsonObject>
#include <QString>

namespace openmarketterminal::services::prediction::kalshi_ns {

/// Deterministic two-leg execution protocol for a BTC threshold corridor.
///
/// This class deliberately has no exchange adapter, HTTP client, signal, or
/// callback. It can describe exactly one next effect, and it advances only
/// after the caller supplies the venue's cumulative fill state. That makes the
/// partial-fill policy testable independently of the exchange transport.
///
/// Micro-live dispatch is available only behind the separate corridor gate and
/// the human-owned live session. Production dispatch is HARD-OFF in this
/// release. See production_refusal().
class KalshiCorridorLiveExecutor {
  public:
    enum class ExecutionTier { MicroLive, ProductionLive };

    // These are separate authority domains, not configurable numbers. A
    // caller cannot turn a micro-live seal into a production-live seal by
    // changing its requested cost.
    static constexpr double kMicroLiveMaxAllInPerLegUsd = 2.0;
    static constexpr double kMicroLiveMaxPairCostUsd = 4.0;
    static constexpr double kProductionLiveMinPerLegUsd = 20.0;
    static constexpr double kProductionLiveMaxPerLegUsd = 100.0;
    static constexpr bool kMicroLiveDispatchArmed = true;
    static constexpr bool kProductionLiveDispatchArmed = false;

    struct Leg {
        QString ticker;
        QString outcome; // YES or NO
        double ask_price = 0.0;
        double unwind_bid = 0.0;
        int depth_at_ask = 0;
    };

    struct Intent {
        QString bundle_id;
        ExecutionTier tier = ExecutionTier::MicroLive;
        Leg lower_yes;
        Leg higher_no;
        int bundles = 0;
        double lower_yes_fee_usd = 0.0;
        double higher_no_fee_usd = 0.0;
        double lower_yes_buffer_usd = 0.0;
        double higher_no_buffer_usd = 0.0;
        double max_all_in_per_leg_usd = 0.0;
    };

    enum class Phase {
        Invalid,
        NeedFirstSubmit,
        AwaitFirst,
        NeedFirstCancel,
        AwaitFirstCancel,
        NeedSecondSubmit,
        AwaitSecond,
        NeedSecondCancel,
        AwaitSecondCancel,
        NeedUnwindSubmit,
        AwaitUnwind,
        NeedUnwindCancel,
        AwaitUnwindCancel,
        Complete,
        HaltedUnsafe,
    };

    enum class ActionKind { None, SubmitFak, Cancel };

    struct Action {
        ActionKind kind = ActionKind::None;
        QString client_order_id;
        QString order_id;
        QString ticker;
        QString outcome;
        QString side; // BUY or SELL
        double limit_price = 0.0;
        int count = 0;
        bool reduce_only = false;
    };

    /// Cumulative venue state for one submitted order. A non-terminal report
    /// causes an explicit cancel before any dependent leg is sized.
    struct OrderReport {
        QString client_order_id;
        QString order_id;
        bool accepted = false;
        bool terminal = false;
        /// A local timeout is not a venue rejection. It freezes the protocol:
        /// no dependent leg or replacement may be submitted until an operator
        /// reconciles the exchange-side client id.
        bool indeterminate = false;
        int cumulative_filled = 0;
        double average_fill_price = 0.0;
        QString error;
    };

    /// Cancellation must include the latest cumulative fill count. Without a
    /// confirmed cancel the protocol retries cancellation and never submits a
    /// dependent leg against an unknowable first-leg quantity.
    struct CancelReport {
        QString order_id;
        bool confirmed = false;
        /// Cancellation acknowledgement without a fresh venue order read is
        /// not enough to size a dependent leg. Any timeout/parse ambiguity
        /// freezes the bundle for operator reconciliation.
        bool indeterminate = false;
        int cumulative_filled = 0;
        QString error;
    };

    struct Snapshot {
        Phase phase = Phase::Invalid;
        QString bundle_id;
        QString first_ticker;
        QString second_ticker;
        int requested_bundles = 0;
        int first_filled = 0;
        int second_filled = 0;
        int unwind_filled = 0;
        int matched_bundles = 0;
        int unmatched_first_leg = 0;
        QString reason;
    };

    static KalshiCorridorLiveExecutor create(const Intent& intent, QString* error = nullptr);

    /// Stable refusal for every production caller in this release. There is no
    /// runtime flag, environment variable, CLI option, or sealed file that can
    /// override it.
    static QString production_refusal(ExecutionTier tier);

    Action next_action();
    bool apply_order_report(const OrderReport& report, QString* error = nullptr);
    bool apply_cancel_report(const CancelReport& report, QString* error = nullptr);
    Snapshot snapshot() const;

    /// Durable protocol state. The caller persists this before and after every
    /// irreversible exchange effect. Restore validates all invariants and
    /// refuses an invalid or future schema rather than inventing a phase.
    QJsonObject to_json() const;
    static KalshiCorridorLiveExecutor from_json(const QJsonObject& object,
                                                QString* error = nullptr);

  private:
    Intent intent_;
    Leg first_;
    Leg second_;
    Phase phase_ = Phase::Invalid;
    QString reason_;
    QString first_order_id_;
    QString second_order_id_;
    QString unwind_order_id_;
    int first_filled_ = 0;
    int second_filled_ = 0;
    int unwind_filled_ = 0;

    QString first_client_id() const;
    QString second_client_id() const;
    QString unwind_client_id() const;
    void after_first_terminal();
    void after_second_terminal();
    void after_unwind_terminal();
    bool fail(const QString& why, QString* error);
};

static_assert(KalshiCorridorLiveExecutor::kMicroLiveDispatchArmed,
              "corridor micro-live execution is intentionally available behind its runtime gate");
static_assert(!KalshiCorridorLiveExecutor::kProductionLiveDispatchArmed,
              "corridor production-live execution must remain unavailable");

} // namespace openmarketterminal::services::prediction::kalshi_ns
