#include "services/prediction/kalshi/KalshiCorridorLiveExecutor.h"

#include <QtTest>

using openmarketterminal::services::prediction::kalshi_ns::KalshiCorridorLiveExecutor;
using Executor = KalshiCorridorLiveExecutor;

namespace {

Executor::Intent intent() {
    return {QStringLiteral("bundle-12345678"),
            Executor::ExecutionTier::MicroLive,
            {QStringLiteral("KXBTCD-E-T64000"), QStringLiteral("YES"), 0.42, 0.40, 3},
            {QStringLiteral("KXBTCD-E-T65000"), QStringLiteral("NO"), 0.53, 0.51, 8},
            2,
            0.01,
            0.01,
            0.01,
            0.01,
            2.00};
}

Executor::OrderReport order(const Executor::Action& action, int filled, bool terminal = true,
                            bool accepted = true) {
    return {action.client_order_id, QStringLiteral("venue-%1").arg(action.client_order_id),
            accepted, terminal, false, filled, action.limit_price, {}};
}

} // namespace

class TstKalshiCorridorLiveExecutor : public QObject {
    Q_OBJECT
  private slots:
    void tier_arming_is_explicit_and_asymmetric() {
        QVERIFY(Executor::kMicroLiveDispatchArmed);
        QVERIFY(!Executor::kProductionLiveDispatchArmed);
        QVERIFY(Executor::production_refusal(Executor::ExecutionTier::MicroLive)
                    .contains(QStringLiteral("$2 all-in per leg")));
        QVERIFY(Executor::production_refusal(Executor::ExecutionTier::ProductionLive)
                    .contains(QStringLiteral("$20-$100")));
    }

