#include <QtTest>

#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "services/decision/DecisionOrchestrator.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QDir>
#include <QTemporaryDir>

using namespace openmarketterminal;
using namespace openmarketterminal::services::decision;

namespace {

DecisionSignal quantitative(const QString& name, qint64 as_of, double probability) {
    DecisionSignal signal;
    signal.name = name;
    signal.source = QStringLiteral("test-feed");
    signal.model_version = QStringLiteral("model-v1");
    signal.as_of_ms = as_of;
    signal.probability = probability;
    signal.confidence = 0.80;
    signal.calibration_score = 0.90;
    signal.sample_count = 100;
    signal.max_age_ms = 60'000;
    return signal;
}

DecisionRequest request(qint64 now = 1'800'000'000'000LL) {
    DecisionRequest out;
    out.decision_id = QStringLiteral("decision-1");
    out.decision_ts_ms = now;
    out.symbol = QStringLiteral("BTC-USD");
    out.horizon = QStringLiteral("1h");
    out.source = QStringLiteral("test-orchestrator");
    out.data_snapshot_id = QStringLiteral("snapshot-1");
    out.seconds_left = 1800;
    out.model_signals = {quantitative(QStringLiteral("chronos"), now - 1000, 0.70),
                         quantitative(QStringLiteral("microstructure"), now - 500, 0.66)};
    out.quote.venue = QStringLiteral("kalshi");
    out.quote.market_id = QStringLiteral("KXTEST");
    out.quote.side = QStringLiteral("yes");
    out.quote.executable = true;
    out.quote.observed_at_ms = now - 100;
    out.quote.bid = 0.54;
    out.quote.ask = 0.55;
    out.quote.available_size = 50.0;
    out.quote.fee_cost = 0.01;
    out.quote.slippage_cost = 0.005;
    out.quote.exit_cost_reserve = 0.005;
    out.limits.requested_notional = 2.0;
    out.limits.max_total_exposure = 100.0;
    out.limits.max_symbol_exposure = 50.0;
    out.limits.max_venue_exposure = 75.0;
    out.limits.max_daily_loss = 20.0;
    out.limits.max_open_positions = 10;
    out.limits.minimum_edge = 0.02;
    out.limits.minimum_confidence = 0.50;
    out.limits.minimum_liquidity = 2.0;
    out.limits.maximum_spread = 0.10;
    out.limits.maximum_quote_age_ms = 5000;
    out.limits.minimum_seconds_left = 60;
    return out;
}

} // namespace

class TstDecisionOrchestrator : public QObject {
    Q_OBJECT
  private:
    std::unique_ptr<QTemporaryDir> home_;

  private slots:
    void initTestCase() {
        home_ = std::make_unique<QTemporaryDir>();
        QVERIFY(home_->isValid());
        qputenv("HOME", home_->path().toUtf8());
        ProfileManager::instance().set_active(QStringLiteral("default"));
        QDir().mkpath(AppPaths::root());
        AppPaths::ensure_all();
        register_all_migrations();
        QVERIFY(Database::instance().open(AppPaths::data() + QStringLiteral("/openmarketterminal.db")).is_ok());
    }

    void cleanupTestCase() {
        Database::instance().close();
    }

    void deterministic_inputs_produce_identical_envelope_and_hash() {
        const auto first = DecisionOrchestrator::evaluate(request());
        const auto second = DecisionOrchestrator::evaluate(request());
        QCOMPARE(first.verdict, QStringLiteral("TRADE_CANDIDATE"));
        QCOMPARE(first.content_hash, second.content_hash);
        QCOMPARE(first.to_json(), second.to_json());
        QVERIFY(DecisionOrchestrator::validate(first).isEmpty());
    }

    void future_signal_is_audited_but_cannot_change_probability() {
        auto clean = request();
        const auto baseline = DecisionOrchestrator::evaluate(clean);
        clean.model_signals.append(quantitative(QStringLiteral("future"), clean.decision_ts_ms + 1, 0.99));
        const auto with_future = DecisionOrchestrator::evaluate(clean);
        QCOMPARE(with_future.model_probability, baseline.model_probability);
        QCOMPARE(with_future.edge_after_cost, baseline.edge_after_cost);
        QCOMPARE(with_future.rejected_signals.size(), 1);
        QCOMPARE(with_future.rejected_signals.first().name, QStringLiteral("future"));
    }