    void thinner_leg_executes_first_and_ids_are_idempotent() {
        QString error;
        auto executor = Executor::create(intent(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto first = executor.next_action();
        QCOMPARE(first.kind, Executor::ActionKind::SubmitFak);
        QCOMPARE(first.ticker, QStringLiteral("KXBTCD-E-T64000"));
        QCOMPARE(first.outcome, QStringLiteral("YES"));
        QCOMPARE(first.side, QStringLiteral("BUY"));
        QCOMPARE(first.count, 2);
        QVERIFY(!QUuid(first.client_order_id).isNull());
        const auto replay = Executor::create(intent()).next_action();
        QCOMPARE(replay.client_order_id, first.client_order_id);
        QCOMPARE(executor.next_action().kind, Executor::ActionKind::None);
    }

    void first_leg_zero_fill_never_sends_second() {
        auto executor = Executor::create(intent());
        const auto first = executor.next_action();
        QVERIFY(executor.apply_order_report(order(first, 0)));
        QCOMPARE(executor.snapshot().phase, Executor::Phase::Complete);
        QCOMPARE(executor.snapshot().matched_bundles, 0);
        QCOMPARE(executor.next_action().kind, Executor::ActionKind::None);
    }

    void partial_first_is_cancelled_before_second_is_sized() {
        auto executor = Executor::create(intent());
        const auto first = executor.next_action();
        const auto first_report = order(first, 1, false);
        QVERIFY(executor.apply_order_report(first_report));
        const auto cancel = executor.next_action();
        QCOMPARE(cancel.kind, Executor::ActionKind::Cancel);
        QCOMPARE(cancel.order_id, first_report.order_id);
        QVERIFY(executor.apply_cancel_report({cancel.order_id, false, false, 1, {}}));
        QCOMPARE(executor.next_action().kind, Executor::ActionKind::Cancel);
        // Re-issue the deterministic cancellation, then confirm it. No second
        // leg existed while the first leg's final fill count was unknown.
        QVERIFY(executor.apply_cancel_report({cancel.order_id, true, false, 1, {}}));
        const auto second = executor.next_action();
        QCOMPARE(second.kind, Executor::ActionKind::SubmitFak);
        QCOMPARE(second.count, 1);
        QCOMPARE(second.ticker, QStringLiteral("KXBTCD-E-T65000"));
    }

    void exact_two_leg_fill_completes_without_unwind() {
        auto executor = Executor::create(intent());
        const auto first = executor.next_action();
        QVERIFY(executor.apply_order_report(order(first, 2)));
        const auto second = executor.next_action();
        QCOMPARE(second.count, 2);
        QVERIFY(executor.apply_order_report(order(second, 2)));
        QCOMPARE(executor.snapshot().phase, Executor::Phase::Complete);
        QCOMPARE(executor.snapshot().matched_bundles, 2);
        QCOMPARE(executor.snapshot().unmatched_first_leg, 0);
    }

    void second_leg_underfill_unwinds_only_the_excess() {
        auto executor = Executor::create(intent());
        const auto first = executor.next_action();
        QVERIFY(executor.apply_order_report(order(first, 2)));
        const auto second = executor.next_action();
        QVERIFY(executor.apply_order_report(order(second, 1)));
        const auto unwind = executor.next_action();
        QCOMPARE(unwind.kind, Executor::ActionKind::SubmitFak);
        QCOMPARE(unwind.side, QStringLiteral("SELL"));
        QVERIFY(unwind.reduce_only);
        QCOMPARE(unwind.ticker, first.ticker);
        QCOMPARE(unwind.outcome, first.outcome);
        QCOMPARE(unwind.count, 1);
        QVERIFY(executor.apply_order_report(order(unwind, 1)));
        QCOMPARE(executor.snapshot().phase, Executor::Phase::Complete);
        QCOMPARE(executor.snapshot().matched_bundles, 1);
        QCOMPARE(executor.snapshot().unmatched_first_leg, 0);
    }

    void incomplete_unwind_halts_unsafe_and_reports_exposure() {
        auto executor = Executor::create(intent());
        const auto first = executor.next_action();
        QVERIFY(executor.apply_order_report(order(first, 2)));
        const auto second = executor.next_action();
        QVERIFY(executor.apply_order_report(order(second, 0)));
        const auto unwind = executor.next_action();
        QVERIFY(executor.apply_order_report(order(unwind, 1)));
        QCOMPARE(executor.snapshot().phase, Executor::Phase::HaltedUnsafe);
        QCOMPARE(executor.snapshot().unmatched_first_leg, 1);
        QVERIFY(executor.snapshot().reason.contains(QStringLiteral("operator intervention")));
        QCOMPARE(executor.next_action().kind, Executor::ActionKind::None);
    }

    void invalid_or_over_budget_intent_never_emits_an_action() {
        auto bad = intent();
        bad.max_all_in_per_leg_usd = 0.85;
        QString error;
        auto executor = Executor::create(bad, &error);
        QVERIFY(!error.isEmpty());
        QCOMPARE(executor.snapshot().phase, Executor::Phase::HaltedUnsafe);
        QCOMPARE(executor.next_action().kind, Executor::ActionKind::None);
    }

    void inconsistent_venue_report_halts_unsafe() {
        auto executor = Executor::create(intent());
        const auto first = executor.next_action();
        auto bad = order(first, 3);
        QString error;
        QVERIFY(!executor.apply_order_report(bad, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(executor.snapshot().phase, Executor::Phase::HaltedUnsafe);
    }

    void indeterminate_submission_never_sends_a_dependent_leg() {
        auto executor = Executor::create(intent());
        const auto first = executor.next_action();
        Executor::OrderReport unknown;
        unknown.client_order_id = first.client_order_id;
        unknown.indeterminate = true;
        QString error;
        QVERIFY(!executor.apply_order_report(unknown, &error));
        QCOMPARE(executor.snapshot().phase, Executor::Phase::HaltedUnsafe);
        QCOMPARE(executor.next_action().kind, Executor::ActionKind::None);
        QVERIFY(error.contains(QStringLiteral("reconcile")));
    }

    void indeterminate_final_fill_after_cancel_halts_the_bundle() {
        auto executor = Executor::create(intent());
        const auto first = executor.next_action();
        QVERIFY(executor.apply_order_report(order(first, 1, false)));
        const auto cancel = executor.next_action();
        QString error;
        QVERIFY(!executor.apply_cancel_report(
            {cancel.order_id, true, true, 0, QStringLiteral("fresh order read timed out")},
            &error));
        QCOMPARE(executor.snapshot().phase, Executor::Phase::HaltedUnsafe);
        QCOMPARE(executor.next_action().kind, Executor::ActionKind::None);
        QVERIFY(error.contains(QStringLiteral("reconcile")));
    }

    void restart_restores_the_exact_partial_fill_protocol() {
        auto executor = Executor::create(intent());
        const auto first = executor.next_action();
        QVERIFY(executor.apply_order_report(order(first, 1, false)));
        QCOMPARE(executor.next_action().kind, Executor::ActionKind::Cancel);
        QString error;
        auto restored = Executor::from_json(executor.to_json(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(restored.snapshot().phase, Executor::Phase::AwaitFirstCancel);
        QCOMPARE(restored.snapshot().first_filled, 1);
        QCOMPARE(restored.next_action().kind, Executor::ActionKind::None);
    }

    void tiers_cannot_be_confused_by_changing_a_number() {
        auto over_micro = intent();
        over_micro.max_all_in_per_leg_usd = 20.0;
        QString error;
        auto executor = Executor::create(over_micro, &error);
        QVERIFY(error.contains(QStringLiteral("hard $2 per-leg")));
        QCOMPARE(executor.next_action().kind, Executor::ActionKind::None);

        auto production = intent();
        production.tier = Executor::ExecutionTier::ProductionLive;
        production.max_all_in_per_leg_usd = 20.0;
        error.clear();
        executor = Executor::create(production, &error);
        QVERIFY(error.contains(QStringLiteral("unavailable")));
        QCOMPARE(executor.next_action().kind, Executor::ActionKind::None);
    }
};

QTEST_APPLESS_MAIN(TstKalshiCorridorLiveExecutor)
#include "tst_kalshi_corridor_live_executor.moc"