    void advisory_agent_is_recorded_but_excluded_from_probability_path() {
        auto baseline_request = request();
        const auto baseline = DecisionOrchestrator::evaluate(baseline_request);
        auto advisory = quantitative(QStringLiteral("llm-news"), baseline_request.decision_ts_ms - 1, 0.01);
        advisory.advisory_only = true;
        advisory.source = QStringLiteral("local-llm");
        baseline_request.model_signals.append(advisory);
        const auto result = DecisionOrchestrator::evaluate(baseline_request);
        QCOMPARE(result.model_probability, baseline.model_probability);
        QCOMPARE(result.accepted_signals.size(), 3);
    }

    void stale_or_future_quote_is_bad_price() {
        auto stale = request();
        stale.quote.observed_at_ms = stale.decision_ts_ms - 6000;
        QCOMPARE(DecisionOrchestrator::evaluate(stale).verdict, QStringLiteral("BAD_PRICE"));
        auto future = request();
        future.quote.observed_at_ms = future.decision_ts_ms + 1;
        const auto result = DecisionOrchestrator::evaluate(future);
        QCOMPARE(result.verdict, QStringLiteral("BAD_PRICE"));
        QVERIFY(result.risk_blockers.contains(QStringLiteral("quote is from the future")));
    }

    void portfolio_and_time_limits_block_otherwise_positive_edge() {
        auto exposure = request();
        exposure.portfolio.symbol_exposure = 49.0;
        QCOMPARE(DecisionOrchestrator::evaluate(exposure).verdict, QStringLiteral("NO_TRADE"));
        auto late = request();
        late.seconds_left = 30;
        QCOMPARE(DecisionOrchestrator::evaluate(late).verdict, QStringLiteral("TOO_LATE"));
    }

    void replay_is_chronological_and_does_not_rewrite_prior_decisions() {
        auto first = request();
        first.decision_id = QStringLiteral("first");
        auto second = request(first.decision_ts_ms + 60'000);
        second.decision_id = QStringLiteral("second");
        second.model_signals.first().probability = 0.20;
        const auto baseline = DecisionOrchestrator::replay({first});
        const auto replay = DecisionOrchestrator::replay({second, first});
        QCOMPARE(replay.frames, 2);
        QCOMPARE(replay.envelopes.first().decision_id, QStringLiteral("first"));
        QCOMPARE(replay.envelopes.first().content_hash, baseline.envelopes.first().content_hash);
        QCOMPARE(replay.envelopes.last().decision_id, QStringLiteral("second"));
    }

    void immutable_store_rejects_mutation_and_duplicate() {
        const auto envelope = DecisionOrchestrator::evaluate(request());
        QVERIFY(DecisionOrchestrator::persist_immutable(envelope, QStringLiteral("journal-1")).is_ok());
        QVERIFY(DecisionOrchestrator::persist_immutable(envelope, QStringLiteral("journal-1")).is_err());
        QVERIFY(Database::instance().execute(
            "UPDATE decision_envelopes SET verdict='NO_TRADE' WHERE id=?", {envelope.decision_id}).is_err());
        QVERIFY(Database::instance().execute(
            "DELETE FROM decision_envelopes WHERE id=?", {envelope.decision_id}).is_err());
        auto row = Database::instance().execute(
            "SELECT verdict,content_hash FROM decision_envelopes WHERE id=?", {envelope.decision_id});
        QVERIFY(row.is_ok());
        QVERIFY(row.value().next());
        QCOMPARE(row.value().value(0).toString(), QStringLiteral("TRADE_CANDIDATE"));
        QCOMPARE(row.value().value(1).toString(), envelope.content_hash);
    }
};

QTEST_GUILESS_MAIN(TstDecisionOrchestrator)
#include "tst_decision_orchestrator.moc"
